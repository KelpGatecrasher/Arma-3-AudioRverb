// AudioReverb Voice: TeamSpeak 3 plugin adding room reverb to direct speech (TFAR/ACRE), driven
// by the AudioReverb Arma extension. See voice_engine.h for the signal flow.
//
// Built against the TeamSpeak 3 plugin SDK headers in ../ts3sdk (as used by TFAR).

#include "tfar_hook.h"
#include "voice_engine.h"

#include <windows.h>

#include "../ts3sdk/public_definitions.h"
#include "../ts3sdk/public_errors.h"
#include "../ts3sdk/plugin_definitions.h"
#include "../ts3sdk/ts3_functions.h"

#include <cstdio>
#include <cstring>
#include <memory>

#pragma comment(lib, "version.lib")

#define PLUGIN_API extern "C" __declspec(dllexport)

namespace
{
    TS3Functions g_ts{};
    std::unique_ptr<ar::voiceplugin::VoiceEngine> g_engine;

    // Nickname per (server, client), refreshed every couple of seconds: TFAR renames players
    // to their in-game name, so this is how a TeamSpeak voice is matched to an Arma unit.
    struct NickCache
    {
        uint64 server = 0;
        ULONGLONG tick = 0;
        char name[ar::voice::kNameBytes] = {};
    };
    NickCache g_nicks[65536];
    constexpr ULONGLONG kNickRefreshMs = 2000;

    const char* Nickname(uint64 server, anyID client)
    {
        NickCache& n = g_nicks[client];
        const ULONGLONG now = GetTickCount64();
        if (n.server != server || now - n.tick > kNickRefreshMs)
        {
            n.server = server;
            n.tick = now;
            n.name[0] = '\0';
            char* raw = nullptr;
            if (g_ts.getClientVariableAsString &&
                g_ts.getClientVariableAsString(server, client, CLIENT_NICKNAME, &raw) == ERROR_ok && raw)
            {
                ar::voice::NormalizeNickname(raw, n.name);
                g_ts.freeMemory(raw);
            }
        }
        return n.name;
    }

    void Log(const char* msg)
    {
        if (g_ts.logMessage)
            g_ts.logMessage(msg, LogLevel_INFO, "AudioReverb", 0);
    }
}

PLUGIN_API const char* ts3plugin_name() { return "AudioReverb Voice"; }
PLUGIN_API const char* ts3plugin_version() { return "0.3.0"; }
PLUGIN_API const char* ts3plugin_author() { return "AudioReverb"; }
PLUGIN_API const char* ts3plugin_description()
{
    return "Room reverb on direct speech for TFAR/ACRE, matching the AudioReverb Arma mod's environment, "
           "and an enhanced TFAR radio sound (handset filtering, ALC, hiss, fading and dropouts with signal loss).";
}

namespace
{
    // Host executable's file version (TeamSpeak's client version), 0.0.0 if unavailable.
    void HostVersion(int& major, int& minor, int& patch)
    {
        major = minor = patch = 0;
        wchar_t exe[MAX_PATH];
        const DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
            return;
        DWORD handle = 0;
        const DWORD size = GetFileVersionInfoSizeW(exe, &handle);
        if (size == 0)
            return;
        std::unique_ptr<BYTE[]> info(new BYTE[size]);
        VS_FIXEDFILEINFO* v = nullptr;
        UINT vlen = 0;
        if (!GetFileVersionInfoW(exe, 0, size, info.get()) ||
            !VerQueryValueW(info.get(), L"\\", reinterpret_cast<void**>(&v), &vlen) || !v)
            return;
        major = HIWORD(v->dwFileVersionMS);
        minor = LOWORD(v->dwFileVersionMS);
        patch = HIWORD(v->dwFileVersionLS);
    }
}

// Same client-version -> API-version mapping as TFAR (3.6+ requires 26).
PLUGIN_API int ts3plugin_apiVersion()
{
    int major, minor, patch;
    HostVersion(major, minor, patch);
    if (major != 3)
        return 26;
    switch (minor)
    {
    case 0: return patch < 14 ? 19 : patch < 20 ? 20 : 21;
    case 1:
    case 2: return 22;
    case 3:
    case 4:
    case 5: return 23;
    default: return 26;
    }
}

PLUGIN_API void ts3plugin_setFunctionPointers(const struct TS3Functions funcs) { g_ts = funcs; }

PLUGIN_API int ts3plugin_init()
{
    int major, minor, patch;
    HostVersion(major, minor, patch);
    char version[32];
    snprintf(version, sizeof(version), "%d.%d.%d", major, minor, patch);
    g_engine = std::make_unique<ar::voiceplugin::VoiceEngine>();
    g_engine->SetHostInfo(ts3plugin_apiVersion(), version);
    g_engine->Start();
    ar::voiceplugin::Log("AudioReverb Voice %s started in TeamSpeak %s (plugin API %d), waiting for the Arma mod",
        ts3plugin_version(), version, ts3plugin_apiVersion());
    Log("AudioReverb Voice started (waiting for the Arma mod)");
    return 0;
}

PLUGIN_API void ts3plugin_shutdown()
{
    if (g_engine)
        g_engine->Stop();
    g_engine.reset();
    ar::voiceplugin::tfar::Shutdown(); // give TFAR its own radio effect back before we unload
    ar::voiceplugin::Log("AudioReverb Voice stopped");
}

// Enabled by default once installed.
PLUGIN_API int ts3plugin_requestAutoload() { return 1; }

// Decoded voice of one speaker, before TFAR/ACRE process it. Only tapped, never modified.
PLUGIN_API void ts3plugin_onEditPlaybackVoiceDataEvent(uint64 server, anyID clientID, short* samples, int sampleCount,
    int channels)
{
    if (g_engine)
        g_engine->OnSpeakerVoice(clientID, Nickname(server, clientID), samples, sampleCount, channels);
}

// Final mix: add the room reverb.
PLUGIN_API void ts3plugin_onEditMixedPlaybackVoiceDataEvent(uint64, short* samples, int sampleCount, int channels,
    const unsigned int* channelSpeakerArray, unsigned int* channelFillMask)
{
    if (g_engine)
        g_engine->OnMixed(samples, sampleCount, channels, channelSpeakerArray, channelFillMask);
}

// Own microphone (optional self-reverb). Read only; *edited is left untouched.
PLUGIN_API void ts3plugin_onEditCapturedVoiceDataEvent(uint64, short* samples, int sampleCount, int channels, int*)
{
    if (g_engine)
        g_engine->OnCaptured(samples, sampleCount, channels);
}

// Gate own-voice reverb on TeamSpeak's own talk state, so mic noise is never reverbed.
PLUGIN_API void ts3plugin_onTalkStatusChangeEvent(uint64 server, int status, int, anyID clientID)
{
    if (!g_engine || !g_ts.getClientID)
        return;
    anyID me = 0;
    if (g_ts.getClientID(server, &me) == ERROR_ok && me == clientID)
        g_engine->SetTalking(status == STATUS_TALKING);
}
