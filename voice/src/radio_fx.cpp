#include "radio_fx.h"

#include <algorithm>
#include <cmath>

namespace ar::voiceplugin
{
    namespace
    {
        constexpr float kFs = 48000.f;
        constexpr float kPi = 3.14159265358979f;
        constexpr int kTfarBlock = 480; // TeamSpeak hands TFAR 10 ms blocks

        struct KindParams
        {
            float hp, lp;             // channel band edges
            float peakHz, peakDb;     // small-speaker resonance
            float drive;              // saturation
            float refHp, refHpQ, refLp, refLpQ; // TFAR's filters for this type (loudness reference)
            float noiseHp, noiseLp;   // hiss colour
        };

        // Bands are wider than TFAR's (more intelligible) but keep each type's character:
        // handhelds bright and clean, long range narrow and driven, airborne thin and sharp.
        constexpr KindParams kParams[] = {
            {300.f, 3400.f, 1800.f, 4.f, 1.8f, 900.f, 0.85f, 3000.f, 2.0f, 400.f, 5000.f},  // Personal
            {350.f, 2600.f, 1200.f, 5.f, 2.4f, 520.f, 0.97f, 1300.f, 1.0f, 300.f, 3500.f},  // LongRange
            {450.f, 3800.f, 2500.f, 4.f, 2.0f, 1000.f, 1.0f, 4000.f, 1.0f, 500.f, 6000.f}, // Airborne
        };

        constexpr float kCompThreshold = 0.5f; // normalised speech RMS is ~1
        constexpr float kCompSlope = 0.75f;    // 4:1
        const float kCompAttack = 1.f - std::exp(-1.f / (0.003f * kFs));
        const float kCompRelease = 1.f - std::exp(-1.f / (0.080f * kFs));
        constexpr float kSatBias = 0.08f;      // slight asymmetry (even harmonics)
    }

    void RadioFx::Biquad::HighPass(float hz, float q)
    {
        const float w = 2.f * kPi * hz / kFs, c = std::cos(w), alpha = std::sin(w) / (2.f * q);
        const float a0 = 1.f + alpha;
        b0 = (1.f + c) / 2.f / a0;
        b1 = -(1.f + c) / a0;
        b2 = b0;
        a1 = -2.f * c / a0;
        a2 = (1.f - alpha) / a0;
    }

    void RadioFx::Biquad::LowPass(float hz, float q)
    {
        const float w = 2.f * kPi * hz / kFs, c = std::cos(w), alpha = std::sin(w) / (2.f * q);
        const float a0 = 1.f + alpha;
        b0 = (1.f - c) / 2.f / a0;
        b1 = (1.f - c) / a0;
        b2 = b0;
        a1 = -2.f * c / a0;
        a2 = (1.f - alpha) / a0;
    }

    void RadioFx::Biquad::Peak(float hz, float q, float db)
    {
        const float A = std::pow(10.f, db / 40.f);
        const float w = 2.f * kPi * hz / kFs, c = std::cos(w), alpha = std::sin(w) / (2.f * q);
        const float a0 = 1.f + alpha / A;
        b0 = (1.f + alpha * A) / a0;
        b1 = -2.f * c / a0;
        b2 = (1.f - alpha * A) / a0;
        a1 = -2.f * c / a0;
        a2 = (1.f - alpha / A) / a0;
    }

    float RadioFx::Noise()
    {
        m_rng ^= m_rng << 13;
        m_rng ^= m_rng >> 17;
        m_rng ^= m_rng << 5;
        return static_cast<float>(static_cast<int32_t>(m_rng)) * (1.f / 2147483648.f);
    }

