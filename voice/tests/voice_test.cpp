// Offline test of the TeamSpeak voice plugin: loads audioreverb_voice_win64.dll like TeamSpeak
// would, gives it stub TeamSpeak functions, publishes room data through the same shared memory
// the Arma extension writes, and drives the voice callbacks block by block.
// Exit code 0 = pass.

#include "../../extension/src/voice_link.h"

#include <windows.h>
#include <aclapi.h>

#include "../ts3sdk/public_definitions.h"
#include "../ts3sdk/public_errors.h"
#include "../ts3sdk/plugin_definitions.h"
#include "../ts3sdk/ts3_functions.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace
{
    int g_failures = 0;
    void Check(bool ok, const char* what)
    {
        printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok)
            ++g_failures;
    }
    void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

    // --- Stub TeamSpeak ------------------------------------------------------------------------
    std::map<int, std::string> g_names = {{1, "Me"}, {7, "Alice"}, {8, "Bob"}, {9, "Charlie"}, {10, "_Spacey_"}};
    unsigned int StubGetNick(uint64, anyID client, size_t flag, char** result)
    {
        if (flag != CLIENT_NICKNAME || !g_names.count(client))
            return ERROR_client_invalid_id;
        const std::string& n = g_names[client];
        *result = static_cast<char*>(malloc(n.size() + 1));
        memcpy(*result, n.c_str(), n.size() + 1);
        return ERROR_ok;
    }
    unsigned int StubFree(void* p) { free(p); return ERROR_ok; }
    unsigned int StubMyId(uint64, anyID* result) { *result = 1; return ERROR_ok; }
    unsigned int StubLog(const char* msg, enum LogLevel, const char* channel, uint64)
    {
        printf("    [ts log] %s: %s\n", channel, msg);
        return ERROR_ok;
    }

    // --- Plugin exports ------------------------------------------------------------------------
    using FnInit = int (*)();
    using FnVoid = void (*)();
    using FnApi = int (*)();
    using FnName = const char* (*)();
    using FnSetFuncs = void (*)(const struct TS3Functions);
    using FnPlayback = void (*)(uint64, anyID, short*, int, int);
    using FnMixed = void (*)(uint64, short*, int, int, const unsigned int*, unsigned int*);
    using FnCaptured = void (*)(uint64, short*, int, int, int*);
    using FnTalk = void (*)(uint64, int, int, anyID);

    FnPlayback g_playback;
    FnMixed g_mixed;
    FnCaptured g_captured;
    FnTalk g_talk;

    // --- Arma side ---------------------------------------------------------------------------
    ar::voice::Writer g_writer;
    ar::voice::Payload g_payload{};
    std::atomic<bool> g_publishing{false};
    std::thread g_publisher;

    void StartPublishing()
    {
        g_publishing = true;
        g_publisher = std::thread([] {
            while (g_publishing)
            {
                g_writer.Publish(g_payload);
                Sleep(50);
            }
        });
    }
    void StopPublishing()
    {
        g_publishing = false;
        if (g_publisher.joinable())
            g_publisher.join();
    }

    void SetSpeakers(std::initializer_list<std::pair<const char*, float>> list)
    {
        g_payload.speakerCount = 0;
        for (auto& [name, gain] : list)
        {
            ar::voice::Speaker& s = g_payload.speakers[g_payload.speakerCount++];
            ar::voice::NormalizeNickname(name, s.name);
            s.gain = gain;
        }
    }

    constexpr int kBlock = 480; // 10 ms at 48 kHz
    std::mt19937 g_rng(5);

    std::vector<short> Noise(int frames, int channels)
    {
        std::uniform_int_distribution<int> d(-12000, 12000);
        std::vector<short> v(static_cast<size_t>(frames) * channels);
        for (short& s : v)
            s = static_cast<short>(d(g_rng));
        return v;
    }

    // One mixed-callback block on a stereo device; returns RMS of what the plugin added.
    // TeamSpeak's output channel layout for the mixed callback (speaker vs headphone mode).
    unsigned int g_speakers[2] = {SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT};

    double MixBlock(unsigned int* fillMaskOut = nullptr, int channels = 2)
    {
        std::vector<short> out(static_cast<size_t>(kBlock) * channels, 0x5A5A); // garbage: channels "unfilled"
        unsigned int fill = 0;
        g_mixed(1, out.data(), kBlock, channels, g_speakers, &fill);
        if (fillMaskOut)
            *fillMaskOut = fill;
        if (fill == 0)
            return 0; // nothing written: TeamSpeak treats the (garbage) channels as empty
        double e = 0;
        for (short s : out)
            e += static_cast<double>(s) * s;
        return std::sqrt(e / out.size()) / 32768.0;
    }

    // `blocks` of one client talking, then `tailBlocks` of silence. Returns tail RMS.
    double TalkAndTail(anyID client, int blocks, int tailBlocks, bool* untouched = nullptr)
    {
        bool same = true;
        for (int b = 0; b < blocks; ++b)
        {
            std::vector<short> voice = Noise(kBlock, 1);
            const std::vector<short> copy = voice;
            g_playback(1, client, voice.data(), kBlock, 1);
            same = same && voice == copy;
            MixBlock();
        }
        if (untouched)
            *untouched = same;
        double tail = 0;
        for (int b = 0; b < tailBlocks; ++b)
            tail = std::max(tail, MixBlock());
        return tail;
    }

    void Silence(int blocks)
    {
        for (int b = 0; b < blocks; ++b)
            MixBlock();
    }
}

