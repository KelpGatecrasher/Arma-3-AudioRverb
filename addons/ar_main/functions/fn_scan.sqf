/*
	AR_fnc_scan
	Casts a fan of rays from the listener to measure the surrounding space.

	Arguments:
		0: listener position <ASL>
		1: vehicle to ignore <OBJECT> (the listener's own vehicle; objNull for free cameras)
		2: unit to ignore <OBJECT>
	Returns: [hitFrac, nearFrac, nearMeanDist, roofFrac, roofMeanDist, structFrac, slots]
		hitFrac       - share of horizontal/elevated rays that hit anything within rayLength
		nearFrac      - share that hit within nearDistance (enclosing walls)
		nearMeanDist  - mean distance of those near hits (m), nearDistance if none
		roofFrac      - share of upward rays that hit within roofLength
		roofMeanDist  - mean upward hit distance (m), roofLength if none
		structFrac    - share of horizontal hits that are man-made structures (Building kind)
		slots         - one entry per horizontal/elevated ray, always in the same order:
		                [distance (-1 = no hit within echoLength), worldAzimuth, elevation, isStructure]
		                AR_fnc_taps turns these into echo taps (slot = tap index).
*/

params ["_pos", ["_ign1", objNull], ["_ign2", objNull]];

private _cfg = configFile >> "CfgAudioReverb";
private _rayLen = getNumber (_cfg >> "rayLength");
private _echoLen = getNumber (_cfg >> "echoLength") max _rayLen;
private _roofLen = getNumber (_cfg >> "roofLength");
private _nearDist = getNumber (_cfg >> "nearDistance");

// Returns [distance, hit object or objNull, didHit]
private _fnc_cast = {
	params ["_dir", "_len"];
	private _hits = lineIntersectsSurfaces [_pos, _pos vectorAdd (_dir vectorMultiply _len), _ign1, _ign2, true, 1, "GEOM", "FIRE"];
	if (_hits isEqualTo []) exitWith { [_len, objNull, false] };
	(_hits select 0) params ["_hitPos", "", "_obj", "_parent"];
	[_hitPos distance _pos, [_obj, _parent] select (!isNull _parent), true]
};

private _rays = 0;
private _hits = 0;
private _near = 0;
private _nearSum = 0;
private _struct = 0;
private _slots = [];

// 12 horizontal + 8 at 35 degrees up. Cast to echoLength; classification only counts hits
// within rayLength so long echo rays don't make open ground look enclosed.
{
	_x params ["_count", "_elev"];
	for "_i" from 0 to (_count - 1) do {
		private _az = _i * 360 / _count;
		private _dir = [sin _az * cos _elev, cos _az * cos _elev, sin _elev];
		([_dir, _echoLen] call _fnc_cast) params ["_d", "_obj", "_hit"];
		private _isStruct = !isNull _obj && {_obj isKindOf "Building"};
		_slots pushBack [[-1, _d] select _hit, _az, _elev, _isStruct];

		_rays = _rays + 1;
		if (_hit && _d < _rayLen) then {
			_hits = _hits + 1;
			if (_d < _nearDist) then { _near = _near + 1; _nearSum = _nearSum + _d };
			if (_isStruct) then { _struct = _struct + 1 };
		};
	};
} forEach [[12, 0], [8, 35]];

// Straight up + 4 at 60 degrees
private _roofHits = 0;
private _roofSum = 0;
private _roofDirs = [[0, 0, 1]];
for "_i" from 0 to 3 do {
	private _az = 45 + _i * 90;
	_roofDirs pushBack [sin _az * cos 60, cos _az * cos 60, sin 60];
};
{
	([_x, _roofLen] call _fnc_cast) params ["_d", "", "_hit"];
	if (_hit) then { _roofHits = _roofHits + 1; _roofSum = _roofSum + _d };
} forEach _roofDirs;

// if/else rather than [a, b] select: both array elements are evaluated, and these can divide by zero.
[
	_hits / _rays,
	_near / _rays,
	if (_near > 0) then { _nearSum / _near } else { _nearDist },
	_roofHits / count _roofDirs,
	if (_roofHits > 0) then { _roofSum / _roofHits } else { _roofLen },
	if (_hits > 0) then { _struct / _hits } else { 0 },
	_slots
]
