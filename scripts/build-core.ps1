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
    if (-not (Test-Path "sdkconfig")) {
        idf.py set-target esp32p4
    }
    idf.py build
    if ($Port) {
        idf.py -p $Port flash monitor
    }
}
finally {
    Pop-Location
}
