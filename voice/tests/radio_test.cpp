// Offline test for the enhanced TFAR radio effect.
//
//   radio_test [path\to\TFAR_win64.dll]
//
// Part 1 checks RadioFx itself on a synthetic voice (loudness parity with TFAR's own linear
// path, volume-knob linearity, band, hiss behaviour, garbage input). Part 2 loads the real TFAR
// TeamSpeak plugin binary (when given, or %AR_TFAR_DLL%), installs the vtable hooks and drives a
// stand-in effect object through the patched vtable - including TFAR's own setErrorLeveL and
// process - to prove the slots are what tfar_hook.h assumes.

#include "../src/radio_fx.h"
#include "../src/tfar_hook.h"
#include "../../extension/src/memscan.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace ar::voiceplugin;

namespace
{
    int g_failures = 0;
    constexpr float kFs = 48000.f;
    constexpr float kPi = 3.14159265358979f;
    constexpr int kBlock = 480;

    void Check(bool ok, const char* what)
    {
        printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok)
            ++g_failures;
    }

    // Voice-like test signal: a 120 Hz glottal pulse train through three formants, in
    // syllables (~4 per second) with a pause every second. RMS ~0.03 while talking, like a
    // TFAR radio buffer at moderate volume.
    std::vector<float> Voice(float seconds, float level = 1.f)
    {
        const int n = static_cast<int>(seconds * kFs);
        std::vector<float> out(n);
        struct Res
        {
            float a1, a2, g, y1 = 0, y2 = 0;
        };
        auto make = [](float hz, float bw) {
            const float r = std::exp(-kPi * bw / kFs);
            return Res{-2.f * r * std::cos(2.f * kPi * hz / kFs), r * r, 1.f - r};
        };
        Res f[3] = {make(700, 110), make(1200, 120), make(2600, 160)};
        float phase = 0;
        uint32_t rng = 12345;
        for (int i = 0; i < n; ++i)
        {
            const float t = i / kFs;
            const float pitch = 120.f + 15.f * std::sin(2.f * kPi * 0.7f * t);
            phase += pitch / kFs;
            float src = 0;
            if (phase >= 1.f)
            {
                phase -= 1.f;
                src = 1.f;
            }
            rng = rng * 1664525u + 1013904223u;
            src += 0.02f * (static_cast<float>(rng >> 8) / 16777216.f - 0.5f);
            float s = 0;
            for (Res& r : f)
            {
                const float y = r.g * src - r.a1 * r.y1 - r.a2 * r.y2;
                r.y2 = r.y1;
                r.y1 = y;
                s += y;
            }
            const float syll = std::fmod(t, 1.f) < 0.75f ? std::pow(std::sin(kPi * std::fmod(t * 4.f, 1.f)), 2.f) : 0.f;
            out[i] = s * syll;
        }
        double ms = 0;
        int talk = 0;
        for (int i = 0; i < n; ++i)
            if (std::fmod(i / kFs, 1.f) < 0.75f)
            {
                ms += out[i] * out[i];
                ++talk;
            }
        const float k = 0.03f * level / static_cast<float>(std::sqrt(ms / std::max(talk, 1)));
        for (float& v : out)
            v *= k;
        return out;
    }

    // TFAR's linear path at full signal: x30, then its RBJ high-pass and low-pass.
    struct Rbj
    {
        float b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
        static Rbj Make(bool high, float hz, float q)
        {
            const float w = 2.f * kPi * hz / kFs, c = std::cos(w), al = std::sin(w) / (2.f * q), a0 = 1.f + al;
            Rbj r{};
            r.b0 = (high ? (1.f + c) : (1.f - c)) / 2.f / a0;
            r.b1 = (high ? -(1.f + c) : (1.f - c)) / a0;
            r.b2 = r.b0;
            r.a1 = -2.f * c / a0;
            r.a2 = (1.f - al) / a0;
            return r;
        }
        float Run(float x)
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    std::vector<float> Run(RadioFx& fx, std::vector<float> x, float loss, float staticLevel)
    {
        RadioSettings s;
        s.staticLevel = staticLevel;
        for (size_t i = 0; i + kBlock <= x.size(); i += kBlock)
            fx.Process(x.data() + i, kBlock, loss, s);
        return x;
    }

    // RMS over [from, to) seconds, only where talking==true/false (output is delayed 50 ms).
    float Rms(const std::vector<float>& x, float from, float to, int talking)
    {
        double ms = 0;
        int n = 0;
        for (int i = static_cast<int>(from * kFs); i < static_cast<int>(to * kFs) && i < static_cast<int>(x.size()); ++i)
        {
            const float t = std::fmod(i / kFs - 0.05f, 1.f);
            const bool talk = t >= 0.05f && t < 0.7f;
            const bool pause = t >= 0.8f && t < 0.95f;
            if ((talking == 1 && !talk) || (talking == 0 && !pause))
                continue;
            ms += x[i] * x[i];
            ++n;
        }
        return n ? static_cast<float>(std::sqrt(ms / n)) : 0.f;
    }

    float Db(float a, float b) { return 20.f * std::log10(std::max(a, 1e-12f) / std::max(b, 1e-12f)); }

    float BandRms(const std::vector<float>& x, float lo, float hi)
    {
        Rbj h1 = Rbj::Make(true, lo, 0.7071f), h2 = Rbj::Make(true, lo, 0.7071f);
        Rbj l1 = Rbj::Make(false, hi, 0.7071f), l2 = Rbj::Make(false, hi, 0.7071f);
        double ms = 0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const float y = l2.Run(l1.Run(h2.Run(h1.Run(x[i]))));
            if (i > x.size() / 4)
                ms += y * y;
        }
        return static_cast<float>(std::sqrt(ms / (x.size() * 3 / 4)));
    }

    void TestDsp()
    {
        printf("-- RadioFx --\n");
        const std::vector<float> voice = Voice(8.f);
        const char* names[] = {"personal (SW)", "long range (LR)", "airborne"};
        const float ref[][4] = {{900, 0.85f, 3000, 2.0f}, {520, 0.97f, 1300, 1.0f}, {1000, 1.0f, 4000, 1.0f}};
        for (int k = 0; k < 3; ++k)
        {
            RadioFx fx;
            fx.Reset(static_cast<RadioKind>(k));
            const std::vector<float> out = Run(fx, voice, 0.f, 1.f);
            Rbj hp = Rbj::Make(true, ref[k][0], ref[k][1]), lp = Rbj::Make(false, ref[k][2], ref[k][3]);
            std::vector<float> tfar(voice.size());
            for (size_t i = 0; i < voice.size(); ++i)
                tfar[i] = i >= 2400 ? lp.Run(hp.Run(30.f * voice[i - 2400])) : 0.f; // TFAR delays 50 ms too
            const float diff = Db(Rms(out, 2.f, 8.f, 1), Rms(tfar, 2.f, 8.f, 1));
            char what[160];
            snprintf(what, sizeof(what), "%s: speech loudness matches TFAR's (%.2f dB)", names[k], diff);
            Check(std::fabs(diff) < 2.f, what);
        }

        {
            RadioFx a, b;
            const float full = Rms(Run(a, voice, 0.f, 1.f), 2.f, 8.f, 1);
            const float half = Rms(Run(b, Voice(8.f, 0.5f), 0.f, 1.f), 2.f, 8.f, 1);
            char what[160];
            snprintf(what, sizeof(what), "radio volume still scales the output (half input -> %.2f dB)", Db(half, full));
            Check(std::fabs(Db(half, full) + 6.02f) < 1.f, what);
        }

        {
            RadioFx fx;
            const std::vector<float> out = Run(fx, voice, 0.f, 1.f);
            const float mid = BandRms(out, 800, 3000), low = BandRms(out, 20, 150), high = BandRms(out, 6000, 20000);
            char what[160];
            snprintf(what, sizeof(what), "handset band: <150 Hz %.1f dB, >6 kHz %.1f dB vs. speech band", Db(low, mid), Db(high, mid));
            Check(Db(low, mid) < -20.f && Db(high, mid) < -20.f, what);
        }

        {
            RadioFx clean, edge, quiet;
            const std::vector<float> c = Run(clean, voice, 0.f, 1.f);
            const std::vector<float> e = Run(edge, voice, 0.9f, 1.f);
            const std::vector<float> q = Run(quiet, voice, 0.9f, 0.f);
            const float cleanHiss = Db(Rms(c, 2.f, 8.f, 0), Rms(c, 2.f, 8.f, 1));
            const float edgeHiss = Db(Rms(e, 2.f, 8.f, 0), Rms(c, 2.f, 8.f, 1));
            const float quietHiss = Db(Rms(q, 2.f, 8.f, 0), Rms(c, 2.f, 8.f, 1));
            char what[160];
            snprintf(what, sizeof(what), "full signal: faint carrier hiss in pauses (%.1f dB under speech)", -cleanHiss);
            Check(cleanHiss < -24.f && cleanHiss > -60.f, what);
            snprintf(what, sizeof(what), "edge of range: loud static (%.1f dB vs. speech)", edgeHiss);
            Check(edgeHiss > -14.f, what);
            snprintf(what, sizeof(what), "static setting 0 removes the hiss (%.1f dB)", quietHiss);
            Check(quietHiss < -50.f, what);
            const float edgeVoice = Rms(e, 2.f, 8.f, 1);
            Check(std::isfinite(edgeVoice) && edgeVoice > 0.f, "degraded signal still carries the voice");
        }

        {
            RadioFx fx;
            std::vector<float> junk = voice;
            junk.resize(48000);
            for (size_t i = 0; i < junk.size(); i += 97)
                junk[i] = (i / 97) % 3 == 0 ? NAN : (i / 97) % 3 == 1 ? INFINITY : 1e30f;
            const std::vector<float> out = Run(fx, junk, NAN, NAN);
            bool finite = true;
            for (float v : out)
                finite = finite && std::isfinite(v) && std::fabs(v) < 64.f;
            Check(finite, "NaN/Inf/huge input and settings never produce a non-finite output");
            const std::vector<float> after = Run(fx, voice, 0.f, 1.f);
            Check(Rms(after, 2.f, 8.f, 1) > 1e-3f, "effect recovers after garbage");
        }

        {
            RadioFx fx;
            std::vector<float> odd = voice;
            RadioSettings s;
            size_t i = 0;
            int sizes[] = {1, 7, 480, 960, 333, 2048};
            for (int j = 0; i < odd.size(); ++j)
            {
                const int n = static_cast<int>(std::min<size_t>(sizes[j % 6], odd.size() - i));
                fx.Process(odd.data() + i, n, 0.2f, s);
                i += n;
            }
            Check(Rms(odd, 2.f, 8.f, 1) > 1e-3f, "any block size works");
        }

        {
            RadioFx fx;
            std::vector<float> x = Voice(60.f);
            LARGE_INTEGER f, a, b;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&a);
            Run(fx, x, 0.7f, 1.f);
            QueryPerformanceCounter(&b);
            const double sec = static_cast<double>(b.QuadPart - a.QuadPart) / f.QuadPart;
            char what[160];
            snprintf(what, sizeof(what), "cost: %.2f%% of one core per radio stream", 100.0 * sec / 60.0);
            Check(sec / 60.0 < 0.02, what);
        }
    }

    void TestHook(const char* path)
    {
        printf("-- TFAR hook (%s) --\n", path);
        wchar_t wpath[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
        HMODULE mod = LoadLibraryW(wpath);
        Check(mod != nullptr, "TFAR plugin binary loads");
        if (!mod)
        {
            printf("    LoadLibrary error %lu\n", GetLastError());
            return;
        }

        const uintptr_t vt = ar::mem::FindRttiVtable(mod, ".?AVPersonalRadioEffect@@", 0);
        uint64_t origProcess = 0, origSetError = 0;
        ar::mem::SafeRead64(vt, origProcess);
        ar::mem::SafeRead64(vt + 8, origSetError);

        std::string why;
        const bool ok = tfar::InstallInto(mod, why);
        printf("    %s\n", ok ? "installed" : why.c_str());
        Check(ok, "hooks install into TFAR's effect vtables");
        if (!ok)
            return;
        const tfar::Status st = tfar::GetStatus();
        printf("    TFAR version '%s'\n", st.tfarVersion);
        Check(st.state == tfar::HookState::Hooked && st.tfarVersion[0], "status reports hooked + TFAR version");

        // Stand-in PersonalRadioEffect: vptr, RadioEffect { float delayLine[2400]; size_t pos; },
        // SimpleRadioEffect { float errorLevel; float phase; filters... }.
        std::vector<uint8_t> object(64 * 1024, 0);
        void* self = object.data();
        *reinterpret_cast<uintptr_t*>(self) = vt;
        auto slots = *reinterpret_cast<void***>(self);
        using ProcessFn = void (*)(void*, float*, int);
        using SetErrorFn = void (*)(void*, float);
        using DtorFn = void* (*)(void*, unsigned int);

        // Slot 1 through our hook reaches TFAR's setErrorLeveL: it stores calcErrorLevel(0.3) = 0.6
        // in SimpleRadioEffect::errorLevel, right after RadioEffect's delay line and position.
        reinterpret_cast<SetErrorFn>(slots[1])(self, 0.3f);
        const float stored = *reinterpret_cast<float*>(object.data() + 8 + 2400 * 4 + 8);
        char what[160];
        snprintf(what, sizeof(what), "slot 1 is TFAR's setErrorLeveL (stored %.3f, expected 0.600)", stored);
        Check(std::fabs(stored - 0.6f) < 1e-3f, what);

        // Slot 0 (enhanced): our effect, TFAR's object untouched.
        std::vector<float> voice = Voice(2.f);
        tfar::SetSettings(true, 1.f);
        for (size_t i = 0; i + kBlock <= voice.size(); i += kBlock)
            reinterpret_cast<ProcessFn>(slots[0])(self, voice.data() + i, kBlock);
        bool delayTouched = false;
        for (int i = 0; i < 2400; ++i)
            delayTouched = delayTouched || *reinterpret_cast<float*>(object.data() + 8 + i * 4) != 0.f;
        Check(Rms(voice, 0.5f, 2.f, 1) > 1e-3f && !delayTouched, "slot 0 runs the enhanced effect");
        const tfar::Status st2 = tfar::GetStatus();
        Check(st2.active == 1 && std::fabs(st2.signalLoss - 0.3f) < 1e-6f, "status sees the stream and its signal loss");

        // Slot 0 (switched off): TFAR's own process, which writes its 50 ms delay line.
        tfar::SetSettings(false, 1.f);
        std::vector<float> block(kBlock, 0.01f);
        reinterpret_cast<ProcessFn>(slots[0])(self, block.data(), kBlock);
        for (int i = 0; i < 2400; ++i)
            delayTouched = delayTouched || *reinterpret_cast<float*>(object.data() + 8 + i * 4) != 0.f;
        Check(delayTouched, "switched off: slot 0 calls TFAR's original process");
        tfar::SetSettings(true, 1.f);

        // Slot 2: our destructor hook forgets the object, then TFAR's destructor (no delete).
        reinterpret_cast<DtorFn>(slots[2])(self, 0);
        Check(tfar::GetStatus().active == 0, "destructor hook drops the object's state");

        tfar::Shutdown();
        uint64_t p = 0, e = 0;
        ar::mem::SafeRead64(vt, p);
        ar::mem::SafeRead64(vt + 8, e);
        Check(p == origProcess && e == origSetError, "shutdown restores TFAR's vtables");

        // Generator path: finds the module by name and hooks again.
        const bool changed = tfar::Tick();
        Check(changed && tfar::GetStatus().state == tfar::HookState::Hooked, "Tick() finds and hooks the loaded TFAR");
        Check(!tfar::Tick(), "Tick() leaves an intact hook alone");
        tfar::Shutdown();
        ar::mem::SafeRead64(vt, p);
        Check(p == origProcess, "restored again");
    }
}

int main(int argc, char** argv)
{
    TestDsp();
    char env[MAX_PATH] = {};
    GetEnvironmentVariableA("AR_TFAR_DLL", env, MAX_PATH);
    const char* path = argc > 1 ? argv[1] : env;
    if (*path)
        TestHook(path);
    else
        printf("-- TFAR hook: SKIPPED (pass the path to TFAR_win64.dll) --\n");
    printf(g_failures ? "%d FAILED\n" : "ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
