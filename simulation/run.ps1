param(
    [switch]$RebuildFirmware,
    [switch]$RebuildSim,
    [switch]$Borderless,
    [switch]$Serial,
    [int]$Scale = 2
)

$ErrorActionPreference = "Stop"
$SimDir  = $PSScriptRoot
$RootDir = Split-Path -Parent $SimDir
$FwBin   = Join-Path $RootDir ".pio\build\esp32-s3-devkitc-1\firmware.bin"
$SimExe  = Join-Path $SimDir "target\release\mydeskmate-sim.exe"

if ($RebuildFirmware -or -not (Test-Path $FwBin)) {
    $pio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
    & $pio run -d $RootDir
}

if ($RebuildSim -or -not (Test-Path $SimExe)) {
    & cargo build --release --manifest-path (Join-Path $SimDir "Cargo.toml") -p mydeskmate-sim
}

$argsList = @("--scale", $Scale)
if ($Borderless) { $argsList += "--borderless" }
if ($Serial)     { $argsList += "--serial" }

& $SimExe @argsList
