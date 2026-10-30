/*
	AR_fnc_taps
	Turns scanner ray hits into geometric echo taps for the extension's TapEcho effect.

	Each ray that hits a surface becomes one echo:
		delay  = 2 * distance / speed of sound
		gain   = reflectivity * spreading loss over the 2*distance path
		pan    = direction of the surface relative to where the camera faces
		damp   = high-frequency loss from air absorption and head shadow (surfaces behind you)

	Arguments:
		0: slots from AR_fnc_scan <ARRAY>
		1: camera azimuth <NUMBER> (degrees, same convention as getDir)
		2: level multiplier <NUMBER> (indoor/vehicle/intensity scaling, 0 = silent)
	Returns: flat [delay, gainL, gainR, damp, ...] in slot order, for ["taps", ...] call AR_fnc_ext
*/

params ["_slots", "_camAz", "_level"];

private _cfg = configFile >> "CfgAudioReverb" >> "Echo";
private _minDist = getNumber (_cfg >> "minDistance");
private _refDist = getNumber (_cfg >> "referenceDistance");
private _reflStruct = getNumber (_cfg >> "reflectStructure");
private _reflOther = getNumber (_cfg >> "reflectOther");
private _elevWeight = getNumber (_cfg >> "elevatedWeight");
private _baseDamp = getNumber (_cfg >> "baseDamp");
private _airDamp = getNumber (_cfg >> "airDampPer100m");
private _rearDamp = getNumber (_cfg >> "rearDamp");
private _speed = 343;

// More reflectors -> more echoes, but sum them sub-linearly so a courtyard isn't deafening.
private _hitCount = { (_x select 0) >= _minDist } count _slots;
private _norm = _level / sqrt ((_hitCount / 4) max 1);

private _out = [];
{
	_x params ["_d", "_az", "_elev", "_isStruct"];
	if (_d < _minDist || _level <= 0) then {
		_out append [0, 0, 0, 0];
	} else {
		private _g = ([_reflOther, _reflStruct] select _isStruct) * _refDist / (_refDist + 2 * _d) * _norm;
		if (_elev > 0) then { _g = _g * _elevWeight };

		private _rel = _az - _camAz;
		private _pan = sin _rel;    // -1 left .. +1 right
		private _front = cos _rel;  // +1 ahead .. -1 behind
		private _gL = _g * sqrt (0.5 * (1 - _pan));
		private _gR = _g * sqrt (0.5 * (1 + _pan));
		private _damp = (_baseDamp + _airDamp * _d / 100 + _rearDamp * ((-_front) max 0)) min 0.95;

		_out append [2 * _d / _speed, _gL, _gR, _damp];
	};
} forEach _slots;
_out
