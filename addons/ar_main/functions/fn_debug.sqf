/*
	AR_fnc_debug
	Toggles an on-screen readout of the scanner, preset weights and extension status.

	Usage (debug console): [] call AR_fnc_debug;
*/

if (!isNil "AR_debugHandle") exitWith {
	terminate AR_debugHandle;
	AR_debugHandle = nil;
	hintSilent "";
};

AR_debugHandle = [] spawn {
	while {true} do {
		private _status = ["status"] call AR_fnc_ext;
		_status params [["_attached", false], ["_enabled", false], "", "", "", ["_err", ""], ["_inDb", -120], ["_outDb", -120], ["_sendNow", 0], ["_echoDb", -120], ["_taps", 0], ["_model", ""]];
		private _text = format ["AudioReverb (%9)\nattached %1  enabled %2 %3\nbus in %4 dB  reverb %5 dB  send %6\necho %7 dB (%8 taps)\n",
			_attached, _enabled, _err, _inDb toFixed 1, _outDb toFixed 1, _sendNow toFixed 2, _echoDb toFixed 1, _taps, _model];
		if (!isNil "AR_override") then { _text = _text + "AUDITION (scanner overridden)\n" };
		if (_model == "conv") then {
			if (!isNil "AR_irOverride") then {
				_text = _text + format ["IR audition: %1\n", AR_irOverride];
			} else {
				if (AR_irSource == 1 && {!isNil "AR_irLastMix"}) then {
					// Flat [path, weight, ...] -> "name 0.60, name 0.40"
					private _parts = [];
					for "_i" from 0 to (count AR_irLastMix - 2) step 2 do {
						private _p = AR_irLastMix select _i;
						private _name = if (_p == "") then { "synth" } else { ((_p splitString "\") select -1) select [0, count ((_p splitString "\") select -1) - 4] };
						_parts pushBack format ["%1 %2", _name, (AR_irLastMix select (_i + 1)) toFixed 2];
					};
					_text = _text + format ["IRs: %1\n", _parts joinString ", "];
				} else {
					_text = _text + "IRs: synthesized\n";
				};
			};
		};
		_text = _text + format ["voice: %1, %2 speaker(s) in earshot%3\n",
			["off", "on"] select AR_voiceEnabled, missionNamespace getVariable ["AR_lastVoiceSpeakers", 0],
			["", ", own voice on"] select AR_voiceSelf];
		// TeamSpeak plugin heartbeat: is it running, linked, and matching the people talking?
		(["voicestatus"] call AR_fnc_ext) params [["_seen", false], ["_state", "not detected"], ["_ts", ""], ["_api", 0],
			["_heard", 0], ["_matched", 0], ["_lastMatched", ""], ["_lastUnmatched", ""], ["_resets", 0],
			["_radioHook", 0], ["_tfarVersion", ""], ["_radioEnhanced", false], ["_radioActive", 0], ["_radioLoss", 0]];
		if (_seen) then {
			_text = _text + format ["TS plugin: %1 (TS %2), hearing %3, in room %4", _state, _ts, _heard, _matched];
			if (_lastMatched != "") then { _text = _text + format [" [%1]", _lastMatched] };
			if (_lastUnmatched != "") then { _text = _text + format [", dry: %1", _lastUnmatched] };
			_text = _text + "\n";
			_text = _text + (switch (_radioHook) do {
				case 1: {
					format ["radio: TFAR %1, %2, %3 stream(s)%4\n", _tfarVersion,
						["TFAR's own sound", "enhanced"] select _radioEnhanced, _radioActive,
						["", format [", signal %1%2", round (100 * (1 - _radioLoss)), "%"]] select (_radioActive > 0)]
				};
				case 2: { format ["radio: TFAR %1 not supported, TFAR's own sound\n", _tfarVersion] };
				default { "radio: TFAR plugin not loaded in TeamSpeak\n" };
			});
		} else {
			_text = _text + format ["TS plugin: %1\n", _state];
		};
		if (!isNil "AR_lastState") then {
			AR_lastState params ["_scan", "_trees", "_houses", "_values", "_weights", "_indoor", "_roomSize", ["_free", false]];
			_scan params ["_hitFrac", "_nearFrac", "_nearDist", "_roofFrac", "_roofDist", "_structFrac"];
			_text = _text + (["\nlistener: unit", "\nlistener: free camera (Zeus/spectator)"] select _free);
			_text = _text + format [
				"\nhit %1  near %2 (%3 m)\nroof %4 (%5 m)  struct %6\ntrees %7  houses %8\nindoor %9  room %10 m\n",
				_hitFrac toFixed 2, _nearFrac toFixed 2, _nearDist toFixed 1,
				_roofFrac toFixed 2, _roofDist toFixed 1, _structFrac toFixed 2,
				_trees, _houses, _indoor toFixed 2, _roomSize toFixed 1
			];
			{
				if (_y > 0.01) then { _text = _text + format ["\n%1: %2", _x, _y toFixed 2] };
			} forEach _weights;
			_text = _text + format ["\n\ndecay %1 s  send %2", (_values select 3) toFixed 2, (_values select 12) toFixed 2];
			private _echoes = (_scan select 6) select { (_x select 0) >= getNumber (configFile >> "CfgAudioReverb" >> "Echo" >> "minDistance") };
			private _dists = _echoes apply { round (_x select 0) };
			_dists sort true;
			_text = _text + format ["\nechoes (m): %1", _dists];
		};
		hintSilent _text;
		sleep 0.5;
	};
};
