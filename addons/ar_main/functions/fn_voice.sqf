/*
	AR_fnc_voice
	Feeds the TeamSpeak voice plugin (audioreverb_voice_win64.dll): for every player whose
	direct speech TFAR would let us hear, how strongly their voice excites our room reverb.
	Radio is never reverbed: a player far away gets gain 0 even while transmitting.

	Names match TFAR's TeamSpeak renaming (TFAR_unitName, else the unit's name). Distances are
	measured from TFAR's listening position (the unit's head, or the camera while spectating).

	Arguments: none. Returns: number of speakers sent.
*/

private _cfg = configFile >> "CfgAudioReverb" >> "Voice";
private _me = missionNamespace getVariable ["TFAR_currentUnit", player];
private _spectating = !alive _me || {_me getVariable ["TFAR_forceSpectator", false]};
private _ears = if (_spectating) then { AGLToASL (positionCameraToWorld [0, 0, 0]) } else {
	_me modelToWorldVisualWorld (_me selectionPosition "pilot")
};

// Closed vehicles isolate voices (TFAR muffles them too); open/turned-out ones don't.
private _fnc_closed = {
	params ["_unit"];
	private _veh = vehicle _unit;
	_veh != _unit && {!isTurnedOut _unit} && {
		private _att = toLower getText (configOf _veh >> "attenuationEffectType");
		_att != "" && {_att find "open" < 0}
	}
};
private _meClosed = [_me] call _fnc_closed;

private _range = getNumber (_cfg >> "range");
private _extra = getNumber (_cfg >> "cutoffExtra");
private _full = getNumber (_cfg >> "fullLevelFraction");
private _isolation = getNumber (_cfg >> "vehicleIsolation");

private _speakers = [];
{
	if (_x != _me && {alive _x} && {!(_x isKindOf "HeadlessClient_F")}) then {
		// Speakers' whisper/normal/yell range isn't synced to other clients; TFAR's public
		// tf_voiceVolume multiplier is, so the normal range is scaled by it.
		private _cut = _range * (_x getVariable ["tf_voiceVolume", 1]) + _extra;
		private _d = _ears distance (_x modelToWorldVisualWorld (_x selectionPosition "pilot"));
		if (_d < _cut) then {
			// A room's reverberant level barely drops with distance, so full level out to
			// fullLevelFraction of the range, then fade to TFAR's own cutoff.
			private _g = 1 - (((_d - _full * _cut) / ((1 - _full) * _cut)) max 0);
			if (vehicle _x != vehicle _me && {_meClosed || {[_x] call _fnc_closed}}) then { _g = _g * _isolation };
			if (_g > 0.01) then {
				_speakers append [_x getVariable ["TFAR_unitName", name _x], _g];
			};
		};
	};
} forEach allPlayers;

private _self = [0, AR_voiceSelfLevel] select AR_voiceSelf;
["voice", [AR_voiceEnabled, AR_voiceLevel, _self] + _speakers] call AR_fnc_ext;
AR_lastVoiceSpeakers = count _speakers / 2;
AR_lastVoiceSpeakers
