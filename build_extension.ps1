# Builds extension\ -> @AudioReverbConv\AudioReverb_x64.dll using the VS 2019 Build Tools' CMake.
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "No Visual Studio installation with the C++ x64 toolset found" }
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ver = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property catalog_productLineVersion
$gen = @{ "2019" = "Visual Studio 16 2019"; "2022" = "Visual Studio 17 2022" }[$ver]

$build = Join-Path $root "extension\build"
& $cmake -S (Join-Path $root "extension") -B $build -G $gen -A x64
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
& $cmake --build $build --config $Config
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$dst = Join-Path $root "@AudioReverbConv"
New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item (Join-Path $build "$Config\AudioReverb_x64.dll") $dst -Force
Write-Host "-> $dst\AudioReverb_x64.dll"

# TeamSpeak voice plugin, packaged as a .ts3_plugin (a zip: package.ini + plugins\*.dll) that
# TeamSpeak's installer opens on double-click, like TFAR's.
$tsDir = Join-Path $dst "teamspeak"
$pluginDll = Join-Path $build "$Config\audioreverb_voice_win64.dll"
python -I (Join-Path $root "tools\pack_ts3_plugin.py") (Join-Path $root "voice\package.ini") $pluginDll (Join-Path $tsDir "AudioReverbVoice.ts3_plugin")
if ($LASTEXITCODE -ne 0) { throw "packing the TeamSpeak plugin failed" }
Copy-Item $pluginDll $tsDir -Force
Copy-Item (Join-Path $root "voice\INSTALL.txt") $tsDir -Force
Write-Host "-> $tsDir\AudioReverbVoice.ts3_plugin"
