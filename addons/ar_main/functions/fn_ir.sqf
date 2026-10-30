/*
	AR_fnc_ir
	Convolution engine: audition one recorded impulse response (WAV) everywhere, overriding the
	scanner's blend of recorded spaces. The recording is level-matched to the current
	environment; the scanner still controls the send level and echoes.
	Call with no arguments to hand the impulse response back to the scanner (AR_irSource decides
	between the OpenAIR blend and synthesized IRs).

	Arguments: 0: path <STRING> (default ""), relative to the mod folder or absolute

	Examples (debug console):
		["irs\openair\hangar_t2.wav"] call AR_fnc_ir;
		[] call AR_fnc_ir;
*/

params [["_path", ""]];

if (_path isEqualTo "") exitWith {
	AR_irOverride = nil;
	AR_irLastMix = nil; // the scanner loop resends its blend on the next scan
	systemChat format ["AudioReverb: impulse response back to the scanner (%1).",
		["synthesized", "OpenAIR recordings"] select (AR_irSource == 1)];
};

AR_irOverride = _path;
["irfile", [_path]] call AR_fnc_ext;

// Loading happens on the extension's generator thread; report the outcome shortly after.
[_path] spawn {
	params ["_path"];
	sleep 0.5;
	private _err = (["irerror"] call AR_fnc_ext) param [0, ""];
	if (_err isEqualTo "") then {
		systemChat format ["AudioReverb: auditioning impulse response %1", _path];
	} else {
		systemChat format ["AudioReverb: could not load %1 (%2)", _path, _err];
	};
};
