# Builds @AudioReverbProbe\addons\ar_probe.pbo from probe\ar_probe.
# Validates config.cpp with CfgConvert (Arma 3 Tools) first when it is installed.
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$src = Join-Path $root "probe\ar_probe"
$out = Join-Path $root "@AudioReverbProbe\addons\ar_probe.pbo"

$steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam").SteamPath
$cfgConvert = Join-Path $steam "steamapps\common\Arma 3 Tools\CfgConvert\CfgConvert.exe"
if (Test-Path $cfgConvert) {
    $tmp = Join-Path $env:TEMP "ar_probe_check.bin"
    & $cfgConvert -bin -dst $tmp (Join-Path $src "config.cpp")
    if ($LASTEXITCODE -ne 0) { throw "config.cpp failed CfgConvert validation" }
    Remove-Item $tmp -ErrorAction SilentlyContinue
    Write-Host "config.cpp OK"
}

python -I (Join-Path $root "tools\pack_pbo.py") $src $out "ar_probe"
if ($LASTEXITCODE -ne 0) { throw "packing failed" }
