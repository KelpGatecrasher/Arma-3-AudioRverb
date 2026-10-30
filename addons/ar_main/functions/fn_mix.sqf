/*
	AR_fnc_mix
	Turns scan metrics into a reverb environment by blending CfgAudioReverb presets.

	Arguments:
		0: scan result from AR_fnc_scan <ARRAY>
		1: trees within objectRadius <NUMBER>
		2: houses within objectRadius <NUMBER>
		3: listener from AR_fnc_listener <ARRAY>
	Returns: [values <ARRAY of 13>, weights <HASHMAP preset -> weight>, indoor <NUMBER 0..1>, roomSize <NUMBER m>]
*/

params ["_scan", "_trees", "_houses", "_listener"];
_listener params ["_pos", "_man", "_veh", "_free"];
_scan params ["_hitFrac", "_nearFrac", "_nearDist", "_roofFrac", "_roofDist", "_structFrac"];

private _cfg = configFile >> "CfgAudioReverb";
private _presets = _cfg >> "Presets";
private _fnc_step = { params ["_a", "_b", "_x"]; ((_x - _a) / (_b - _a)) max 0 min 1 };

// --- Classify ------------------------------------------------------------------------------
// Indoors: a roof overhead and walls close around. insideBuilding is the engine's own guess for
// the unit, so it only counts while the camera is on that unit (not in Zeus / free cameras).
private _indoor = _roofFrac * ([0.25, 0.7, _nearFrac] call _fnc_step);
if (!_free) then { _indoor = _indoor max (insideBuilding _man) };
private _roomSize = (2 * _nearDist + _roofDist) / 3;

private _urban = ((_houses / getNumber (_cfg >> "housesForFullUrban")) max (_structFrac * 1.5)) min 1;
private _dense = _urban * ([0.3, 0.7, _hitFrac] call _fnc_step);
private _forest = (_trees / getNumber (_cfg >> "treesForFullForest")) min 1;

private _weights = createHashMap;
private _out = 1 - _indoor;
_weights set ["UrbanDense", _out * _dense];
_weights set ["Urban", _out * ((_urban - _dense) max 0)];
private _rest = _out * (1 - _urban);
_weights set ["Forest", _rest * _forest];
_weights set ["Open", _rest * (1 - _forest)];

// Indoor weight split between the two presets bracketing the room size.
private _sizes = getArray (_cfg >> "indoorSizes");
private _names = getArray (_cfg >> "indoorPresets");
private _last = count _sizes - 1;
if (_roomSize <= _sizes select 0) then {
	_weights set [_names select 0, _indoor];
} else {
	if (_roomSize >= _sizes select _last) then {
		_weights set [_names select _last, _indoor];
	} else {
		for "_i" from 0 to (_last - 1) do {
			private _a = _sizes select _i;
			private _b = _sizes select (_i + 1);
			if (_roomSize >= _a && _roomSize < _b) exitWith {
				private _t = [_a, _b, _roomSize] call _fnc_step;
				_weights set [_names select _i, _indoor * (1 - _t)];
				_weights set [_names select (_i + 1), _indoor * _t];
			};
		};
	};
};

// --- Blend ---------------------------------------------------------------------------------
private _values = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
{
	if (_y > 0) then {
		private _p = getArray (_presets >> _x >> "values");
		for "_i" from 0 to 12 do { _values set [_i, (_values select _i) + _y * (_p select _i)] };
	};
} forEach _weights;

// Closed vehicle cabin: mostly the cabin, a little of the outside environment.
if (!_free && {_veh != _man} && {!isTurnedOut _man}) then {
	private _att = toLower getText (configOf _veh >> "attenuationEffectType");
	if (_att != "" && {_att find "open" < 0}) then {
		private _k = getNumber (_cfg >> "vehicleClosedBlend");
		private _cab = getArray (_presets >> "VehicleClosed" >> "values");
		for "_i" from 0 to 12 do { _values set [_i, (_values select _i) * (1 - _k) + (_cab select _i) * _k] };
		_weights set ["VehicleClosed", _k];
	};
};

_values set [12, (_values select 12) * AR_intensity];
// Underwater: judged at the camera, which is where the engine listens.
if (_pos select 2 < 0 && {surfaceIsWater _pos}) then { _values set [12, 0] };

[_values, _weights, _indoor, _roomSize]
