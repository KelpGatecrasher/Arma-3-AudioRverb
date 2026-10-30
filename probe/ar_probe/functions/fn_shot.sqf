/*
	AR_fnc_shot
	Plays a dry (no baked tail) MX shot at a given distance in front of the player, so any
	echo/reverb heard comes from the engine effect chain rather than the sample itself.

	Usage (debug console): [20] call AR_fnc_shot;   // 20 m in front
	                       [20, 5, 0.3] spawn AR_fnc_shot;   // 5 shots, 0.3 s apart
*/

params [["_dist", 20], ["_count", 1], ["_gap", 0.3]];

private _pos = player getPos [_dist, getDir player];
_pos set [2, 1.5];
_pos = ATLToASL _pos;

for "_i" from 1 to _count do {
	playSound3D ["a3\sounds_f\arsenal\weapons\Rifles\MX\MX_closeShot_01.wss", objNull, false, _pos, 3, 1, 1500];
	if (_i < _count) then { sleep _gap };
};
