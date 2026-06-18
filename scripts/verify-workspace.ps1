# Verifica ambiente MrBin CORE (ESP-IDF, Python, progetto, build opzionale).
param(
    [switch]$Build,
    [switch]$Flash,
    [string]$Port = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$Project = Join-Path $Root "src\mrbin_core"
$failures = @()

function Test-ItemPath {
    param([string]$Label, [string]$Path, [switch]$Optional)
    if (Test-Path $Path) {
        Write-Host "[OK]   $Label" -ForegroundColor Green
        return $true
    }
    if ($Optional) {
        Write-Host "[WARN] $Label - non presente: $Path" -ForegroundColor Yellow
        return $false
    }
    $msg = "[FAIL] $Label - mancante: $Path"
    Write-Host $msg -ForegroundColor Red
    $script:failures += $msg
    return $false
}

Write-Host ""
Write-Host "=== MrBin workspace check ===" -ForegroundColor Cyan
Write-Host ""

$null = Test-ItemPath "Repository root" $Root
$null = Test-ItemPath "Workspace Cursor" (Join-Path $Root "MrBin.code-workspace")
$null = Test-ItemPath "Progetto CORE" $Project
$null = Test-ItemPath "CMakeLists root" (Join-Path $Project "CMakeLists.txt")
$null = Test-ItemPath "Component manifest" (Join-Path $Project "main\idf_component.yml")
$null = Test-ItemPath "sdkconfig.defaults" (Join-Path $Project "sdkconfig.defaults")
$null = Test-ItemPath "EIM config" (Join-Path $Root "eim_config.toml") -Optional
$null = Test-ItemPath "EIM registry" "C:\Espressif\tools\eim_idf.json"

$eimOk = $false
if (Test-Path "C:\Espressif\tools\eim_idf.json") {
    $eim = Get-Content "C:\Espressif\tools\eim_idf.json" -Raw | ConvertFrom-Json
    $selected = $eim.idfInstalled | Where-Object { $_.id -eq $eim.idfSelectedId } | Select-Object -First 1
    if (-not $selected) { $selected = $eim.idfInstalled | Select-Object -Last 1 }
    if ($selected) {
        Write-Host "[OK]   ESP-IDF selezionato: $($selected.name) -> $($selected.path)" -ForegroundColor Green
        $null = Test-ItemPath "ESP-IDF tree" $selected.path
        $toolsRoot = if ($eim.espIdfJsonPath) { $eim.espIdfJsonPath } else { "C:\Espressif\tools" }
        $py = Join-Path $toolsRoot "python\$($selected.name)\venv\Scripts\python.exe"
        $null = Test-ItemPath "Python venv IDF" $py
        $eimOk = $true
    }
}

$null = Test-ItemPath "Build script" (Join-Path $Root "scripts\build-core.ps1")
$null = Test-ItemPath "Activate script" (Join-Path $Root "scripts\activate-idf.ps1")

$binPath = Join-Path $Project "build\mrbin_core.bin"
if (Test-Path $binPath) {
    $bin = Get-Item $binPath
    $sizeMb = [math]::Round($bin.Length / 1MB, 2)
    Write-Host "[OK]   Firmware compilato: mrbin_core.bin ($sizeMb MB, $($bin.LastWriteTime))" -ForegroundColor Green
} else {
    Write-Host "[WARN] Nessuna build locale (build\mrbin_core.bin assente)" -ForegroundColor Yellow
}

if ($eimOk) {
    Write-Host ""
    Write-Host "--- Attivazione ESP-IDF ---" -ForegroundColor Cyan
    . (Join-Path $Root "scripts\activate-idf.ps1")
    $ver = (& idf.py --version 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -eq 0 -and $ver) {
        Write-Host "[OK]   idf.py: $ver" -ForegroundColor Green
    } else {
        $msg = "[FAIL] idf.py non disponibile dopo activate-idf.ps1"
        Write-Host $msg -ForegroundColor Red
        $failures += $msg
    }
}

if ($Build -and $failures.Count -eq 0) {
    Write-Host ""
    Write-Host "--- Build CORE ---" -ForegroundColor Cyan
    & (Join-Path $Root "scripts\build-core.ps1") -Port $Port
    if ($LASTEXITCODE -ne 0) {
        $failures += "[FAIL] build-core.ps1 exit code $LASTEXITCODE"
    } elseif (Test-Path $binPath) {
        Write-Host "[OK]   Build completata" -ForegroundColor Green
    }
} elseif ($Flash -and $Port -and (Test-Path $binPath)) {
    Write-Host ""
    Write-Host "--- Flash su $Port ---" -ForegroundColor Cyan
    Push-Location $Project
    try {
        . (Join-Path $Root "scripts\activate-idf.ps1")
        python (Join-Path $env:IDF_PATH "tools\idf.py") -p $Port flash monitor
    } finally {
        Pop-Location
    }
}

Write-Host ""
if ($failures.Count -eq 0) {
    Write-Host "=== Workspace pronto ===" -ForegroundColor Green
    Write-Host "Build:  .\scripts\build-core.ps1"
    Write-Host "Flash:  .\scripts\build-core.ps1 -Port COMx"
    exit 0
}

Write-Host "=== $($failures.Count) problema/i ===" -ForegroundColor Red
$failures | ForEach-Object { Write-Host $_ -ForegroundColor Red }
Write-Host ""
Write-Host "Ripara con: .\scripts\install-idf.ps1" -ForegroundColor Yellow
exit 1
