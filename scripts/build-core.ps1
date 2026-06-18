# Build MrBin CORE (ESP32-P4)
param(
    [string]$Port = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$Project = Join-Path $Root "src\mrbin_core"

. (Join-Path $PSScriptRoot "activate-idf.ps1")

Push-Location $Project
try {
    $IdfPy = Join-Path $env:IDF_PATH "tools\idf.py"
    if (-not (Test-Path "sdkconfig")) {
        python $IdfPy set-target esp32p4
    }
    & (Join-Path $PSScriptRoot "patch-gmf-fft.ps1") -ProjectDir $Project
    python $IdfPy build
    if ($Port) {
        python $IdfPy -p $Port flash monitor
    }
}
finally {
    Pop-Location
}
