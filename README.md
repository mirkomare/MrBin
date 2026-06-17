# MrBin

Progetto firmware e documentazione per le **schede dedicate MrBin** (CORE su Waveshare ESP32-P4-WIFI6-M).

Repository separato da [BLEmax-M](https://github.com/mirkomare/BLEmax-M): ogni modifica a hardware, sketch o configurazione board MrBin va aggiornata **qui**.

## Primo firmware: MrBin CORE

Device polling alimentato da **TPL5111** + **PIR**:
- Registrazione video **1080p 30fps** su SD (file cifrati AES-128)
- Segnali **D1/D2/DONE** su GPIO 28/21/23
- Web GUI porta **1510** per configurazione e visione filmati

| Documento | Contenuto |
|-----------|-----------|
| [docs/CORE_FIRMWARE.md](docs/CORE_FIRMWARE.md) | Build, flussi, struttura codice |
| [docs/CURSOR_ESP_IDF.md](docs/CURSOR_ESP_IDF.md) | **Cursor + estensione Espressif IDF** |
| [schede/esp32-p4-wifi6-m.md](schede/esp32-p4-wifi6-m.md) | Pinout e scheda Waveshare |

### Build rapida

```bash
cd src/mrbin_core
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor
```

## Struttura

```
MrBin/
├── README.md
├── CHANGELOG.md
├── docs/
├── schede/
└── src/
    └── mrbin_core/     # progetto ESP-IDF CORE
```

## Collegamento con BLEmax-M

MrBin CORE è un progetto satellite: integrazioni gateway BLE e profili device restano in **BLEmax-M**, salvo diversa indicazione.