int main(int argc, char** argv)
{
    // Nickname normalisation (TFAR's rule).
    {
        char out[ar::voice::kNameBytes];
        ar::voice::NormalizeNickname("  Bob ", out);
        Check(strcmp(out, "__Bob_") == 0, "leading/trailing spaces become '_'");
        ar::voice::NormalizeNickname("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", out);
        Check(strlen(out) == 30, "nicknames cut to TeamSpeak's 30 characters");
        ar::voice::NormalizeNickname("\xC3\xA9tienne", out); // "Ã©tienne"
        Check(strcmp(out, "\xC3\xA9tienne") == 0, "multi-byte characters kept intact");
    }

    // Optional: the real TFAR plugin binary (argument 2 or %AR_TFAR_DLL%), loaded first like in
    // TeamSpeak, so the plugin's radio hook finds it.
    char tfarPath[MAX_PATH] = {};
    if (argc > 2)
        strncpy_s(tfarPath, argv[2], _TRUNCATE);
    else
        GetEnvironmentVariableA("AR_TFAR_DLL", tfarPath, MAX_PATH);
    const bool tfarLoaded = tfarPath[0] && LoadLibraryA(tfarPath) != nullptr;
    printf("    TFAR plugin binary: %s\n", tfarLoaded ? tfarPath : "not loaded (radio hook checks limited)");

    const char* dllPath = argc > 1 ? argv[1] : "audioreverb_voice_win64.dll";
    HMODULE dll = LoadLibraryA(dllPath);
    if (!dll)
    {
        printf("cannot load %s\n", dllPath);
        return 2;
    }
    auto name = reinterpret_cast<FnName>(GetProcAddress(dll, "ts3plugin_name"));
    auto api = reinterpret_cast<FnApi>(GetProcAddress(dll, "ts3plugin_apiVersion"));
    auto setFuncs = reinterpret_cast<FnSetFuncs>(GetProcAddress(dll, "ts3plugin_setFunctionPointers"));
    auto init = reinterpret_cast<FnInit>(GetProcAddress(dll, "ts3plugin_init"));
    auto shutdown = reinterpret_cast<FnVoid>(GetProcAddress(dll, "ts3plugin_shutdown"));
    g_playback = reinterpret_cast<FnPlayback>(GetProcAddress(dll, "ts3plugin_onEditPlaybackVoiceDataEvent"));
    g_mixed = reinterpret_cast<FnMixed>(GetProcAddress(dll, "ts3plugin_onEditMixedPlaybackVoiceDataEvent"));
    g_captured = reinterpret_cast<FnCaptured>(GetProcAddress(dll, "ts3plugin_onEditCapturedVoiceDataEvent"));
    g_talk = reinterpret_cast<FnTalk>(GetProcAddress(dll, "ts3plugin_onTalkStatusChangeEvent"));
    const char* required[] = {"ts3plugin_name", "ts3plugin_version", "ts3plugin_apiVersion", "ts3plugin_author",
        "ts3plugin_description", "ts3plugin_setFunctionPointers", "ts3plugin_init", "ts3plugin_shutdown"};
    bool allExports = true;
    for (const char* r : required)
        allExports = allExports && GetProcAddress(dll, r) != nullptr;
    Check(allExports && g_playback && g_mixed && g_captured && g_talk, "plugin exports the TeamSpeak entry points");
    printf("    %s, API version %d (test host has no TeamSpeak version info -> default)\n", name(), api());

    TS3Functions funcs{};
    funcs.getClientVariableAsString = StubGetNick;
    funcs.freeMemory = StubFree;
    funcs.getClientID = StubMyId;
    funcs.logMessage = StubLog;
    setFuncs(funcs);
    Check(init() == 0, "ts3plugin_init");

    // Before Arma publishes anything: no reverb.
    {
        const double tail = TalkAndTail(7, 10, 20);
        Check(tail == 0, "no reverb without the Arma mod running");
    }

    // A medium room (synthesized IR), voice level 1, environment send 1.
    g_payload.enabled = 1;
    g_payload.voiceLevel = 1.f;
    g_payload.selfLevel = 0.f;
    g_payload.env.room = 0;
    g_payload.env.roomHF = -600;
    g_payload.env.decayTime = 1.3f;
    g_payload.env.decayHFRatio = 0.83f;
    g_payload.env.reflections = -1000;
    g_payload.env.reflectionsDelay = 0.01f;
    g_payload.env.reverb = -200;
    g_payload.env.reverbDelay = 0.02f;
    g_payload.env.send = 1.f;
    g_payload.irCount = 0;
    g_payload.radioEnhanced = 1;
    g_payload.radioStatic = 1.f;
    SetSpeakers({{"Alice", 1.f}, {"Bob", 0.f}, {" Spacey ", 1.f}});
    StartPublishing();
    Sleep(600); // plugin generator reads the link and builds the IR

    bool untouched = false;
    const double alice = TalkAndTail(7, 30, 40, &untouched);
    printf("    Alice (gain 1) tail RMS %.5f\n", alice);
    Check(alice > 1e-3, "direct speech gets room reverb");
    Check(untouched, "speaker voice data is never modified (TFAR/ACRE see the original)");

    // Shared memory carries a low mandatory label, so a non-elevated TeamSpeak can open what an
    // elevated Arma created (the likely cause of "voice reverb does nothing" on a friend's PC).
    {
        bool lowLabel = false;
        HANDLE m = OpenFileMappingW(READ_CONTROL, FALSE, ar::voice::kMappingName);
        PSECURITY_DESCRIPTOR sd = nullptr;
        PACL sacl = nullptr;
        if (m && GetSecurityInfo(m, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, &sacl, &sd) ==
                     ERROR_SUCCESS && sacl)
        {
            for (DWORD i = 0; i < sacl->AceCount; ++i)
            {
                ACE_HEADER* ace = nullptr;
                if (GetAce(sacl, i, reinterpret_cast<void**>(&ace)) && ace->AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE)
                {
                    PSID sid = &reinterpret_cast<SYSTEM_MANDATORY_LABEL_ACE*>(ace)->SidStart;
                    lowLabel = *GetSidSubAuthority(sid, 0) == SECURITY_MANDATORY_LOW_RID;
                }
            }
        }
        if (sd)
            LocalFree(sd);
        if (m)
            CloseHandle(m);
        Check(lowLabel, "Arma link is openable across admin/non-admin (low integrity label)");
    }

    // Heartbeat back to Arma: connected, Alice heard and matched.
    {
        ar::voice::AckReader ackReader;
        ar::voice::Ack ack{};
        std::vector<short> voice = Noise(kBlock, 1);
        g_playback(1, 7, voice.data(), kBlock, 1);
        MixBlock();
        Sleep(300);
        const bool ok = ackReader.Read(ack);
        printf("    heartbeat: read %d, state '%s', TS '%s', heard %u, matched %u, last matched '%s'\n", ok,
            ar::voice::LinkStateName(ack.state), ack.tsVersion, ack.heard, ack.matched, ack.lastMatched);
        Check(ok && ack.state == ar::voice::LinkState::Connected && ack.matched >= 1 && strcmp(ack.lastMatched, "Alice") == 0,
            "plugin heartbeat reports the link and who it hears");
        printf("    radio: hook state %u, TFAR '%s', enhanced %u\n", ack.radioHook, ack.tfarVersion, ack.radioEnhanced);
        Check(ack.radioEnhanced == 1 && ack.radioHook == (tfarLoaded ? 1u : 0u),
            tfarLoaded ? "heartbeat: TFAR radio effect hooked, enhanced on" : "heartbeat: no TFAR loaded, radio setting received");

        g_payload.radioEnhanced = 0;
        Sleep(1200);
        ackReader.Read(ack);
        Check(ack.radioEnhanced == 0, "radio enhancement switched off from Arma");
        g_payload.radioEnhanced = 1;
        Sleep(300);
    }

    Silence(400);
    const double bob = TalkAndTail(8, 30, 40);
    printf("    Bob (gain 0, e.g. far away on radio) tail RMS %.6f\n", bob);
    Check(bob < 1e-5, "speaker with zero direct-speech gain stays dry");
    {
        ar::voice::AckReader ackReader;
        ar::voice::Ack ack{};
        Sleep(300);
        ackReader.Read(ack);
        Check(strcmp(ack.lastUnmatched, "Bob") == 0, "heartbeat names a speaker that got no room reverb");
    }

    Silence(400);
    const double charlie = TalkAndTail(9, 30, 40);
    Check(charlie < 1e-5, "speaker unknown to Arma stays dry");

    Silence(400);
    const double spacey = TalkAndTail(10, 30, 40);
    Check(spacey > 1e-3, "in-game name with spaces matches TFAR's renamed nickname");

    // Fill mask handling: channels marked filled, garbage replaced.
    {
        Silence(400);
        std::vector<short> voice = Noise(kBlock, 1);
        g_playback(1, 7, voice.data(), kBlock, 1);
        unsigned int fill = 0;
        MixBlock(&fill);
        double tail = 0;
        unsigned int tailFill = 0;
        for (int b = 0; b < 10; ++b)
            tail = std::max(tail, MixBlock(&tailFill));
        printf("    fill mask after reverb: 0x%x, RMS %.5f\n", tailFill, tail);
        Check(tailFill == 0x3 && tail < 0.5, "front L/R marked filled and unfilled garbage cleared");
    }

    // TeamSpeak in headphone mode labels its channels SPEAKER_HEADPHONES_*; an unlabelled layout
    // falls back to channels 0/1 like TFAR. (Headphone mode was why voice reverb was silent on
    // a tester's PC while the plugin log showed it working.)
    {
        const unsigned int layouts[2][2] = {{SPEAKER_HEADPHONES_LEFT, SPEAKER_HEADPHONES_RIGHT}, {0, 0}};
        const char* names[2] = {"headphone-mode output (SPEAKER_HEADPHONES_*)", "unlabelled output channels (channels 0/1)"};
        for (int i = 0; i < 2; ++i)
        {
            g_speakers[0] = layouts[i][0];
            g_speakers[1] = layouts[i][1];
            Silence(400);
            unsigned int fill = 0;
            double tail = 0;
            for (int b = 0; b < 30; ++b)
            {
                std::vector<short> voice = Noise(kBlock, 1);
                g_playback(1, 7, voice.data(), kBlock, 1);
                tail = std::max(tail, MixBlock(&fill));
            }
            printf("    %s: RMS %.5f, fill 0x%x\n", names[i], tail, fill);
            Check(tail > 1e-3 && fill == 0x3, names[i]);
        }
        g_speakers[0] = SPEAKER_FRONT_LEFT;
        g_speakers[1] = SPEAKER_FRONT_RIGHT;
    }

    // Mono output device.
    {
        Silence(400);
        std::vector<short> voice = Noise(kBlock, 1);
        g_playback(1, 7, voice.data(), kBlock, 1);
        double mono = 0;
        for (int b = 0; b < 20; ++b)
            mono = std::max(mono, MixBlock(nullptr, 1));
        Check(mono > 1e-4, "mono output device");
    }

    // Own voice: off by default, on with selfLevel while TeamSpeak reports us talking.
    {
        Silence(400);
        std::vector<short> mic = Noise(kBlock, 1);
        g_talk(1, STATUS_TALKING, 0, 1);
        for (int b = 0; b < 20; ++b)
        {
            g_captured(1, mic.data(), kBlock, 1, nullptr);
            MixBlock();
        }
        double off = 0;
        for (int b = 0; b < 20; ++b)
            off = std::max(off, MixBlock());
        Check(off < 1e-5, "own voice not reverbed when the option is off");

        g_payload.selfLevel = 1.f;
        Sleep(300);
        for (int b = 0; b < 20; ++b)
        {
            g_captured(1, mic.data(), kBlock, 1, nullptr);
            MixBlock();
        }
        double on = 0;
        for (int b = 0; b < 20; ++b)
            on = std::max(on, MixBlock());
        printf("    own voice tail RMS %.5f\n", on);
        Check(on > 1e-3, "own voice reverbed with the option on");

        Silence(400);
        g_talk(1, STATUS_NOT_TALKING, 0, 1);
        for (int b = 0; b < 20; ++b)
        {
            g_captured(1, mic.data(), kBlock, 1, nullptr);
            MixBlock();
        }
        double notTalking = 0;
        for (int b = 0; b < 20; ++b)
            notTalking = std::max(notTalking, MixBlock());
        Check(notTalking < 1e-5, "mic ignored while not talking (no noise reverb)");
        g_payload.selfLevel = 0.f;
    }

    // Recorded OpenAIR blend, like the game.
    {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring dir(self);
        dir = dir.substr(0, dir.find_last_of(L"\\/")) + L"\\..\\..\\..\\irs\\openair\\";
        g_payload.irCount = 2;
        wcsncpy_s(g_payload.irs[0].path, (dir + L"indoormedium_typing.wav").c_str(), _TRUNCATE);
        g_payload.irs[0].weight = 0.7f;
        wcsncpy_s(g_payload.irs[1].path, (dir + L"indoorsmall_stairway.wav").c_str(), _TRUNCATE);
        g_payload.irs[1].weight = 0.3f;
        Sleep(800);
        Silence(400);
        const double blend = TalkAndTail(7, 30, 40);
        printf("    Alice with OpenAIR blend tail RMS %.5f\n", blend);
        Check(blend > 1e-3, "voice reverb with the recorded IR blend");
    }

    // Arma stops publishing (closed/crashed): plugin goes quiet after the stale timeout.
    {
        StopPublishing();
        Sleep(static_cast<int>(ar::voice::kStaleMs) + 500);
        Silence(400);
        const double stale = TalkAndTail(7, 30, 40);
        Check(stale < 1e-5, "no reverb once the Arma link goes stale");
        ar::voice::AckReader ackReader;
        ar::voice::Ack ack{};
        Sleep(300);
        ackReader.Read(ack);
        Check(ack.state == ar::voice::LinkState::Stale, "heartbeat reports the stale link");
    }

    // Plugin log (what a user sends back when something doesn't work).
    {
        wchar_t buf[MAX_PATH];
        GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        std::wifstream f(std::wstring(buf) + L"\\Arma 3\\AudioReverbVoice.log");
        std::wstringstream ss;
        ss << f.rdbuf();
        const std::wstring log = ss.str();
        const bool started = log.find(L"AudioReverb Voice 0.3.0 started") != std::wstring::npos;
        const bool connected = log.find(L"link: connected to Arma") != std::wstring::npos;
        const bool aliceLine = log.find(L"speaker 'Alice'") != std::wstring::npos;
        const bool bobLine = log.find(L"speaker 'Bob'") != std::wstring::npos && log.find(L"Arma sent:") != std::wstring::npos;
        printf("    plugin log: started %d, connected %d, Alice %d, Bob unmatched %d\n", started, connected, aliceLine, bobLine);
        Check(started && connected && aliceLine && bobLine, "plugin log records start, link and per-speaker results");
        if (tfarLoaded)
            Check(log.find(L"radio: TFAR 1.") != std::wstring::npos && log.find(L"radio effect replaced") != std::wstring::npos,
                "plugin log records the TFAR radio hook");
    }

    shutdown();
    FreeLibrary(dll);
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
