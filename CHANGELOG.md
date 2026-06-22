# Changelog

Formato basato su [Keep a Changelog](https://keepachangelog.com/it/1.0.0/).

## [0.2.0-beta] - 2026-06-22 — Beta: GPIO29 config, fast-boot PSRAM, Web condizionale, LED errore

**Commit:** `PLACEHOLDER`  
**Data/ora release:** 2026-06-22 13:18:37 +0200  
**Tag:** `v0.2.0-beta`  
**Stato:** **Beta** — funzionalità nuove, da validare su hardware reale (registrazione PSRAM→SD, interruttore GPIO29, LED GPIO52).  
**Target:** Waveshare ESP32-P4-WIFI6-M (ESP-IDF 5.5.4, esp32p4 rev v1.x)  
**Versione firmware:** `src/mrbin_core/VERSION` → `0.2.0-beta`

### Aggiunto

#### Selettore modalità boot — GPIO29 (`CORE_GPIO_MODE_CFG`)
- **GPIO29 HIGH al boot** → modalità **configurazione manuale**: avvio **WiFi STA** (se SSID in NVS) + **Web GUI** porta 1510; **D2 (GPIO21) ignorato** finché GPIO29 resta HIGH; uscita con **DONE immediato** (GPIO23 HIGH, delay 0 ms) non appena GPIO29 va **LOW** → spegnimento TPL5111.
- **GPIO29 LOW al boot** → modalità **PIR normale** (registrazione solo se **D1 GPIO28 LOW**).
- Input con **pull-down interno**; interruttore esterno **GND** (PIR) / **3,3 V** (config) — mai tensione > 3,3 V sul pin.
- Log esteso `core_gpio_log_inputs()` include stato MODE.

#### LED errore boot PIR — GPIO52 (`CORE_GPIO_STATUS_LED`)
- Se boot PIR (**GPIO29 LOW**) senza **D1 attivo**: **lampeggio rapido** ~5 Hz (100 ms on/off) su **GPIO52** invece di attesa silenziosa.
- LED **active-high** (`CORE_STATUS_LED_ON_LEVEL = 1`); cablaggio previsto: GPIO52 → R → LED → GND + **pull-down esterno** sul pin (header 2×20 alto-sinistra).
- Funzione `core_gpio_blink_error_forever()` — loop non ritorna (TPL resta acceso finché non si corregge il wake).

#### Registrazione fast-boot — buffer H264 in PSRAM + SD in parallelo
- Nuovo modulo **`CoreH264Buffer`** (`CoreH264Buffer.cpp/h`): coda frame H264 in **PSRAM** (max **12 MB**), estrazione SPS/PPS, rilevamento IDR.
- **`CoreRecorder` riscritto** per architettura fast-boot:
  1. Task FreeRTOS **`sd_prep`**: `core_sd_init()`, verifica spazio (50 MB min), creazione path giorno/file `.tmp`.
  2. **Capture H264 immediato** in PSRAM (senza attendere SD).
  3. Al mount SD + path OK: **replay buffer → MP4** via `esp_muxer` manuale, poi frame live su SD fino a **D2**.
  4. Stop → chiusura muxer → cifratura AES-128 → file finale.
- Log diagnostici: tempi ms dal boot, `SD prep completata (mount=… paths=…)`, riepilogo `capture/sd/mux/enc`.
- Costanti in `CoreConfig.h`: `CORE_H264_PSRAM_MAX_BYTES`, `CORE_SD_PREP_TASK_*`.

#### Ottimizzazioni boot (`sdkconfig.defaults`)
- `CONFIG_BOOTLOADER_LOG_LEVEL_NONE=y` — bootloader silenzioso.
- `CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON=y` — salta validazione app al power-on (boot più rapido).
- `CONFIG_SPIRAM_MEMTEST=n` — niente memtest PSRAM al boot.

#### Versioning
- File **`src/mrbin_core/VERSION`** con stringa `0.2.0-beta`.

### Modificato

#### `mrbin_core_main.cpp` — routing boot
- Priorità **GPIO29** rispetto a D1: config manuale vs PIR.
- Rimosso avvio Web automatico su “boot senza D1” (sostituito da GPIO29 HIGH).
- Rimosso `core_sd_init()` bloccante pre-recorder in main (SD gestita in task parallelo nel recorder).
- Funzioni `run_config_mode()` / `run_pir_mode()`.

#### `CoreWeb.cpp` — Web solo con WiFi configurato
- **Web GUI e HTTP server** partono **solo** se SSID salvato in NVS **e** `wifi_start_sta()` riesce.
- **Rimosso AP di fallback** `MrBin-XXXXX` / password `12345678`.
- Senza SSID: nessuno stack HTTP, nessuna rete avviata da `core_web_start()`.

#### `CoreGPIO.cpp/h`
- `core_gpio_is_mode_config()` — lettura GPIO29.
- `core_gpio_is_d2_end()` restituisce sempre **false** se GPIO29 HIGH (D2 ignorato in config).
- Init separato pull-up (D1/D2) e pull-down (GPIO29).

#### Workspace / build
- **`MrBin.code-workspace`**: ESP-IDF ripristinato a **v5.5.4** (era erroneamente 6.0.1); path Python/venv allineati.

#### `CMakeLists.txt`
- Aggiunto `CoreH264Buffer.cpp` al componente `main`.

### Rimosso

- Modalità “config implicita” al boot senza D1 e senza GPIO29 (non più Web automatica).
- AP WiFi di emergenza in assenza di SSID.

### GPIO — mappa aggiornata MrBin CORE

| GPIO | Segnale | Boot / runtime |
|------|---------|----------------|
| 21 | D2 | LOW = fine registrazione (ignorato se GPIO29 HIGH) |
| 23 | DONE | HIGH = spegnimento TPL5111 |
| 28 | D1 | LOW = wake PIR / registrazione |
| 29 | MODE | HIGH = config WiFi/Web; LOW = PIR |
| 52 | LED errore | Lampeggio se PIR senza D1 |

### Note Beta / test consigliati

1. **GPIO29 HIGH** → verificare Web su `:1510`, ignorare D2, spegnere abbassando GPIO29 (DONE).
2. **GPIO29 LOW + D1 LOW** → registrazione; controllare log `Passaggio PSRAM→SD` e file su SD (cartella data 1970 se no RTC).
3. **GPIO29 LOW + D1 non LOW** → lampeggio GPIO52.
4. **SSID vuoto + GPIO29 HIGH** → nessuna Web; attende GPIO29 LOW e DONE.
5. Build: `idf.py fullclean build` con **ESP-IDF 5.5.4** dopo aggiornamento workspace.

### Problemi noti (Beta)

- Soglia **50 MB liberi** su SD può bloccare `paths_ok` su schede piccole o piene.
- Path fast-boot PSRAM→muxer dipende da SPS/PPS nel buffer; sessioni molto brevi possono non produrre file.
- Documentazione `docs/CORE_FIRMWARE.md` non ancora aggiornata a GPIO29/GPIO52 (da fare in release stable).

## [0.1.1] - 2026-06-18 — bugfix registrazione / DONE / SD

**Commit:** `5c579ba693d2290bf401bae73da91209435404ca`  
**Data/ora release:** 2026-06-19 15:04:49 +0200  
**Tag:** `v0.1.1`  
**Target:** Waveshare ESP32-P4-WIFI6-M (ESP-IDF 5.5.4, esp32p4 rev v1.x)

### Corretto

- **DONE (GPIO23) sempre emesso** dopo ricezione **D2 LOW (GPIO21)**, anche se registrazione o cifratura falliscono — necessario per spegnimento **TPL5111** e consumo batteria.
- **Attesa D2 obbligatoria:** la sessione PIR non esce più prima di D2 se camera/`esp_capture`/SD falliscono all'avvio (prima usciva subito → niente DONE).
- **`CoreRecorder`:** refactor `recorder_capture_start/stop`; validazione MP4 temporaneo (min 1 KB); log `errno` su apertura/scrittura file; riepilogo finale `reg/mp4/cifrato`.
- **`CoreGPIO`:** `core_gpio_log_inputs()` e `core_gpio_hold_tpl_done()` (delay configurabile + DONE HIGH permanente).
- **`mrbin_core_main`:** fallback DONE se la sessione ritorna; niente passaggio silenzioso a Web GUI dopo boot con **D1 attivo**.
- **Elenco file SD (FAT):** `CoreWeb` e `CoreSD` usano `stat()` oltre a `d_type` — su FAT `DT_UNKNOWN` nascondeva file e directory esistenti nella Web GUI.
- **Cifratura:** rifiuto MP4 temporaneo assente o troppo piccolo prima di AES-128-CTR; log errori scrittura payload.

### Note

- I file registrati in modalità PIR senza NTP restano in cartelle data **1970** (`/sdcard/DDMMYYYY/` con RTC non sincronizzato) — comportamento invariato, ora visibili in GUI se presenti.
- Build consigliata: ESP-IDF **v5.5.4** (`src/mrbin_core/.vscode/settings.json`).

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

[0.2.0-beta]: https://github.com/mirkomare/MrBin/releases/tag/v0.2.0-beta
[0.1.1]: https://github.com/mirkomare/MrBin/releases/tag/v0.1.1
[0.1.0]: https://github.com/mirkomare/MrBin/releases/tag/v0.1.0
