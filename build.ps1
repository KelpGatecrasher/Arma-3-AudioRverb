# Builds the convolution variant into @AudioReverbConv:
#   AudioReverb_x64.dll   (extension, via build_extension.ps1)
#   addons\ar_main.pbo    (SQF + config; config validated with CfgConvert when available)
#   mod.cpp, irs\         (impulse response folder for AR_fnc_ir)
param([switch]$SkipExtension)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$modDir = Join-Path $root "@AudioReverbConv"

if (-not $SkipExtension) { & (Join-Path $root "build_extension.ps1") }

$steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam").SteamPath
$cfgConvert = Join-Path $steam "steamapps\common\Arma 3 Tools\CfgConvert\CfgConvert.exe"

foreach ($addon in Get-ChildItem (Join-Path $root "addons") -Directory) {
    $cfg = Join-Path $addon.FullName "config.cpp"
    if (Test-Path $cfgConvert) {
        $tmp = Join-Path $env:TEMP "$($addon.Name)_check.bin"
        & $cfgConvert -bin -dst $tmp $cfg
        if ($LASTEXITCODE -ne 0) { throw "$cfg failed CfgConvert validation" }
        Remove-Item $tmp -ErrorAction SilentlyContinue
    }
    $pbo = Join-Path $modDir "addons\$($addon.Name).pbo"
    python -I (Join-Path $root "tools\pack_pbo.py") $addon.FullName $pbo $addon.Name
    if ($LASTEXITCODE -ne 0) { throw "packing $($addon.Name) failed" }
}

Copy-Item (Join-Path $root "mod.cpp") $modDir -Force
New-Item -ItemType Directory -Force (Join-Path $modDir "irs") | Out-Null
Copy-Item (Join-Path $root "irs\*") (Join-Path $modDir "irs") -Recurse -Force
Write-Host "Built $modDir"
