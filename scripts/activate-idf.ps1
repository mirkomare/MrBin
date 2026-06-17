# Attiva ESP-IDF v5.5 (ESP32-P4) in questa sessione PowerShell.
# Uso:  . .\scripts\activate-idf.ps1

$ProfileScript = "C:\Espressif\tools\Microsoft.v5.5.PowerShell_profile.ps1"
if (-not (Test-Path $ProfileScript)) {
    Write-Error "Profilo ESP-IDF non trovato: $ProfileScript"
    return
}
. $ProfileScript
