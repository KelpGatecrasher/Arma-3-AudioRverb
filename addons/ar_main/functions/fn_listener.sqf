/*
	AR_fnc_listener
	Who or what is hearing the world right now. The engine's listener is always the camera, so the
	scan follows the camera; unit-based checks (insideBuilding, vehicle cabin) only apply while
	the camera is actually on a unit, not in Zeus or another free camera.

	Returns: [cameraPosASL, unit or objNull, vehicle or objNull, freeCamera]
		unit     - the controlled unit (player, Zeus remote control, or a vehicle's commander for
		           UAV/turret views); objNull for free cameras
		vehicle  - vehicle unit (== unit when on foot); objNull for free cameras
*/

private _pos = AGLToASL (positionCameraToWorld [0, 0, 0]);
private _on = cameraOn;

// cameraOn is the controlled unit or vehicle (covers Zeus remote control and UAV views).
private _man = if (_on isKindOf "CAManBase") then { _on } else {
	if (vehicle player == _on) then { player } else { effectiveCommander _on }
};

private _free = !isNull curatorCamera
	|| {!isNull (missionNamespace getVariable ["BIS_DEBUG_CAM", objNull])}
	|| {!isNull (missionNamespace getVariable ["BIS_EGSpectatorCamera_camera", objNull])}
	|| {isNull _man}
	// Any other scripted camera: far from the unit (third-person cameras stay well within this).
	|| {_pos distance (getPosASL vehicle _man) > 60};

if (_free) exitWith { [_pos, objNull, objNull, true] };
[_pos, _man, vehicle _man, false]
