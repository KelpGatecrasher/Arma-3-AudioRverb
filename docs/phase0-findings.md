# Phase 0 — Can the engine's built-in reverb be driven from config/SQF?

Analysed: `arma3_x64.exe` and `Dta\bin.pbo` from the installed build (2026-10-24),
using CfgConvert plus the scripts in `tools/re/`.

## Verdict

**Not as real reverb.** The engine *parses* a full XAudio2 reverb parameter set from
`CfgSoundEffects >> ReverbationEffects`, but nothing ever creates a reverb effect from it.
What config *can* drive is an **EQ → EQ → Echo** chain (`AttenuationsEffects`), including a
hardcoded `HouseAttenuation` chain used inside buildings. That gives a single-tap feedback echo
(slapback), not diffuse room reverb, and the config is fixed when the game starts.

The in-game probe below checks these conclusions before we commit to the DLL.

## Evidence

### What the config loader does
- Loader function `0x14093FBA9` walks two classes under `CfgSoundEffects`:
  - `AttenuationsEffects` → each subclass becomes an object (vtable `0x141B70F20`, RTTI
    `XA2AttenuationEffectPars`) holding exactly three stages, read by `0x14093F8EA`:
    `Equalizer0` (FXEQ), `Equalizer1` (FXEQ), `Echo` (FXECHO: `WetDryMix`, `Feedback`, `Delay`).
    No other subclass names are read, so a `Reverb` subclass is silently ignored.
  - `ReverbationEffects` → each subclass becomes `Sound::XA2ReverEffectPars` (vtable
    `0x141B70F48`). Its reader `0x140937690` reads all 22 `XAUDIO2FX_REVERB_PARAMETERS` keys
    (`WetDryMix … RoomSize`) directly from the class. The result is stored in a map keyed by
    class name.
- Base config (`bin.pbo`) defines neither class; `sounds_f.pbo` defines `AttenuationsEffects`
  (House, Car, Truck, Tank, Heli, Plane variants, …). No installed addon or mod defines
  `ReverbationEffects`.

### Why `ReverbationEffects` is dead
- The engine's runtime effect classes (from RTTI) are: `XA2Echo`, `XA2Equalizer`,
  `XA2Compressor`, `XA2MonoEffect`, `XA2Stereo3D`, `XA2VehicleAttenuateEffect`,
  `XA2WeaponFireDistanceEffect`, plus custom APOs (Compressor, Limiter, Mono, Stereo3D,
  Discretizer256). **There is no reverb effect class.**
- Imports from `XAudio2_8.dll`: only ordinals 4/5/6 = `CreateFX`, `X3DAudioCalculate`,
  `X3DAudioInitialize`. `CreateAudioReverb` is not imported, not named anywhere in the exe, and
  no reverb CLSID (FXReverb, AudioReverb 2.7/2.9) is present. Only the FXEQ CLSID is found.
- The I3DL2 preset name table (`Paddedcell`, `Room`, … `Plate`) at `0x14004BD90` has no direct
  callers. It looks like a leftover enum table.

### What script can touch
- `enableAudioFeature` — features `"lowpass"` (global, unary) and `"building_interior"`
  (per object, binary: `obj enableAudioFeature ["building_interior", bool]`, returns the previous
  state). O&T Expansion Eden exposes the latter as an Eden object attribute.
- `HouseAttenuation` and `UnconsciousState` are hardcoded names, selected in `0x1407063D0`.
- There's no script command to swap effect chains or change their parameters at runtime. The
  only per-entity choice is `attenuationEffectType` in a vehicle's config.

### Useful for the DLL phase
The engine builds separate XAudio2 submix voices. From its error strings: PRE-MASTER,
VOICE_3D, VOICE_2D, SPEECH_EX, PREVIEW, MUSIC, RADIO, External VOICE_3D, CUSTOM, WEAPONs,
ENVIRONMENT. Adding reverb on VOICE_3D / WEAPONs / ENVIRONMENT would leave music, UI and radio
dry without guessing per voice. The engine creates voices through `CreateFX` and standard
`IXAudio2` calls, so hooking `IXAudio2::CreateSubmixVoice` should let us find them.

## In-game probe (`@AudioReverbProbe`)

Build: `powershell -File build_probe.ps1` → `@AudioReverbProbe\addons\ar_probe.pbo`.
Load it as a local mod in the launcher. Singleplayer / Eden only: it's unsigned, and it
deliberately overrides vanilla `HouseAttenuation`.

What it adds:
- `HouseAttenuation` overridden with flat EQ + an obvious echo (wet 0.5, feedback 0.5, 90 ms).
- `ReverbationEffects` classes `Default`, `Generic`, `HouseAttenuation`, `CarAttenuation`,
  `Hangar`, all at extreme reverb values.
- Three SUVs in Eden (Civilian → Cars): `[AR Probe] SUV - no echo / small echo / large echo`.
- `AR_fnc_dump`, `AR_fnc_status`, `AR_fnc_shot` (see function headers).

### Test steps (Eden, Altis or Stratis, run each from the debug console)
1. `[] call AR_fnc_dump;` → paste the clipboard into `docs/cfgsoundeffects-dump.txt`.
   Check that `HouseAttenuation` lists `ar_probe` last in its mod list. If another mod (e.g.
   O&T Expansion Eden) wins, unload that mod for the test.
2. **Is `ReverbationEffects` live?** Stand outdoors: `[20] call AR_fnc_shot;`
   Expected: a dry shot with no 20-second tail. Any huge wash means the static analysis is wrong.
3. **Does `HouseAttenuation` apply indoors?** `[] call AR_fnc_status;`, walk into an
   enterable house, check `building_interior` reads `true`, then fire `[8] call AR_fnc_shot;`
   indoors and outdoors. Expected indoors: an obvious 90 ms repeating echo.
   If it reads `false`: `cursorObject enableAudioFeature ["building_interior", true];` while
   looking at the house, then retest.
4. **How good can an echo-only "room" sound?** Sit in each probe SUV in turn and fire
   `[20, 5, 0.3] spawn AR_fnc_shot;`. Compare no / small / large.
5. **Lowpass feature:** outdoors, `enableAudioFeature ["lowpass", false];` and fire shots
   behind a building. Then set it back to `true`. Note what changes.

Record results in the table:

| Test | Expected | Result |
|---|---|---|
| 2. ReverbationEffects audible | no | |
| 3. HouseAttenuation echo indoors | yes | |
| 3. building_interior default on houses | ? | |
| 4. Echo "rooms" usable as reverb | probably not | |
| 5. lowpass off changes occlusion | ? | |

## What it means for the plan
- If test 2 confirms no reverb: real reverb needs the DLL (phase 1+). Config can still help:
  tone down vanilla `HouseAttenuation`'s echo so it doesn't stack on top of our reverb.
- `building_interior` and `insideBuilding` are useful inputs for the SQF environment
  scanner either way.
