#pragma once

#include "../../extension/src/conv_engine.h"
#include "../../extension/src/voice_link.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

// Room reverb for TeamSpeak voices (TFAR/ACRE direct speech), driven by the Arma extension
// through voice_link.h.
//
// Each speaker's raw decoded voice is tapped before the radio mod processes it, scaled by that
// speaker's direct-speech gain from Arma, and summed into a mixing ring. The final-mix callback
// convolves the ring's next block with the listener's room (same IR blend as the game) and adds
// the result to TeamSpeak's output. Own voice (optional) arrives from the capture thread.
//
// Diagnostics: %LOCALAPPDATA%\Arma 3\AudioReverbVoice.log, plus a heartbeat block (voice::Ack)
// the Arma debug HUD reads.
namespace ar::voiceplugin
{
    void Log(const char* fmt, ...);
    std::wstring LogPath();

    class VoiceEngine
    {
    public:
        static constexpr float kRate = 48000.f; // TeamSpeak's mixing rate

        VoiceEngine();
        ~VoiceEngine();
        void Start();
        void Stop();
        void SetHostInfo(int apiVersion, const char* tsVersion);

        // Audio thread. Taps one speaker's decoded voice (never modifies it); the speaker's
        // gain comes from the latest Arma data by TeamSpeak nickname.
        void OnSpeakerVoice(uint16_t clientId, const char* nickname, const short* samples, int frames, int channels);
        void OnMixed(short* samples, int frames, int channels, const unsigned int* channelSpeakers,
            unsigned int* channelFillMask);

        // Capture thread (own microphone); only used while TeamSpeak reports us talking.
        void OnCaptured(const short* samples, int frames, int channels);
        void SetTalking(bool talking) { m_talking = talking; }

        bool Active() const;
        float SpeakerGain(const char* nickname) const;

    private:
        struct Settings
        {
            bool enabled = false;
            float voiceGain = 0;   // voiceLevel * environment send
            float selfGain = 0;    // selfLevel * environment send
            float inputDamp = 0;
            uint32_t speakerCount = 0;
            voice::Speaker speakers[voice::kMaxSpeakers];
        };

        // Recently heard speakers (audio thread writes, generator reads for log + heartbeat).
        struct Heard
        {
            std::atomic<uint32_t> id{0xFFFFFFFFu};
            std::atomic<uint64_t> tick{0};
            std::atomic<float> gain{0};
            std::atomic<uint32_t> nameSeq{0}; // seqlock for name
            char name[voice::kNameBytes] = {};
        };
        static constexpr int kHeardSlots = 64;

        void GeneratorMain();
        void RecordHeard(uint16_t clientId, const char* nickname, float gain);
        const Settings& Current() const { return m_settings[m_settingsIndex.load(std::memory_order_acquire)]; }

        // Settings: written by the generator into a spare slot, then published by index, so
        // the audio thread never blocks or sees a half-written snapshot.
        Settings m_settings[3];
        std::atomic<int> m_settingsIndex{0};

        ConvEngine m_conv;
        float m_lp = 0; // input HF damping state
        std::atomic<uint32_t> m_resets{0};
        std::atomic<uint64_t> m_outLayout{0}; // channels | L+1 << 8 | R+1 << 16 | speaker[0] << 32

        // Mixing ring for other speakers: absolute sample positions, index = pos & kRingMask.
        static constexpr int64_t kRingSize = 1 << 15; // ~0.68 s at 48 kHz
        static constexpr int64_t kRingMask = kRingSize - 1;
        float m_ring[kRingSize] = {};
        int64_t m_readPos = 0;
        int64_t m_writePos[65536] = {}; // per TeamSpeak client id

        // Own voice FIFO (single producer: capture thread, single consumer: mix thread).
        static constexpr int64_t kSelfSize = 1 << 14;
        float m_self[kSelfSize] = {};
        std::atomic<int64_t> m_selfWrite{0};
        std::atomic<int64_t> m_selfRead{0};
        std::atomic<bool> m_talking{false};

        Heard m_heard[kHeardSlots];
        int m_apiVersion = 0;
        char m_tsVersion[32] = {};

        std::atomic<bool> m_running{false};
        std::thread m_generator;
        float m_in[8192], m_outL[8192], m_outR[8192];
    };
}
