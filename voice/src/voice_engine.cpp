#include "voice_engine.h"

#include "tfar_hook.h"

#include "../../extension/src/env_mapping.h"
#include "../../extension/src/ir_mix.h"

#include <xmmintrin.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ar::voiceplugin
{
    namespace
    {
        constexpr int kGenPollMs = 100;
        constexpr float kGenMinIntervalSec = 0.15f;
        constexpr int64_t kMaxLead = 24000;   // a speaker this far ahead of the mix gets re-synced
        constexpr uint64_t kHeardWindowMs = 2000;
        constexpr uint64_t kSpeakerRelogMs = 60000;
        constexpr uint64_t kLinkGraceMs = 500;
        constexpr float kNoLink = -1.f; // recorded gain while there's no Arma link
        constexpr int kTfarTickPolls = 10; // look for / re-check TFAR every second

        short Saturate(float v)
        {
            return static_cast<short>(std::clamp(v, -32768.f, 32767.f));
        }

        std::mutex g_logMutex;
    }

    std::wstring LogPath()
    {
        wchar_t buf[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf) + L"\\Arma 3" : L".";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\AudioReverbVoice.log";
    }

    // Generator/init threads only (file I/O never happens on TeamSpeak's audio threads).
    void Log(const char* fmt, ...)
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        static FILE* f = nullptr;
        if (!f)
            f = _wfsopen(LogPath().c_str(), L"a", _SH_DENYWR);
        if (!f)
            return;
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fputc('\n', f);
        fflush(f);
    }

    VoiceEngine::VoiceEngine() = default;

    VoiceEngine::~VoiceEngine()
    {
        Stop();
    }

    void VoiceEngine::SetHostInfo(int apiVersion, const char* tsVersion)
    {
        m_apiVersion = apiVersion;
        strncpy_s(m_tsVersion, tsVersion ? tsVersion : "", _TRUNCATE);
    }

    void VoiceEngine::Start()
    {
        if (m_running.exchange(true))
            return;
        m_generator = std::thread([this] { GeneratorMain(); });
    }

    void VoiceEngine::Stop()
    {
        if (m_running.exchange(false) && m_generator.joinable())
            m_generator.join();
    }

    bool VoiceEngine::Active() const
    {
        return Current().enabled;
    }

    float VoiceEngine::SpeakerGain(const char* nickname) const
    {
        const Settings& s = Current();
        if (!s.enabled)
            return 0.f;
        for (uint32_t i = 0; i < s.speakerCount; ++i)
            if (strcmp(s.speakers[i].name, nickname) == 0)
                return s.speakers[i].gain * s.voiceGain;
        return 0.f;
    }

    void VoiceEngine::RecordHeard(uint16_t clientId, const char* nickname, float gain)
    {
        Heard* slot = nullptr;
        Heard* oldest = &m_heard[0];
        for (Heard& h : m_heard)
        {
            if (h.id.load(std::memory_order_relaxed) == clientId)
            {
                slot = &h;
                break;
            }
            if (h.tick.load(std::memory_order_relaxed) < oldest->tick.load(std::memory_order_relaxed))
                oldest = &h;
        }
        const bool fresh = !slot;
        if (fresh)
            slot = oldest;
        if (fresh || strcmp(slot->name, nickname) != 0)
        {
            slot->nameSeq.fetch_add(1, std::memory_order_acq_rel); // odd: writing
            strncpy_s(slot->name, nickname, _TRUNCATE);
            slot->nameSeq.fetch_add(1, std::memory_order_acq_rel);
            slot->id.store(clientId, std::memory_order_relaxed);
        }
        slot->gain.store(gain, std::memory_order_relaxed);
        slot->tick.store(GetTickCount64(), std::memory_order_relaxed);
    }

    void VoiceEngine::OnSpeakerVoice(uint16_t clientId, const char* nickname, const short* samples, int frames,
        int channels)
    {
        if (frames <= 0 || channels <= 0)
            return;
        const float gain = SpeakerGain(nickname);
        RecordHeard(clientId, nickname, Active() ? gain : kNoLink);
        if (gain <= 0.f)
            return;
        int64_t& wp = m_writePos[clientId];
        if (wp < m_readPos || wp > m_readPos + kMaxLead)
            wp = m_readPos; // started talking again (or drifted): align with the mix
        const float k = gain / (32768.f * static_cast<float>(channels));
        for (int f = 0; f < frames; ++f)
        {
            int sum = 0;
            for (int c = 0; c < channels; ++c)
                sum += samples[f * channels + c];
            m_ring[(wp + f) & kRingMask] += k * static_cast<float>(sum);
        }
        wp += frames;
    }

    void VoiceEngine::OnCaptured(const short* samples, int frames, int channels)
    {
        if (!m_talking || frames <= 0 || channels <= 0)
            return;
        const float selfGain = Current().selfGain;
        if (selfGain <= 0.f)
            return;
        const int64_t w = m_selfWrite.load(std::memory_order_relaxed);
        const int64_t r = m_selfRead.load(std::memory_order_acquire);
        const int64_t room = kSelfSize - (w - r);
        const int n = static_cast<int>(std::min<int64_t>(frames, room));
        const float k = selfGain / (32768.f * static_cast<float>(channels));
        for (int f = 0; f < n; ++f)
        {
            int sum = 0;
            for (int c = 0; c < channels; ++c)
                sum += samples[f * channels + c];
            m_self[(w + f) & (kSelfSize - 1)] = k * static_cast<float>(sum);
        }
        m_selfWrite.store(w + n, std::memory_order_release);
    }

    void VoiceEngine::OnMixed(short* samples, int frames, int channels, const unsigned int* channelSpeakers,
        unsigned int* channelFillMask)
    {
        frames = std::min(frames, 8192);
        const Settings& s = Current();

        // Gather this block: other speakers from the ring (cleared as consumed), own voice from the FIFO.
        for (int f = 0; f < frames; ++f)
        {
            float& slot = m_ring[(m_readPos + f) & kRingMask];
            m_in[f] = slot;
            slot = 0.f;
        }
        m_readPos += frames;
        {
            const int64_t r = m_selfRead.load(std::memory_order_relaxed);
            const int64_t w = m_selfWrite.load(std::memory_order_acquire);
            const int n = static_cast<int>(std::min<int64_t>(frames, w - r));
            for (int f = 0; f < n; ++f)
                m_in[f] += m_self[(r + f) & (kSelfSize - 1)];
            m_selfRead.store(r + n, std::memory_order_release);
        }
        if (!s.enabled)
            return;

        const unsigned int csr = _mm_getcsr();
        _mm_setcsr(csr | 0x8040); // flush denormals
        const float k = 1.f - std::clamp(s.inputDamp, 0.f, 0.99f);
        for (int f = 0; f < frames; ++f)
        {
            const float x = std::isfinite(m_in[f]) ? m_in[f] : 0.f;
            m_lp += k * (x - m_lp);
            m_in[f] = m_lp;
        }
        // Always run the engine (even on silence) so tails ring out and timing stays continuous.
        m_conv.Process(m_in, m_outL, m_outR, frames);
        _mm_setcsr(csr);

        float peak = 0;
        bool finite = true;
        for (int f = 0; f < frames; ++f)
        {
            finite = finite && std::isfinite(m_outL[f]) && std::isfinite(m_outR[f]);
            peak = std::max({peak, std::fabs(m_outL[f]), std::fabs(m_outR[f])});
        }
        if (!finite || peak > 16.f) // never add a non-finite/runaway signal to TeamSpeak's output
        {
            m_conv.Reset();
            m_lp = 0;
            m_resets.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (peak < 1e-6f)
            return;

        // Left/right output channels: front L/R in speaker mode, TeamSpeak's own headphone
        // channels in headphone mode; if neither is labelled, channels 0/1 like TFAR does.
        // A channel TeamSpeak hasn't filled yet holds undefined data, so it is zeroed before
        // adding and then marked filled.
        constexpr unsigned int kFrontLeft = 0x1, kFrontRight = 0x2;
        constexpr unsigned int kHeadphonesLeft = 0x10000000, kHeadphonesRight = 0x20000000;
        int left = -1, right = -1;
        for (int c = 0; c < channels && channelSpeakers; ++c)
        {
            const unsigned int sp = channelSpeakers[c];
            if ((sp == kFrontLeft || sp == kHeadphonesLeft) && left < 0)
                left = c;
            else if ((sp == kFrontRight || sp == kHeadphonesRight) && right < 0)
                right = c;
        }
        if (left < 0 && right < 0 && channels >= 2)
        {
            left = 0;
            right = 1;
        }
        if (channels == 1)
            left = 0;
        // Layout for the log (written here, logged by the generator thread when it changes).
        m_outLayout.store((static_cast<uint64_t>(channels) & 0xFF) | (static_cast<uint64_t>(left + 1) & 0xFF) << 8 |
                              (static_cast<uint64_t>(right + 1) & 0xFF) << 16 |
                              static_cast<uint64_t>(channelSpeakers && channels > 0 ? channelSpeakers[0] : 0) << 32,
            std::memory_order_relaxed);
        auto addTo = [&](int c, const float* a, const float* b) {
            if (c < 0)
                return;
            const unsigned int bit = 1u << c;
            if (channelFillMask && !(*channelFillMask & bit))
                for (int f = 0; f < frames; ++f)
                    samples[f * channels + c] = 0;
            for (int f = 0; f < frames; ++f)
            {
                const float v = b ? 0.5f * (a[f] + b[f]) : a[f];
                samples[f * channels + c] = Saturate(samples[f * channels + c] + v * 32768.f);
            }
            if (channelFillMask)
                *channelFillMask |= bit;
        };
        if (channels == 1)
            addTo(0, m_outL, m_outR);
        else
        {
            addTo(left, m_outL, nullptr);
            addTo(right, m_outR, nullptr);
        }
    }

    void VoiceEngine::GeneratorMain()
    {
        using clock = std::chrono::steady_clock;
        voice::Reader reader;
        voice::AckWriter ackWriter;
        static voice::Payload payload; // ~9 KB, one generator thread
        irmix::Cache cache;
        bus::Environment builtEnv;
        std::vector<irmix::Entry> builtMix;
        bool built = false;
        auto lastBuild = clock::now() - std::chrono::seconds(1);
        int next = 1;
        voice::LinkState loggedState = static_cast<voice::LinkState>(0xFFFFFFFFu);
        bool everConnected = false;
        uint64_t lastGoodRead = 0;
        uint64_t loggedLayout = 0;
        uint32_t loggedResets = 0;
        // Per TeamSpeak client: last logged match result and when.
        struct SpeakerLog
        {
            int matched = -1;
            uint64_t tick = 0;
        };
        std::map<uint32_t, SpeakerLog> speakerLog;
        int tfarPoll = 0;
        bool loggedEnhanced = true;
        uint32_t loggedRadioActive = 0;

        while (m_running)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(kGenPollMs));
            m_conv.DrainRetired();

            // --- TFAR radio effect hook -------------------------------------------------------
            if (tfarPoll-- <= 0)
            {
                tfarPoll = kTfarTickPolls;
                if (tfar::Tick())
                {
                    const tfar::Status st = tfar::GetStatus();
                    if (st.state == tfar::HookState::Hooked)
                        Log("radio: TFAR %s found, radio effect replaced (%s)", st.tfarVersion,
                            st.enhanced ? "enhanced sound on" : "switched off in Arma: TFAR's own sound");
                    else if (st.state == tfar::HookState::Unsupported)
                        Log("radio: TFAR %s found but not supported (%s); TFAR's own radio sound is used",
                            st.tfarVersion, st.detail.c_str());
                    else
                        Log("radio: TFAR plugin not loaded");
                }
            }

            // --- Link state -----------------------------------------------------------------
            // One failed read (Arma mid-update, or a scheduling hiccup) keeps the last good data;
            // the link only counts as lost after kLinkGraceMs without a good read.
            Settings& s = m_settings[next];
            bool ok = reader.Read(payload);
            if (ok)
                lastGoodRead = GetTickCount64();
            else if (lastGoodRead && GetTickCount64() - lastGoodRead < kLinkGraceMs)
                ok = true; // payload still holds the last good copy
            voice::LinkState state;
            if (ok)
                state = payload.enabled ? voice::LinkState::Connected : voice::LinkState::Disabled;
            else if (reader.OpenError() == ERROR_ACCESS_DENIED)
                state = voice::LinkState::AccessDenied;
            else if (reader.Exists() && everConnected)
                state = voice::LinkState::Stale;
            else
                state = voice::LinkState::WaitingForArma;
            everConnected = everConnected || ok;
            if (state != loggedState)
            {
                switch (state)
                {
                case voice::LinkState::AccessDenied:
                    Log("link: ACCESS DENIED opening the Arma link (Windows error %lu). Arma and TeamSpeak are running "
                        "with different privileges (one as administrator) and the Arma side is an older build.",
                        reader.OpenError());
                    break;
                case voice::LinkState::Connected:
                    Log("link: connected to Arma (%u player(s) in earshot, voice level %.2f, room send %.2f)",
                        payload.speakerCount, payload.voiceLevel, payload.env.send);
                    break;
                default:
                    Log("link: %s", voice::LinkStateName(state));
                    break;
                }
                loggedState = state;
            }

            if (ok)
                tfar::SetSettings(payload.radioEnhanced != 0, payload.radioStatic);

            s.enabled = state == voice::LinkState::Connected;
            if (s.enabled)
            {
                s.voiceGain = payload.voiceLevel * payload.env.send;
                s.selfGain = payload.selfLevel * payload.env.send;
                s.inputDamp = mapping::ToFdn(payload.env, kRate).inputDamp;
                s.speakerCount = std::min<uint32_t>(payload.speakerCount, voice::kMaxSpeakers);
                memcpy(s.speakers, payload.speakers, sizeof(voice::Speaker) * s.speakerCount);
            }
            m_settingsIndex.store(next, std::memory_order_release);
            next = (next + 1) % 3;

            // --- Who is talking, and did they match? (log + heartbeat) -----------------------
            voice::Ack ack{};
            ack.state = state;
            ack.openError = reader.OpenError();
            ack.apiVersion = m_apiVersion;
            strncpy_s(ack.tsVersion, m_tsVersion, _TRUNCATE);
            ack.resets = m_resets.load();
            const tfar::Status radio = tfar::GetStatus();
            ack.radioHook = static_cast<uint32_t>(radio.state);
            strncpy_s(ack.tfarVersion, radio.tfarVersion, _TRUNCATE);
            ack.radioEnhanced = radio.enhanced ? 1u : 0u;
            ack.radioActive = radio.active;
            ack.radioLoss = radio.signalLoss;
            if (radio.state == tfar::HookState::Hooked && radio.enhanced != loggedEnhanced)
                Log("radio: enhanced radio sound %s", radio.enhanced ? "on" : "off (TFAR's own sound)");
            loggedEnhanced = radio.enhanced;
            if (radio.active != loggedRadioActive && radio.state == tfar::HookState::Hooked && radio.active > 0)
                Log("radio: %u radio stream(s) processing, latest signal loss %.2f", radio.active, radio.signalLoss);
            loggedRadioActive = radio.active;
            const uint64_t now = GetTickCount64();
            for (Heard& h : m_heard)
            {
                const uint64_t tick = h.tick.load(std::memory_order_relaxed);
                if (tick == 0 || now - tick > kHeardWindowMs)
                    continue;
                char name[voice::kNameBytes];
                uint32_t seq;
                do
                {
                    seq = h.nameSeq.load(std::memory_order_acquire);
                    strncpy_s(name, h.name, _TRUNCATE);
                } while ((seq & 1) || seq != h.nameSeq.load(std::memory_order_acquire));
                const float gain = h.gain.load(std::memory_order_relaxed);
                const uint32_t id = h.id.load(std::memory_order_relaxed);
                ++ack.heard;
                if (gain > 0)
                {
                    ++ack.matched;
                    strncpy_s(ack.lastMatched, name, _TRUNCATE);
                }
                else if (gain == 0)
                {
                    strncpy_s(ack.lastUnmatched, name, _TRUNCATE);
                }

                SpeakerLog& sl = speakerLog[id];
                const int matched = gain > 0 ? 1 : gain == 0 ? 0 : 2; // 2: heard without an Arma link
                if (sl.matched != matched || now - sl.tick > kSpeakerRelogMs)
                {
                    if (matched == 1)
                    {
                        Log("speaker '%s' (TeamSpeak id %u): direct speech, room send %.2f", name, id, gain);
                    }
                    else if (matched == 0)
                    {
                        std::string sent;
                        for (uint32_t i = 0; i < payload.speakerCount; ++i)
                            sent += (i ? ", '" : "'") + std::string(payload.speakers[i].name) + "'";
                        Log("speaker '%s' (TeamSpeak id %u): no room reverb - not in direct-speech range per Arma "
                            "(Arma sent: %s)", name, id, sent.empty() ? "nobody" : sent.c_str());
                    }
                    else
                    {
                        Log("speaker '%s' (TeamSpeak id %u): heard, but no Arma link (%s)", name, id,
                            voice::LinkStateName(loggedState));
                    }
                    sl.matched = matched;
                    sl.tick = now;
                }
            }
            ackWriter.Publish(ack);
            const uint64_t layout = m_outLayout.load(std::memory_order_relaxed);
            if (layout != loggedLayout && layout != 0)
            {
                Log("output: TeamSpeak mixes %u channel(s), first speaker id 0x%08x; reverb goes to channel %d (L) / %d (R)",
                    static_cast<unsigned>(layout & 0xFF), static_cast<unsigned>(layout >> 32),
                    static_cast<int>((layout >> 8) & 0xFF) - 1, static_cast<int>((layout >> 16) & 0xFF) - 1);
                loggedLayout = layout;
            }
            if (ack.resets != loggedResets)
            {
                Log("voice reverb produced a non-finite/runaway signal and was reset (total %u)", ack.resets);
                loggedResets = ack.resets;
            }

            if (!s.enabled)
                continue;

            // Same room as the game: rebuild the IR when the environment or blend changes audibly.
            std::vector<irmix::Entry> mix;
            for (uint32_t i = 0; i < std::min<uint32_t>(payload.irCount, voice::kMaxIrs); ++i)
                mix.push_back({payload.irs[i].path, payload.irs[i].weight});
            const float since = std::chrono::duration<float>(clock::now() - lastBuild).count();
            if ((!built || mapping::IrDiffers(payload.env, builtEnv) || irmix::Differs(mix, builtMix)) &&
                since >= kGenMinIntervalSec)
            {
                std::string error;
                const ir::Stereo out = irmix::Build(payload.env, kRate, ConvEngine::kBlock, mix, cache, error);
                m_conv.Submit(ConvEngine::Build(out.l.data(), out.r.data(), static_cast<int>(out.l.size())));
                if (!error.empty())
                    Log("impulse response: %s", error.c_str());
                builtEnv = payload.env;
                builtMix = mix;
                built = true;
                lastBuild = clock::now();
            }
        }
    }
}
