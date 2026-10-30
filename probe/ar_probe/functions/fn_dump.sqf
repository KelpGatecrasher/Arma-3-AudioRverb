/*
	AR_fnc_dump
	Dumps the merged configFile >> CfgSoundEffects (vanilla + every loaded mod) to the RPT
	and the clipboard, and lists which mods touched each top-level effect class.

	Usage (debug console): [] call AR_fnc_dump;
*/

private _lines = [];

private _fnc_value = {
	params ["_c"];
	if (isNumber _c) exitWith { str getNumber _c };
	if (isText _c) exitWith { str getText _c };
	if (isArray _c) exitWith { str getArray _c };
	"?"
};

private _fnc_walk = {
	params ["_cfg", "_indent"];
	private _parent = inheritsFrom _cfg;
	private _header = format ["%1class %2", _indent, configName _cfg];
	if (!isNull _parent) then { _header = _header + ": " + configName _parent };
	_lines pushBack _header;
	_lines pushBack (_indent + "{");
	{
		if (isClass _x) then {
			[_x, _indent + "    "] call _fnc_walk;
		} else {
			_lines pushBack format ["%1    %2 = %3;", _indent, configName _x, [_x] call _fnc_value];
		};
	} forEach configProperties [_cfg, "true", false];
	_lines pushBack (_indent + "};");
};

private _root = configFile >> "CfgSoundEffects";
[_root, ""] call _fnc_walk;

_lines pushBack "";
_lines pushBack "// Mods that modified each effect group / class:";
{
	_lines pushBack format ["// %1 <- %2", configName _x, configSourceModList _x];
	private _group = _x;
	{
		_lines pushBack format ["//     %1 <- %2", configName _x, configSourceModList _x];
	} forEach ("true" configClasses _group);
} forEach ("true" configClasses _root);

diag_log "===== AR_Probe CfgSoundEffects dump BEGIN =====";
{ diag_log text _x } forEach _lines;
diag_log "===== AR_Probe CfgSoundEffects dump END =====";

private _text = _lines joinString endl;
copyToClipboard _text;
systemChat format ["AR_Probe: dumped %1 lines of CfgSoundEffects to RPT and clipboard.", count _lines];
_text
