# AudioReverb: Convolution (main line)

**Convolution won the A/B, so this is now the main line.** `..\AudioReverb` (v0.3, FDN) is kept as
a snapshot. This project builds `@AudioReverbConv`. Load only one of the two mods at a time: both
ship `AudioReverb_x64.dll` and the `AR_Main` addon.

**Diagnostics:** `%LOCALAPPDATA%\Arma 3\AudioReverb.log` records attach/detach, a performance line
every minute (XAudio2 audio-thread CPU, glitch count, our effects' worst processing time), and, if
game audio ever stops, a `STALL:` line naming what stopped (XAudio2's audio thread, our worker, or
one of our effects) plus any `XAudio2 CRITICAL ERROR` from the device. The TeamSpeak plugin writes
`AudioReverbVoice.log` beside it.

**TeamSpeak voice reverb (TFAR):** `voice/` builds the AudioReverb Voice TeamSpeak plugin
(`@AudioReverbConv\teamspeak\AudioReverbVoice.ts3_plugin`). Nearby players' direct speech takes
on your room; radio stays dry. Optional own-voice reverb is a CBA option. See `voice/README.md`.

What's different here:
- **Convolution late reverb** (`AR_model = 2`, the default). It uses uniformly partitioned
  overlap-save FFT convolution (`extension/src/conv_engine.cpp`) with 512-sample blocks, and costs
  about 2% of one core for a 3 s IR.
- **Parametric IRs.** `ir_synth.cpp` synthesizes the impulse response from the scanned environment:
  band-split decorrelated velvet noise with separate low/mid/high T60s, echo build-up, and discrete
  early reflections. A generator thread rebuilds it when the environment changes audibly, and the
  engine crossfades over about 90 ms.
- **Recorded IRs.** `["irs\file.wav"] call AR_fnc_ir;` uses a WAV file (see `irs/README.txt`).
  Its level is matched to the environment.
- Instant A/B in-game: `AR_model = 2;` (convolution) / `1;` (FDN) / `0;` (XAudio2).
- Tests: `bus_test` also checks the convolution engine against direct convolution (max error
  1.4e-5), its level against the FDN (within ±1.5 dB), stability across IR swaps, and WAV loading.

Client-side spatial reverb for Arma 3: an extension DLL adds a real-time reverb bus to the game's
XAudio2 graph, and an SQF scanner drives it from the geometry around the listener.

| Path | What |
|---|---|
| `extension/` | `AudioReverb_x64.dll` (C++17, CMake). `tests/bus_test.cpp` = offline test |
| `voice/` | TeamSpeak plugin (`audioreverb_voice_win64.dll`), `tests/voice_test.cpp` |
| `addons/ar_main/` | Mod: environment scanner, preset blending (`CfgAudioReverb`), debug HUD |
| `probe/` | Phase 0 config probe (not part of the mod) |
| `docs/` | `phase0-findings.md`, `engine-audio-map.md` |
| `tools/` | PBO packer, reverse-engineering scripts (`tools/re/`) |

## Build
Needs MSVC (VS 2019/2022 C++ workload), Python 3, and Arma 3 Tools (for config validation).

```
powershell -File build.ps1
extension\build\Release\bus_test.exe
```

Output: `@AudioReverb\` (DLL + `addons\ar_main.pbo` + `mod.cpp`).

## Test in-game
1. Launcher → Mods → add local mod `@AudioReverb`. **Unload `@AudioReverbProbe`**: it overrides
   HouseAttenuation with an exaggerated echo.
2. Launcher → Parameters → **disable BattlEye** (the DLL isn't whitelisted).
3. Eden → any map → play as a unit. You should see no "AudioReverb:" error in system chat.
4. Debug console: `[] call AR_fnc_debug;` shows the scanner state, preset weights and DLL status.
   `bus in` / `bus out` are the reverb bus's peak levels over the last 0.5 s (-120 = silence).
   When you fire, `bus in` should jump to around -10..0 dB. If it stays at -120, game audio isn't
   reaching the bus.
5. Sanity check at full strength: `["Hangar", 2] call AR_fnc_audition;` and fire. Turn it off
   with `[] call AR_fnc_audition;`.
6. Fire a weapon outdoors in an open field, in a town, in a forest, in a small house, and in a
   large hangar. Toggle A/B: `AR_enabled = false;` / `AR_enabled = true;`.
   Overall strength: `AR_intensity = 1.5;` (or the CBA slider).
7. If status shows `attached=false`: run `["diag"] call AR_fnc_ext;` and send
   `%LOCALAPPDATA%\Arma 3\AudioReverb.log`.

Tuning lives in `addons/ar_main/config.cpp` (`CfgAudioReverb`). With CBA loaded, the enable and
intensity options appear under Addon Options → AudioReverb.

Echo tuning: `CfgAudioReverb >> Echo` (level, reflectivity, damping). The HUD lists the echo
distances it found and the echo bus peak level.

## Status
- v0.1 (verified in-game): engine graph mapped, reverb bus attaches, scanner + preset blend.
- v0.2 (offline tests pass, not yet heard in-game): geometric echo bus (TapEcho XAPO driven by the
  scanner rays); sparse outdoor late field so towns no longer sound like rooms.
- v0.3 (offline tests pass, not yet heard in-game): own FDN late reverb replaces XAudio2's I3DL2
  reverb by default (A/B with `AR_model = 0;` / `AR_model = 1;`); urban sends -60%.
- Next: per-source distance (distant/muffled), TFAR direct-speech reverb, BattlEye whitelisting.
