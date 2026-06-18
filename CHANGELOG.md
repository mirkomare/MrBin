# Changelog

Formato basato su [Keep a Changelog](https://keepachangelog.com/it/1.0.0/).

## [0.1.0] - 2026-06-18 — stable

**Commit:** `5f9ffd0b103f632cb95317fa9fa0d139b13ac1f5`  
**Data/ora release:** 2026-06-18 02:12:27 +0200  
**Tag:** `v0.1.0`  
**Target:** Waveshare ESP32-P4-WIFI6-M (ESP-IDF 5.5.4, esp32p4 rev v1.x)

### Aggiunto

- Firmware **MrBin CORE** (`src/mrbin_core/`) per ESP32-P4 con ESP-IDF 5.5.4.
- Registrazione video **H264 hardware 1920×1080 @ 30 fps** su SD (MP4 temporaneo → cifratura AES-128 → `.enc`).
- **Web GUI** porta **1510** (login `mm` / `123456`): impostazioni, elenco video, riproduzione, formattazione SD.
- Pagina **Live**: anteprima **MJPEG hardware 1920×1080 @ 30 fps** via `multipart/x-mixed-replace` (compatibile browser).
- Moduli `CoreLive`, `CoreVideo` (MIPI-CSI OV5647), `CoreSD`, `CoreWeb`, `CoreRecorder`, `CoreAuth`, `CoreCrypto`.
- Partition table custom, `dependencies.lock`, `idf_component.yml` (esp_capture, esp_muxer, esp_video, esp_hosted).
- Script PowerShell: `build-core.ps1`, `activate-idf.ps1`, `install-idf.ps1`, `verify-workspace.ps1`, `patch-gmf-fft.ps1`.
- Documentazione aggiornata (`docs/CORE_FIRMWARE.md`, `docs/ESP-IDF_SETUP.md`, `docs/CURSOR_ESP_IDF.md`, scheda P4).

### Corretto

- **SDMMC:** slot **0** (GPIO 43/44, 39–42), alimentazione **LDO_VO4**, retry mount; risolto conflitto con ESP-Hosted (slot 1).
- **Rete / HTTP:** init stack di rete sempre attivo; fallback AP se SSID assente; fix panic `httpd` / `Invalid mbox`.
- **SD format UX:** overlay spinner, messaggio esito, stato SD in impostazioni.
- **Camera Live:** probe `/dev/video0`, messaggio errore se OV5647 assente (no immagine rotta).
- **Handler HTTP:** aumento `max_uri_handlers` e stack server per Live/stream.

### Rimosso

- Sketch legacy `mrbin_core.ino` (sostituito da ESP-IDF `mrbin_core_main.cpp`).
- `idf_component.yml` obsoleto nella root di `mrbin_core/` (spostato in `main/`).

### Note tecniche

- **Live:** MJPEG HW in browser; tentativi H264 live (JMuxer/mpegts/WebCodecs) non adottati per instabilità su HTTP embedded.
- **Registrazione:** H264 HW identico a `CORE_VIDEO_*` in `CoreConfig.h`.
- **Build:** `idf.py -p COM15 flash monitor` con ESP-IDF **v5.5.4** (non 6.x per compatibilità chip rev e componenti).

[0.1.0]: https://github.com/mirkomare/MrBin/releases/tag/v0.1.0
