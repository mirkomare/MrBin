# Patch gmf_fft solo su ESP-IDF < 5.5.2 (bug assembler PIE su ESP32-P4).
# Con v5.5.4+ non serve.

param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectDir
)

$idfVer = (& idf.py --version 2>&1 | Out-String)
if ($idfVer -match 'v5\.5\.([4-9]|[1-9][0-9])' -or $idfVer -match 'v5\.[6-9]\.' -or $idfVer -match 'v6\.') {
    Write-Host "patch-gmf-fft: skip (ESP-IDF $idfVer)"
    return
}
$cmakePath = Join-Path $ProjectDir "managed_components\espressif__gmf_fft\CMakeLists.txt"
if (-not (Test-Path $cmakePath)) {
    Write-Host "patch-gmf-fft: gmf_fft non presente, skip (verra' applicato dopo idf.py set-target)"
    return
}

$patched = @'
# Patched by MrBin scripts/patch-gmf-fft.ps1 — C fallback instead of PIE assembly on ESP32-P4
set(GMF_FFT_SRCS
    "src/common/fft_heap.c"
    "src/common/fft_power2_q15.c"
    "src/esp_c/gmf_fft_radix2_dit_s16.c")

idf_component_register(
    SRCS ${GMF_FFT_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES heap
)
'@

Set-Content -Path $cmakePath -Value $patched -Encoding utf8
Write-Host "patch-gmf-fft: CMakeLists gmf_fft -> implementazione C"
