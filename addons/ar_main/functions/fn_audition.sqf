/*
	AR_fnc_audition
	Overrides the scanner with a fixed preset so presets can be compared by ear.
	Call with no arguments to hand control back to the scanner.

	Arguments:
		0: preset name from CfgAudioReverb >> Presets, or a full 13-value array <STRING|ARRAY> (default "": stop)
		1: send override <NUMBER> (default -1: keep the preset's send)

	Examples (debug console):
		["Hangar"] call AR_fnc_audition;
		["Urban", 2] call AR_fnc_audition;
		[] call AR_fnc_audition;
*/

params [["_preset", ""], ["_send", -1]];

if (_preset isEqualTo "") exitWith {
	AR_override = nil;
	systemChat "AudioReverb: audition off, scanner resumed.";
};

private _values = if (_preset isEqualType []) then { +_preset } else {
	getArray (configFile >> "CfgAudioReverb" >> "Presets" >> _preset >> "values")
};
if (count _values != 13) exitWith { systemChat format ["AudioReverb: unknown preset %1", _preset] };
if (_send >= 0) then { _values set [12, _send] };

AR_override = _values;
systemChat format ["AudioReverb: auditioning %1 (send %2)", _preset, _values select 12];
