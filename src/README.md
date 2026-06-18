# Firmware MrBin — ESP-IDF

Cartella dei **progetti ESP-IDF** per le schede MrBin.

## Progetto attuale

| Progetto | Target | Descrizione |
|----------|--------|-------------|
| [`mrbin_core/`](mrbin_core/) | ESP32-P4 (Waveshare ESP32-P4-WIFI6-M) | CORE polling PIR/TPL5111, camera H.264, SD cifrata, web GUI |

## Build

```powershell
cd C:\CURSOR\Mrbin
.\scripts\build-core.ps1
```

Oppure manualmente:

```bash
cd src/mrbin_core
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor
```

Vedi [docs/CORE_FIRMWARE.md](../docs/CORE_FIRMWARE.md) e [docs/ESP-IDF_SETUP.md](../docs/ESP-IDF_SETUP.md).
