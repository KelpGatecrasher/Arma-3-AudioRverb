#pragma once

#include "radio_fx.h"

#include <windows.h>
#include <cstdint>
#include <string>

// Replaces TFAR's radio effect with RadioFx, inside TeamSpeak, without modifying TFAR.
//
// TFAR's TeamSpeak plugin (TFAR_win64.dll, same process as us) runs every radio transmission
// through a RadioEffect object: PersonalRadioEffect (SW), LongRangeRadioEffect (LR, intercom)
// or AirborneRadioEffect. Their vtables are found by RTTI name and three slots patched:
//   0 process(float* mono, int n)   -> RadioFx (or TFAR's original while switched off)
//   1 setErrorLeveL(float loss)     -> remember the raw signal loss, then TFAR's original
//   2 scalar deleting destructor    -> drop our state for the object, then TFAR's original
// TFAR still decides who is heard on which radio, volume, stereo side and signal loss; only the
// sound of the radio changes. Underwater radios keep TFAR's effect.
//
// Install/uninstall happen on the voice engine's generator thread; the hooks run on
// TeamSpeak's audio thread.
namespace ar::voiceplugin::tfar
{
    enum class HookState : uint32_t
    {
        NotLoaded = 0,   // TFAR's plugin isn't loaded in TeamSpeak
        Hooked = 1,
        Unsupported = 2, // TFAR found but its effect classes weren't (other TFAR build)
    };
    const char* HookStateName(HookState s);

    struct Status
    {
        HookState state = HookState::NotLoaded;
        char tfarVersion[32] = {};
        bool enhanced = true;
        uint32_t active = 0;     // radio streams processed in the last second
        float signalLoss = 0;    // most recent transmission's signal loss (0 = perfect)
        uint64_t blocks = 0;     // blocks processed by RadioFx since start
        std::string detail;      // why Unsupported
    };

    // Live settings from Arma (CBA); defaults until the first update: enhanced, static 1.
    void SetSettings(bool enhanced, float staticLevel);

    // Generator thread (~1 Hz): finds TFAR in this process and (re)patches it, including after
    // TFAR is reloaded. Returns true when the state changed (worth logging).
    bool Tick();
    Status GetStatus();

    // Restores TFAR's vtables (if TFAR is still loaded) and waits for in-flight hook calls.
    void Shutdown();

    // Tests: patch an explicitly given module.
    bool InstallInto(HMODULE mod, std::string& why);
}
