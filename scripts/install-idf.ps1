# Installa/aggiorna ESP-IDF via EIM (config in eim_config.toml).
param(
    [string]$Version = "v5.5.4"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$EimJsonPath = "C:\Espressif\tools\eim_idf.json"
$EimExe = $null

if (Test-Path $EimJsonPath) {
    $eimCfg = Get-Content $EimJsonPath -Raw | ConvertFrom-Json
    if ($eimCfg.eimPath -and (Test-Path $eimCfg.eimPath)) {
        $EimExe = $eimCfg.eimPath
    }
}
if (-not $EimExe) {
    $wingetEim = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Filter "eim.exe" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($wingetEim) { $EimExe = $wingetEim.FullName }
}
if (-not $EimExe) {
    throw "EIM non trovato. Installare con: winget install Espressif.EIM-CLI"
}

$config = Join-Path $Root "eim_config.toml"
if (-not (Test-Path $config)) {
    $example = Join-Path $Root "eim_config.toml.example"
    if (Test-Path $example) {
        Copy-Item $example $config
        Write-Host "Creato eim_config.toml da eim_config.toml.example"
    }
}
Write-Host "Installazione ESP-IDF $Version con EIM..."
& $EimExe install -c $config -n true -i $Version -t esp32p4 -p "C:\esp" --version-name $Version --log-file (Join-Path $Root "eim-install.log")

Write-Host "Selezione versione $Version..."
& $EimExe select $Version --esp-idf-json-path (Split-Path $EimJsonPath -Parent)

Write-Host "Fatto. Riapri il terminale o esegui:  . .\scripts\activate-idf.ps1"
