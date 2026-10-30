/*
	AR_fnc_ext
	Calls the AudioReverb extension and parses its array result.

	Arguments: 0: function <STRING>, 1: arguments <ARRAY> (default [])
	Returns:   parsed result <ARRAY>, [] if the extension is missing or errored

	Example: ["status"] call AR_fnc_ext;
*/

params ["_function", ["_args", []]];

private _ret = "AudioReverb" callExtension [_function, _args];
_ret params [["_result", ""], ["_code", 0], ["_error", 0]];
if (_error != 0 || {_result isEqualTo ""}) exitWith {
	if (_error != 0) then { diag_log format ["[AR] callExtension %1 failed, error %2", _function, _error] };
	[]
};
parseSimpleArray _result