    void RadioFx::Reset(RadioKind kind)
    {
        const uint32_t seed = m_rng;
        m_kind = static_cast<int>(kind) < static_cast<int>(RadioKind::Count) ? kind : RadioKind::Personal;
        const KindParams& p = kParams[static_cast<int>(m_kind)];
        m_drive = p.drive;
        // 4th-order Butterworth band edges from two biquads each.
        m_hp[0].HighPass(p.hp, 0.5412f);
        m_hp[1].HighPass(p.hp, 1.3066f);
        m_lp[0].LowPass(p.lp, 0.5412f);
        m_lp[1].LowPass(p.lp, 1.3066f);
        m_peak.Peak(p.peakHz, 1.2f, p.peakDb);
        m_noiseHp.HighPass(p.noiseHp, 0.7071f);
        m_noiseLp.LowPass(p.noiseLp, 0.7071f);
        m_noiseNorm = std::sqrt(kFs * 0.5f / (p.noiseLp - p.noiseHp)); // white -> ~unit RMS in band
        m_refHp.HighPass(p.refHp, p.refHpQ);
        m_refLp.LowPass(p.refLp, p.refLpQ);
        for (Biquad* b : {&m_hp[0], &m_hp[1], &m_lp[0], &m_lp[1], &m_peak, &m_noiseHp, &m_noiseLp, &m_refHp, &m_refLp})
            b->Clear();
        m_level = m_compEnv = m_refMs = m_outMs = m_match = 0;
        m_fade = m_fadeTarget = 0;
        m_fadeHold = 0;
        m_flutterPhase = 0;
        m_dropLeft = 0;
        m_dropGain = 1;
        m_crackle = 0;
        m_hold = 0;
        m_holdCount = 0;
        m_rng = seed ? seed * 747796405u + 2891336453u : 0x9E3779B9u;
        std::fill(std::begin(m_delay), std::end(m_delay), 0.f);
        m_delayPos = 0;
    }

