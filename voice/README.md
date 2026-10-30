# AudioReverb Voice (TeamSpeak 3 plugin)

Room reverb on **TFAR direct speech**, in the same room the game's reverb uses (radio stays dry),
and an **enhanced TFAR radio sound** that replaces TFAR's own radio effect.

## How it works

```
Arma: AR_fnc_voice ──"voice" cmd──> AudioReverb_x64.dll ──shared memory (voice_link.h)──┐
      (per player: TFAR name + direct-speech audibility; room environment; OpenAIR blend)  │
                                                                                          v
TeamSpeak: decoded voice of each speaker ──tap (unmodified)──> gain by name ──> mixing ring
           final mix  <──add── convolution (same IR blend as the game) <──────────────────┘
           own mic (optional, only while TeamSpeak says you're talking) ──> same convolution
```

- The plugin taps each speaker in `onEditPlaybackVoiceDataEvent`, *before* TFAR processes them,
  and never modifies the audio. TFAR's own processing is untouched.
- Each speaker's reverb send is their direct-speech audibility from Arma. Someone far away
  talking on the radio gets 0, so radio is never reverbed. Someone beside you gets reverb whether
  they're keyed up or not, because you'd hear them in the room anyway.
- Matching: TFAR renames TeamSpeak nicknames to the in-game name (`TFAR_unitName` or `name`).
  Both sides apply TFAR's rule (edge spaces become `_`, 30-character limit).
- No Arma data for 3 s (game closed or not in a mission): the plugin is silent.
- Cost: one convolution, about 2% of a CPU core.

## Enhanced radio sound

TFAR's radio effect (`RadioEffect.hpp` in TFAR's plugin) is a high-pass and low-pass biquad, plus a
90 Hz ring modulator and foldback distortion that both grow with signal loss. The plugin replaces
it with `radio_fx.h`:

```
normalise -> radio ALC (4:1) -> soft saturation -> [digital crunch near max range]
  -> 4th-order channel band-pass + speaker resonance -> fading / flutter / dropouts
  -> + band-limited hiss and crackle -> TFAR's long-term loudness -> 50 ms delay (as TFAR)
```

- Hiss behaves like FM. On a good signal it only fills the pauses, about 32 dB down. Near the
  edge of range it nearly swamps the voice, and short dropouts and crackle start.
- Per radio type: SW handhelds 300-3400 Hz, LR 350-2600 Hz with more drive, airborne 450-3800 Hz.
  Underwater radios keep TFAR's effect.
- Loudness matches TFAR's within 0.1 dB. A replica of TFAR's linear path runs alongside as the
  reference, so radio volume knobs scale exactly as before.
- Cost: about 1.2% of a CPU core per active radio stream.

**How it hooks TFAR (`tfar_hook.h`).** TFAR's plugin (`TFAR_win64.dll`) runs in the same
TeamSpeak process. Its `PersonalRadioEffect`, `LongRangeRadioEffect` and `AirborneRadioEffect`
vtables are found by RTTI name. Three slots are swapped: `process`, `setErrorLeveL` (to read the
raw signal loss) and the destructor.
- TFAR's routing, volumes, stereo sides and signal-loss maths all still run.
- Before patching, the layout is checked: all three classes must share `setErrorLeveL`, and LR
  and airborne must share `process`. An unknown TFAR build is left alone and logged.
- Switching the option off calls TFAR's original functions live.
- Unloading the plugin restores TFAR's vtables. A reloaded TFAR is hooked again.
- Verified against TFAR 1.-1.0.341 (Workshop beta, Jan 2026): `radio_test.exe <TFAR_win64.dll>`
  drives a stand-in object through the patched vtable, including TFAR's own `setErrorLeveL` and
  `process`.

## Install

Double-click `@AudioReverbConv\teamspeak\AudioReverbVoice.ts3_plugin` and TeamSpeak's installer
does the rest. Or copy `audioreverb_voice_win64.dll` to `%APPDATA%\TS3Client\plugins`, restart
TeamSpeak, and enable "AudioReverb Voice" under Settings > Plugins.

## Options (CBA: Addon Options > AudioReverb > Voice (TFAR))

| Setting | Default | |
|---|---|---|
| Voice reverb | on | other players' direct speech |
| Voice reverb level | 1.0 | |
| Hear own voice in the room | off | only you hear it |
| Own voice reverb level | 0.6 | |

Radio (Addon Options > AudioReverb > Radio (TFAR)):

| Setting | Default | |
|---|---|---|
| Enhanced radio sound | on | off = TFAR's own effect (switches live) |
| Radio static | 1.0 | hiss/crackle amount, 0..2 |

Without CBA: `AR_voiceEnabled`, `AR_voiceLevel`, `AR_voiceSelf`, `AR_voiceSelfLevel`,
`AR_radioEnhanced`, `AR_radioStatic`.
Tuning (range, fade, vehicle isolation): `CfgAudioReverb >> Voice`.

## Known limits
- A speaker's whisper/normal/yell range isn't synced to other clients, so TFAR's normal 20 m
  range is assumed (scaled by TFAR's public `tf_voiceVolume`). A whisper therefore gets about
  as much room reverb as normal speech.
- The room is the *listener's* (the camera's in Zeus). TFAR hears at your unit's head in Zeus
  unless you're spectating.
- ACRE2: not wired up yet. The plugin side is mod-agnostic; ACRE needs its own name mapping and
  plugin load order checked.

## Build and test
Built with the extension (`build_extension.ps1`). `extension\build\Release\voice_test.exe`
loads the plugin like TeamSpeak and checks the behaviour above. `radio_test.exe [TFAR_win64.dll]`
tests the radio effect, and the hook against a real TFAR binary. Pass the same path as the second
argument of `voice_test.exe` to run the plugin end to end next to TFAR.

`ts3sdk/` contains the TeamSpeak 3 Client plugin SDK headers (TeamSpeak Systems GmbH), taken
from TFAR's repository, which builds against TeamSpeak 3.6.
