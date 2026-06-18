# Attiva ESP-IDF (versione selezionata in EIM) in questa sessione PowerShell.
# Uso:  . .\scripts\activate-idf.ps1

$EimJson = "C:\Espressif\tools\eim_idf.json"
if (-not (Test-Path $EimJson)) {
    Write-Error "eim_idf.json non trovato: $EimJson - esegui .\scripts\install-idf.ps1"
    return
}

$cfg = Get-Content $EimJson -Raw | ConvertFrom-Json
$selected = $cfg.idfInstalled | Where-Object { $_.id -eq $cfg.idfSelectedId } | Select-Object -First 1
if (-not $selected) {
    $selected = $cfg.idfInstalled | Select-Object -Last 1
}

$IdfPath = $selected.path
$ExportScript = Join-Path $IdfPath "export.ps1"
if (-not (Test-Path $ExportScript)) {
    Write-Error "export.ps1 non trovato: $ExportScript"
    return
}

$ToolsPath = if ($cfg.espIdfJsonPath) { $cfg.espIdfJsonPath } else { "C:\Espressif\tools" }
$PythonVenv = Join-Path $ToolsPath "python\$($selected.name)\venv"
$PythonExe = Join-Path $PythonVenv "Scripts\python.exe"

if (Test-Path $PythonExe) {
    $env:IDF_PYTHON_ENV_PATH = $PythonVenv
    $env:Path = (Split-Path $PythonExe -Parent) + ";" + $env:Path
} else {
    Write-Warning "Python venv non trovato: $PythonVenv - export.ps1 potrebbe fallire"
}

$env:IDF_PATH = $IdfPath
$env:IDF_TOOLS_PATH = $ToolsPath

Write-Host "ESP-IDF: $($selected.name) -> $IdfPath"
. $ExportScript
