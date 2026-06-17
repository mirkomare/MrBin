# Changelog — MrBin

## [0.2.0] — 2026-06-17

### Aggiunto
- Firmware **MrBin CORE** (`src/mrbin_core/`) per Waveshare ESP32-P4-WIFI6-M
- Flusso PIR/TPL5111: D1 (GPIO28), D2 (GPIO21), DONE (GPIO23)
- Registrazione H.264 1080p30 su SD con naming `DDMMAAAA/ID_DDMM_hhmmss.mp4`
- Cifratura AES-128 file video, ID CORE 5 cifre e chiave persistenti in NVS
- Web GUI porta 1510: login, impostazioni, format SD, playback decifrato
- Documentazione `docs/CORE_FIRMWARE.md` e scheda `schede/esp32-p4-wifi6-m.md`

### Modificato
- Repository spostato da `c:/CURSOR/src/MrBin` a `c:/CURSOR/Mrbin`

## [0.1.0] — 2026-06-17

### Aggiunto
- Repository iniziale in `c:/CURSOR/Mrbin`
- Struttura cartelle: `src/`, `docs/`, `schede/`
- README e `.gitignore` per sviluppo Arduino/ESP-IDF
