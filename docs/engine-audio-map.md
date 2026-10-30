# Arma 3 engine audio graph (arma3_x64.exe, 2026-10-24 build)

Mapped statically with `tools/re/` (xref.py, vtref.py, rtti_vtable.py). At runtime the extension
checks every pointer against `xaudio2_9.dll`, so a layout change fails safely ("validation failed")
instead of crashing. If that happens, run `["diag"] call AR_fnc_ext` and read
`%LOCALAPPDATA%\Arma 3\AudioReverb.log`.

## Finding the sound system
- Class `SoundSystemXA2` (RTTI `.?AVSoundSystemXA2@@`), size 0x410, multiple inheritance:
  primary vtable `0x141B6F9A0`, AbstractSoundSystem subobject at +0x10 (vtable `0x141B6F9C0`).
- Constructed at `0x14176ED93`. The engine global (RVA `0x21A3F70`) holds **instance + 0x10**.
- The extension finds both vtables from RTTI at runtime, then scans `.data` (about 2 MB) for
  pointers to an object with either vtable. It doesn't depend on the build-specific global address.

## Device setup
- `0x14092CC00`: `LoadLibraryExW("xaudio2_9.dll", LOAD_LIBRARY_SEARCH_SYSTEM32)` →
  `XAudio2CreateWithVersionInfo(&inst->xaudio, 0, 1, NTDDI 0x0A000008)`, stored at **inst+0xE0**.
- `0x14092D460` / `0x14092D501`: builds the voice graph on the mixer subobject at **inst+0xE8**.

## Voices (offsets from the instance base)

| Offset | Name (from engine error strings) | Ch | Rate | Stage | Sends to | Effects |
|---|---|---|---|---|---|---|
| +0x240 | mastering | device | device | – | – | engine chain |
| +0x248 | PRE-MASTER | device | 44100 | 0x80 | mastering | – |
| +0x250 | VOICE_3D | = pre | = pre | 0x40 | PRE-MASTER (USEFILTER) | **none** |
| +0x258 | External VOICE_3D | = 3D | = 3D | 0x20 | VOICE_3D | attenuation (House/vehicle EQ+Echo) |
| +0x260 | VOICE_2D | 2 | = pre | 0x40 | PRE-MASTER | – |
| +0x268 | MUSIC | 2 | = pre | 0x80 | mastering | – |
| +0x270/278/280 | RADIO chain | 2 | 22–48k / 11025 | 0x20/0x10/0x08 | → VOICE_2D | radio filter |
| +0x288 | SPEECH_EX | 2 | = pre | 0x80 | mastering | – |
| +0x290 | PREVIEW | 2 | = pre | 0x80 | mastering | – |
| +0x298 | CUSTOM | = 3D | = 3D | 0x10 | External VOICE_3D | – |
| +0x2A0 | WEAPONS | = 3D | = 3D | 0x10 | External VOICE_3D | 1 effect |
| +0x2A8 | External VOICE_3D #2 (interior) | = 3D | = 3D | 0x20 | VOICE_3D | 1 effect |
| +0x2B0 | ENVIRONMENT | = 3D | = 3D | 0x10 | External VOICE_3D | – |

## Where the reverb goes
VOICE_3D carries every 3D world sound after the engine's own attenuation, and the engine never
sets an effect chain on it. The extension adds a stereo submix at stage **0x60** with the XAudio2
reverb APO (100% wet). VOICE_3D sends to it through a downmix matrix whose gain is the "send"
level, and it outputs to PRE-MASTER. VOICE_3D's original send keeps its filter and matrix:
they're read back and re-applied after `SetOutputVoices`.

Music, UI, speech, preview and radio never pass through VOICE_3D, so they stay dry.

A second stereo submix at the same stage (the **echo bus**) holds a custom XAPO, `TapEcho`
(`extension/src/tap_echo.cpp`). It's a 24-slot multi-tap delay, one slot per scanner ray. Each slot
has a delay of 2d/c, a gain from reflectivity and spreading loss, an L/R pan relative to the camera,
and a one-pole HF damping filter. Delays glide (with at most 2% pitch bend) and large jumps
crossfade, so moving around never clicks. VOICE_3D sends to it with a plain downmix: the echo level
lives in the tap gains.

A third stereo submix holds `FdnReverb` (`extension/src/fdn_reverb.cpp`). It's the default late
reverb, because XAudio2's I3DL2 reverb rings metallically. The signal path is: input HF damping,
pre-delay, 6-tap early reflections, 4 allpass diffusers, then an 8-line FDN with a Hadamard
feedback matrix, slow per-line delay modulation, and per-line damping set from the low- and
high-frequency T60s. The same I3DL2 presets drive it through a mapping in `reverb_bus.cpp`
(`ToFdn`). Its output level is calibrated to match the XAudio2 reverb within ±2 dB (bus_test). The
send goes to whichever model is active (`model` command / `AR_model`). The other bus gets a zero
matrix.

```
VOICE_3D --dry------------------------------------------> PRE-MASTER
        \--send (model 0)--> xaudio2 bus [meter|I3DL2|meter] -/
        \--send (model 1)--> fdn bus     [meter|FDN|meter] ---/
        \--downmix---------> echo bus    [TapEcho|meter] ----/
```

## Lifetime
`IXAudio2Voice::DestroyVoice` (vtable slot 18) on XAudio2's submix vtable is patched in
xaudio2_9.dll memory, not game memory. When the engine destroys VOICE_3D or PRE-MASTER (device
change, shutdown), the hook first restores VOICE_3D's routing and destroys the bus, so the engine's
own teardown succeeds. A watchdog re-reads the voice pointers every second. While detached, the
worker retries attaching every 3 s. `extension/tests/bus_test.cpp` exercises all of this against
a replica graph.
