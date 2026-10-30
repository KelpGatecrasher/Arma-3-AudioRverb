/*
	AR_fnc_init (postInit)
	Attaches the reverb bus and runs the environment loop on clients.
*/

if (!hasInterface) exitWith {};

private _ver = ["version"] call AR_fnc_ext;
if (_ver isEqualTo []) exitWith {
	diag_log "[AR] AudioReverb_x64.dll not loaded (missing, or blocked by BattlEye)";
	systemChat "AudioReverb: extension not loaded. Is BattlEye disabled?";
};
diag_log format ["[AR] extension %1", _ver select 0];

// The extension attaches on its own worker thread ("pending"), so the game thread never waits on
// the audio engine. Report only if it still isn't attached a few seconds later.
private _attach = ["attach"] call AR_fnc_ext;
diag_log format ["[AR] attach -> %1", _attach];
[] spawn {
	sleep 5;
	private _status = ["status"] call AR_fnc_ext;
	if !(_status param [0, false]) then {
		systemChat format ["AudioReverb: not attached yet (%1), retrying in background", _status param [5, "unknown"]];
	};

	// Voice reverb needs the TeamSpeak plugin: tell TFAR users once if it isn't running.
	sleep 5;
	if (AR_voiceEnabled && {isClass (configFile >> "CfgPatches" >> "tfar_core")}) then {
		private _vs = ["voicestatus"] call AR_fnc_ext;
		if !(_vs param [0, false]) then {
			systemChat "AudioReverb: TeamSpeak voice plugin not detected - install/enable 'AudioReverb Voice' in TeamSpeak for room reverb on voices.";
		};
	};
};

if (!isNil "AR_loopHandle") then { terminate AR_loopHandle };
AR_loopHandle = [] spawn {
	private _cfg = configFile >> "CfgAudioReverb";
	private _interval = getNumber (_cfg >> "interval");
	private _glide = getNumber (_cfg >> "glide");
	private _glideFast = getNumber (_cfg >> "glideFast");
	private _objEvery = getNumber (_cfg >> "objectScanEvery");
	private _objRadius = getNumber (_cfg >> "objectRadius");

	private _tick = 0;
	private _trees = 0;
	private _houses = 0;
	private _wasIndoor = -1;
	private _wasEnabled = !AR_enabled;
	private _wasModel = -1;

	while {true} do {
		if (AR_enabled != _wasEnabled) then {
			["enable", [AR_enabled]] call AR_fnc_ext;
			_wasEnabled = AR_enabled;
		};
		if (AR_model != _wasModel) then {
			["model", [AR_model]] call AR_fnc_ext;
			_wasModel = AR_model;
		};
		// TFAR radio sound (TeamSpeak plugin): independent of the world reverb switch and of being alive.
		["radio", [AR_radioEnhanced, AR_radioStatic]] call AR_fnc_ext;

		private _listener = call AR_fnc_listener;
		_listener params ["_pos", "_man", "_veh", "_free"];
		if (AR_enabled && {_free || {alive _man}}) then {

			if (_tick % _objEvery == 0) then {
				_trees = count nearestTerrainObjects [ASLToAGL _pos, ["TREE", "SMALL TREE"], _objRadius, false, true];
				_houses = count nearestTerrainObjects [ASLToAGL _pos, ["HOUSE", "BUILDING", "CHURCH", "CHAPEL", "BUNKER", "FORTRESS", "RUIN"], _objRadius, false, true];
			};

			private _scan = [_pos, _veh, _man] call AR_fnc_scan;
			([_scan, _trees, _houses, _listener] call AR_fnc_mix) params ["_values", "_weights", "_indoor", "_roomSize"];

			private _isIndoor = [0, 1] select (_indoor > 0.5);
			private _g = [_glide, _glideFast] select (_isIndoor != _wasIndoor);
			if (_wasIndoor < 0) then { _g = 0 };
			_wasIndoor = _isIndoor;

			if (!isNil "AR_override") then { _values = AR_override; _g = _glideFast };
			["env", _values + [_g]] call AR_fnc_ext;

			// Geometric echoes: quieter indoors (the room reverb carries it) and in closed vehicles.
			private _camVec = (positionCameraToWorld [0, 0, 1]) vectorDiff (positionCameraToWorld [0, 0, 0]);
			private _camAz = (_camVec select 0) atan2 (_camVec select 1);
			private _echoLevel = getNumber (_cfg >> "Echo" >> "level") * AR_intensity
				* (1 - _indoor * (1 - getNumber (_cfg >> "Echo" >> "indoorScale")))
				* (1 - (_weights getOrDefault ["VehicleClosed", 0]));
			if (_pos select 2 < 0 && {surfaceIsWater _pos}) then { _echoLevel = 0 };
			["taps", [_scan select 6, _camAz, _echoLevel] call AR_fnc_taps] call AR_fnc_ext;

			// Recorded impulse responses, blended by the same preset weights (convolution only).
			// AR_fnc_ir's single-file audition overrides this until cleared.
			if (AR_model == 2 && {isNil "AR_irOverride"}) then {
				private _mix = [];
				if (AR_irSource == 1) then {
					private _pairs = [];
					private _cabin = _weights getOrDefault ["VehicleClosed", 0];
					{
						// Other weights describe the outside; in a closed cabin they share what's left.
						private _w = if (_x == "VehicleClosed") then { _y } else { _y * (1 - _cabin) };
						if (_w > 0.05) then {
							_pairs pushBack [getText (_cfg >> "IRs" >> _x), round (_w * 20) / 20];
						};
					} forEach _weights;
					_pairs sort true; // stable order, so an unchanged blend compares equal
					{ _mix append _x } forEach _pairs;
				};
				if (isNil "AR_irLastMix" || {_mix isNotEqualTo AR_irLastMix}) then {
					["irmix", _mix] call AR_fnc_ext;
					AR_irLastMix = _mix;
				};
			};

			// TeamSpeak voice reverb (TFAR direct speech), same room as above.
			call AR_fnc_voice;

			AR_lastState = [_scan, _trees, _houses, _values, _weights, _indoor, _roomSize, _free];
		};

		_tick = _tick + 1;
		sleep _interval;
	};
};
