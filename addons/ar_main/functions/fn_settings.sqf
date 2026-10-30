/*
	AR_fnc_settings (preInit)
	Defaults for the client options, exposed as CBA settings when CBA is loaded.

	AR_enabled        - reverb on/off
	AR_intensity      - multiplier on the send level (0..2)
	AR_model          - late reverb engine: 2 = convolution (default), 1 = AudioReverb FDN,
	                    0 = XAudio2 built-in
	AR_irSource       - convolution impulse responses: 1 = OpenAIR recordings blended per environment
	                    (default), 0 = synthesized from the scanned environment
	AR_voiceEnabled   - room reverb on TFAR direct speech (needs the AudioReverb Voice TeamSpeak plugin)
	AR_voiceLevel     - how strongly other players' voices fill the room (0..2)
	AR_voiceSelf      - also hear your own voice in the room (off by default)
	AR_voiceSelfLevel - own-voice room level (0..2)
	AR_radioEnhanced  - replace TFAR's radio sound with the plugin's enhanced one (same TeamSpeak plugin)
	AR_radioStatic    - radio hiss/crackle amount (0..2)
*/

if (isNil "AR_enabled") then { AR_enabled = true };
if (isNil "AR_intensity") then { AR_intensity = 1 };
if (isNil "AR_model") then { AR_model = 2 };
if (isNil "AR_irSource") then { AR_irSource = 1 };
if (isNil "AR_voiceEnabled") then { AR_voiceEnabled = true };
if (isNil "AR_voiceLevel") then { AR_voiceLevel = 1 };
if (isNil "AR_voiceSelf") then { AR_voiceSelf = false };
if (isNil "AR_voiceSelfLevel") then { AR_voiceSelfLevel = 0.6 };
if (isNil "AR_radioEnhanced") then { AR_radioEnhanced = true };
if (isNil "AR_radioStatic") then { AR_radioStatic = 1 };

if (!isNil "CBA_fnc_addSetting") then {
	["AR_enabled", "CHECKBOX", ["Enable reverb", "Spatial reverb on world sounds."], "AudioReverb", true, 0] call CBA_fnc_addSetting;
	["AR_intensity", "SLIDER", ["Reverb intensity", "Scales how much sound is sent into the reverb."], "AudioReverb", [0, 2, 1, 2], 0] call CBA_fnc_addSetting;
	["AR_model", "LIST", ["Reverb engine", "Convolution (synthesized or recorded impulse responses), AudioReverb FDN, or XAudio2 built-in (legacy)."], "AudioReverb", [[2, 1, 0], ["Convolution", "AudioReverb FDN", "XAudio2 built-in"], 0], 0] call CBA_fnc_addSetting;
	["AR_irSource", "LIST", ["Impulse responses", "Convolution engine: recorded OpenAIR spaces blended per environment, or synthesized from the scanned geometry."], "AudioReverb", [[1, 0], ["OpenAIR recordings", "Synthesized"], 0], 0] call CBA_fnc_addSetting;

	private _voice = ["AudioReverb", "Voice (TFAR)"];
	["AR_voiceEnabled", "CHECKBOX", ["Voice reverb", "Room reverb on nearby players' direct speech. Radio stays dry. Requires the AudioReverb Voice TeamSpeak plugin."], _voice, true, 0] call CBA_fnc_addSetting;
	["AR_voiceLevel", "SLIDER", ["Voice reverb level", "How strongly other players' voices fill the room."], _voice, [0, 2, 1, 2], 0] call CBA_fnc_addSetting;
	["AR_voiceSelf", "CHECKBOX", ["Hear own voice in the room", "Adds the room's reverb to your own voice while you talk (only you hear it)."], _voice, false, 0] call CBA_fnc_addSetting;
	["AR_voiceSelfLevel", "SLIDER", ["Own voice reverb level", "Room level of your own voice."], _voice, [0, 2, 0.6, 2], 0] call CBA_fnc_addSetting;

	private _radio = ["AudioReverb", "Radio (TFAR)"];
	["AR_radioEnhanced", "CHECKBOX", ["Enhanced radio sound", "Replaces TFAR's radio effect with a handset model: band-limited, compressed, with hiss, fading and dropouts as the signal weakens. Only changes what you hear. Requires the AudioReverb Voice TeamSpeak plugin."], _radio, true, 0] call CBA_fnc_addSetting;
	["AR_radioStatic", "SLIDER", ["Radio static", "Amount of hiss and crackle (0 = none, 1 = default)."], _radio, [0, 2, 1, 2], 0] call CBA_fnc_addSetting;
};