    void RadioFx::Process(float* buffer, int samples, float signalLoss, const RadioSettings& settings)
    {
        if (!buffer || samples <= 0)
            return;
        // Work in TFAR-sized chunks so per-block smoothing behaves the same for any block size.
        while (samples > kTfarBlock)
        {
            Process(buffer, kTfarBlock, signalLoss, settings);
            buffer += kTfarBlock;
            samples -= kTfarBlock;
        }

        const float e = std::isfinite(signalLoss) ? std::clamp(signalLoss, 0.f, 1.f) : 0.f;
        const float staticLevel = std::isfinite(settings.staticLevel) ? std::clamp(settings.staticLevel, 0.f, 2.f) : 1.f;
        const float blockScale = static_cast<float>(samples) / kTfarBlock;

        // --- Level normaliser + TFAR loudness reference --------------------------------------
        double ms = 0, refMs = 0;
        for (int i = 0; i < samples; ++i)
        {
            float x = buffer[i];
            if (!std::isfinite(x))
                x = buffer[i] = 0.f;
            ms += static_cast<double>(x) * x;
            const float r = m_refLp.Run(m_refHp.Run(30.f * x)); // TFAR: gain 30, then its two filters
            refMs += static_cast<double>(r) * r;
        }
        ms /= samples;
        refMs /= samples;
        const float rms = static_cast<float>(std::sqrt(ms));
        const bool speech = m_level <= 0.f ? rms > 1e-6f : rms > 0.3f * m_level;
        if (speech)
            m_level = m_level <= 0.f ? rms : m_level + std::min(1.f, (rms > m_level ? 0.25f : 0.05f) * blockScale) * (rms - m_level);
        const float inv = 1.f / std::max(m_level, 1e-6f);

        // --- Degradation for this block -------------------------------------------------------
        const float fadeDepth = 0.7f * e * std::sqrt(e);
        const float flutter = e > 0.35f ? (e - 0.35f) / 0.65f : 0.f;
        const float crunch = e > 0.55f ? (e - 0.55f) / 0.45f : 0.f;
        const int holdN = 1 + static_cast<int>(crunch * 3.f);
        const float quant = std::exp2(9.f - 5.f * crunch);
        if (e > 0.6f && m_dropLeft <= 0 && (Noise() * 0.5f + 0.5f) < 0.025f * blockScale * (e - 0.6f) / 0.4f)
            m_dropLeft = static_cast<int>(kFs * (0.03f + 0.12f * (Noise() * 0.5f + 0.5f)));
        const float crackleRate = staticLevel * e * e * 25.f / kFs; // events per sample
        const float hissBase = staticLevel * (0.03f + 0.55f * e * e);

        float* voice = m_scratch;
        double outMs = 0;
        for (int i = 0; i < samples; ++i)
        {
            // Radio ALC on the normalised voice.
            const float xn = buffer[i] * inv;
            const float a = std::fabs(xn);
            m_compEnv += (a > m_compEnv ? kCompAttack : kCompRelease) * (a - m_compEnv);
            const float g = m_compEnv > kCompThreshold ? std::pow(kCompThreshold / m_compEnv, kCompSlope) : 1.f;
            const float c = xn * g * 1.5f;

            // Soft saturation (transmitter / speaker overdrive), harder as the signal degrades.
            const float d = m_drive * (1.f + 0.6f * e);
            float y = std::tanh(d * (c + kSatBias)) - std::tanh(d * kSatBias);

            // Digital crunch near the edge of range: reduced rate and resolution.
            if (crunch > 0.f)
            {
                if (m_holdCount <= 0)
                {
                    m_hold = std::round(y * quant) / quant;
                    m_holdCount = holdN;
                }
                --m_holdCount;
                y += crunch * (m_hold - y);
            }

            y = m_peak.Run(m_lp[1].Run(m_lp[0].Run(m_hp[1].Run(m_hp[0].Run(y)))));
            outMs += static_cast<double>(y) * y;
            voice[i] = y;
        }
        outMs /= samples;

        // Match TFAR's long-term loudness (measured on speech only, before fading/dropouts).
        if (speech && outMs > 1e-12)
        {
            if (m_match <= 0.f)
            {
                m_refMs = static_cast<float>(refMs);
                m_outMs = static_cast<float>(outMs);
            }
            else
            {
                const float k = std::min(1.f, 0.02f * blockScale); // ~0.5 s
                m_refMs += k * (static_cast<float>(refMs) - m_refMs);
                m_outMs += k * (static_cast<float>(outMs) - m_outMs);
            }
            m_match = std::clamp(std::sqrt(m_refMs / std::max(m_outMs, 1e-12f)), 0.f, 1e4f);
        }
        const float voiceRms = std::sqrt(std::max(m_outMs, 0.f));

        for (int i = 0; i < samples; ++i)
        {
            // Slow fading (random walk) and multipath flutter.
            if (--m_fadeHold <= 0)
            {
                m_fadeTarget = Noise() * 0.5f + 0.5f;
                m_fadeHold = static_cast<int>(kFs * (0.08f + 0.17f * (Noise() * 0.5f + 0.5f)));
            }
            m_fade += 0.0005f * (m_fadeTarget - m_fade);
            m_flutterPhase += (11.f + 3.f * Noise()) / kFs;
            if (m_flutterPhase > 1.f)
                m_flutterPhase -= 1.f;
            const float flutterGain = 1.f - 0.35f * flutter * (0.5f + 0.5f * std::sin(2.f * kPi * m_flutterPhase));

            // Dropouts: voice ramps out (~1 ms) and the noise jumps.
            const bool dropping = m_dropLeft > 0;
            if (dropping)
                --m_dropLeft;
            m_dropGain += 0.02f * ((dropping ? 0.f : 1.f) - m_dropGain);

            const float v = voice[i] * (1.f - fadeDepth * m_fade) * flutterGain * m_dropGain;

            // Hiss: on a good signal it only fills the pauses (FM quieting); a bad one swamps the voice.
            const float presence = std::min(1.f, m_compEnv / kCompThreshold);
            const float quieting = 1.f - 0.85f * (1.f - e) * presence;
            if ((Noise() * 0.5f + 0.5f) < crackleRate * (dropping ? 4.f : 1.f))
                m_crackle = 1.5f + 2.f * (Noise() * 0.5f + 0.5f);
            m_crackle *= 0.97f;
            const float raw = Noise() * 1.7320508f + m_crackle * Noise(); // unit-RMS white + bursts
            const float hiss = m_noiseLp.Run(m_noiseHp.Run(raw)) * m_noiseNorm;
            const float n = hiss * hissBase * quieting * (dropping ? 1.8f : 1.f) * voiceRms;

            const float out = (v + n) * m_match;
            m_delay[m_delayPos] = out;
            m_delayPos = (m_delayPos + 1) % kDelay;
            buffer[i] = m_delay[m_delayPos];
        }

        bool finite = true;
        for (int i = 0; i < samples; ++i)
            finite = finite && std::isfinite(buffer[i]) && std::fabs(buffer[i]) < 64.f;
        if (!finite)
        {
            Reset(m_kind);
            std::fill(buffer, buffer + samples, 0.f);
        }
    }
}
