/*
	AR_fnc_status
	Toggles an on-screen readout of the audio state that decides whether HouseAttenuation applies:
	insideBuilding, the building overhead and its building_interior audio feature.

	Usage (debug console): [] call AR_fnc_status;   (call again to stop)
*/

if (!isNil "AR_probe_statusHandle") exitWith {
	terminate AR_probe_statusHandle;
	AR_probe_statusHandle = nil;
	hintSilent "";
	systemChat "AR_Probe: status off.";
};

AR_probe_statusHandle = [] spawn {
	while {true} do {
		private _eye = eyePos player;
		private _hits = lineIntersectsSurfaces [_eye, _eye vectorAdd [0, 0, 50], player, objNull, true, 1, "GEOM", "NONE"];
		private _roof = objNull;
		private _roofDist = -1;
		if (_hits isNotEqualTo []) then {
			_roof = (_hits select 0) select 3;
			_roofDist = (_hits select 0 select 0) distance _eye;
		};

		// The binary form returns the previous state, so set-and-restore reads it without changing it.
		private _interior = "n/a";
		if (!isNull _roof) then {
			private _prev = _roof enableAudioFeature ["building_interior", true];
			_roof enableAudioFeature ["building_interior", _prev];
			_interior = str _prev;
		};

		hintSilent format [
			"AR Probe\n\ninsideBuilding: %1\nvehicle: %2\nroof object: %3\nroof dist: %4 m\nbuilding_interior: %5",
			insideBuilding player,
			typeOf vehicle player,
			if (isNull _roof) then {"none"} else {typeOf _roof},
			_roofDist toFixed 1,
			_interior
		];
		sleep 0.5;
	};
};
systemChat "AR_Probe: status on.";
