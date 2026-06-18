# MrBin CORE — progetto ESP-IDF

Firmware **MrBin CORE** per [Waveshare ESP32-P4-WIFI6-M](../../schede/esp32-p4-wifi6-m.md).

## Prerequisiti

- ESP-IDF **v5.5+** con target `esp32p4`
- Componenti gestiti da `main/idf_component.yml` (`esp_capture`, `esp_video`, `esp_muxer`, …)

## Build rapida

```powershell
# Dalla root Mrbin
.\scripts\build-core.ps1
.\scripts\build-core.ps1 -Port COM5
```

```bash
cd src/mrbin_core
idf.py set-target esp32p4   # prima volta
idf.py build
idf.py -p COMx flash monitor
```

## Struttura

```
mrbin_core/
├── CMakeLists.txt          # root progetto ESP-IDF
├── sdkconfig.defaults      # target P4, OV5647, Wi-Fi remote, PSRAM
├── partitions.csv
├── dependencies.lock       # lock component manager (generato)
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml   # dipendenze managed components
│   ├── mrbin_core_main.cpp # app_main
│   └── Core*.cpp           # moduli firmware
└── managed_components/     # scaricati da idf.py (gitignored)
```

## Workspace Cursor

Apri `MrBin.code-workspace` dalla root del repository. Vedi [CURSOR_ESP_IDF.md](../../docs/CURSOR_ESP_IDF.md).

Verifica ambiente:

```powershell
cd C:\CURSOR\Mrbin
.\scripts\verify-workspace.ps1
.\scripts\verify-workspace.ps1 -Build
```

## Documentazione

- [CORE_FIRMWARE.md](../../docs/CORE_FIRMWARE.md) — flussi, moduli, formato file
- [ESP-IDF_SETUP.md](../../docs/ESP-IDF_SETUP.md) — ambiente Windows
- [CURSOR_ESP_IDF.md](../../docs/CURSOR_ESP_IDF.md) — estensione Espressif in Cursor
