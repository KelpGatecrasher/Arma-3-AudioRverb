#pragma once

#include <cstdint>

// Radio voice effect used in place of TFAR's own (see tfar_hook.h).
//
// TFAR's chain is a 90 Hz ring modulator plus foldback distortion that both grow with signal
// loss, then one high-pass and one low-pass biquad. This one models a real handset instead:
//
//   level normalise -> radio ALC (compressor) -> soft saturation -> [digital crunch]
//     -> fading / flutter / dropouts -> + band-limited hiss and crackle (FM-style: noise fills
//     the pauses on a good signal and swamps the voice on a bad one)
//     -> 4th-order channel band-pass -> small-speaker resonance -> 50 ms delay (as TFAR)
//
// Loudness follows TFAR: a replica of TFAR's own linear path for the radio type runs alongside,
// and the voice is matched to its long-term level, so radio volume knobs and per-radio gains
// behave exactly as before.
namespace ar::voiceplugin
{
    enum class RadioKind : uint8_t
    {
        Personal,  // TFAR "SW" handhelds (AN/PRC-152, 148, 343...)
        LongRange, // TFAR "LR" backpack and vehicle radios (also used for intercom)
        Airborne,  // TFAR airborne subtype
        Count
    };

    struct RadioSettings
    {
        float staticLevel = 1.f; // hiss/crackle amount, 0..2 (1 = default)
    };

    class RadioFx
    {
    public:
        RadioFx() { Reset(RadioKind::Personal); }
        void Reset(RadioKind kind);
        RadioKind Kind() const { return m_kind; }

        // Mono float block as TFAR hands it to its effect (already scaled by radio volume).
        // signalLoss: TFAR's raw error level, 0 = perfect signal .. 1 = edge of range.
        void Process(float* buffer, int samples, float signalLoss, const RadioSettings& settings);

    private:
        struct Biquad
        {
            float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
            float z1 = 0, z2 = 0;
            void HighPass(float hz, float q);
            void LowPass(float hz, float q);
            void Peak(float hz, float q, float db);
            float Run(float x)
            {
                const float y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                return y;
            }
            void Clear() { z1 = z2 = 0; }
        };

        float Noise(); // white, -1..1

        static constexpr int kDelay = 48000 / 20; // TFAR delays radio by 50 ms

        RadioKind m_kind = RadioKind::Personal;
        float m_drive = 1.8f;
        float m_noiseNorm = 1.f;

        Biquad m_hp[2], m_lp[2], m_peak;      // channel band-pass + speaker
        Biquad m_noiseHp, m_noiseLp;          // hiss colour
        Biquad m_refHp, m_refLp;              // TFAR's own filters (loudness reference)

        float m_level = 0;    // long-term input level (normaliser)
        float m_compEnv = 0;  // compressor envelope
        float m_refMs = 0;    // mean square of TFAR's path
        float m_outMs = 0;    // mean square of our voice path
        float m_match = 0;    // output gain matching m_refMs

        float m_fade = 0, m_fadeTarget = 0; // slow fading (random walk)
        int m_fadeHold = 0;
        float m_flutterPhase = 0;
        int m_dropLeft = 0;   // samples left in a dropout
        float m_dropGain = 1; // ramped voice gain around dropouts
        float m_crackle = 0;  // decaying crackle burst
        float m_hold = 0;     // sample-and-hold value (digital crunch)
        int m_holdCount = 0;

        uint32_t m_rng = 0x9E3779B9u;
        float m_delay[kDelay] = {};
        int m_delayPos = 0;
        float m_scratch[480]; // one TFAR block (Process works in 10 ms chunks)
    };
}
