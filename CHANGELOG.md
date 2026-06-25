# Changelog

Formato basato su [Keep a Changelog](https://keepachangelog.com/it/1.0.0/).

**Versioning:** se non indicato diversamente, incrementare sempre l’**ultimo numero** (patch: `0.3.0` → `0.3.1`). Minor/major solo su richiesta esplicita.

## [0.3.1] - 2026-06-25 — Patch: registrazione web, video cifrati MRBI, performance I/O

**Commit:** `b9329ae6c5df8848f80cf2babceac11d240dcb86`  
**Data/ora release:** 2026-06-25 03:45:23 +0200  
**Tag:** `v0.3.1`  
**Stato:** **Stable** (patch) — registrazione manuale da Web GUI, gestione video cifrati su SD, ottimizzazioni I/O cifratura/decifratura.  
**Target:** Waveshare ESP32-P4-WIFI6-M (ESP-IDF 5.5.4, esp32p4 rev v1.x)  
**Versione firmware:** `src/mrbin_core/VERSION` → `0.3.1`

### Aggiunto

#### Registrazione manuale da Web (`CoreRecorder`, `CoreWeb`)
- API **`core_recorder_manual_*`**: start, stop, wait, status con frame count e percorso ultimo file.
- Pagina **Live**: pulsanti **Registra** / **Stop**, polling `/live/record/status`, live MJPEG sospeso durante registrazione H264 su SD.
- Endpoint HTTP: `POST /live/record/start`, `POST /live/record/stop`, `GET /live/record/status`.
- **Cifratura in background** (`enc_mp4` task) dopo stop web: risposta HTTP al termine del mux, cifratura AES continua in task separato.
- Modalità **PIR (D2)** invariata: cifratura **sincrona** prima del segnale TPL DONE.

#### Pagina Video e streaming decifrato (`CoreWeb`)
- Elenco SD con **dimensione file**, stato **Elaborazione…** per `.mp4.tmp`, avviso SD non montata.
- **Riproduzione browser** (`/videos/watch` + `/play?f=…`) e **download MP4 decifrato** (`/videos/download?f=…`).
- **Eliminazione singolo file** (`POST /videos/delete`).
- Formato file: **`[MRBI 4B][IV 16B][MP4 AES-128-CTR cifrato]`** — decifratura on-the-fly in stream HTTP.
- URL query con **`%2F`** per path annidati (`url_encode_query` / `url_decode`).

#### Formato e cifratura MP4 (`CoreRecorder`, `CoreCrypto`)
- Flusso affidabile: mux **plain** su `{nome}.mp4.tmp` → **`encrypt_mp4_file()`** → `{nome}.mp4` finale.
- Validazione **`ftyp`** sui primi 512 byte del tmp prima della cifratura.
- **`core_crypto_deinit_ctx()`** e contesto **`mbedtls_aes`** persistente nel ctx (niente re-init per chunk).
- Costanti buffer in `CoreConfig.h`: `CORE_MUXER_RAM_CACHE_BYTES` (512 KB), `CORE_REC_ENCRYPT_IO_BYTES` (512 KB), `CORE_CRYPTO_STREAM_IO_BYTES` (64 KB), `CORE_SD_FILE_BUF_BYTES` (32 KB).

#### LED registrazione (`CoreStatusLed`)
- Modalità **`CORE_LED_RECORDING`**: LED **acceso fisso** durante registrazione (PIR e manuale).

#### Tooling sviluppo Cursor / ESP-IDF
- **`Apri-Cursor-ESP-IDF.cmd`**: apre workspace in modalità Editor classica (Glass non carica ESP-IDF).
- Script **`scripts/build-core.cmd`**, **`scripts/build-flash.cmd`**.
- **`docs/CURSOR_ESP_IDF.md`**: sezione Glass vs Editor, Doctor obbligatorio, status bar.
- **`MrBin.code-workspace`**: path ESP-IDF, `idf.extensionActivationMode: always`, variabili target esp32p4.

### Modificato

#### `CoreRecorder.cpp`
- **`recorder_run_core()`** unificato per sessione PIR e registrazione web (`async_encrypt` flag).
- **`encrypt_mp4_file()`**: buffer heap 512 KB (interno → PSRAM → fallback 64 KB), **`setvbuf` 32 KB** su FILE (heap, non static).
- Log periodico stato, timeout attesa rilascio D2, drain frame, last-chance mux.
- Task background **`encrypt_bg_task`** con stack 16 KB, fallback sync se memoria/task fallisce.

#### `CoreWeb.cpp`
- **`stream_decrypted_mp4()`**: buffer **64 KB** + `setvbuf` 32 KB (prima 8 KB stack).
- CSS pagina Video: righe `.vid-row`, pulsanti download/elimina separati da titolo link.
- Mount SD esplicito su handler `/videos` se non montata.

#### `CoreSD.cpp`
- **`max_files`**: 8 → **12** (più handle aperti durante mux + web).
- Log **`errno`** su `mkdir` e spazio insufficiente.

#### `CoreLive`
- **`core_live_request_stop()`** / **`core_live_is_running()`** per cedere la camera alla registrazione manuale.

### Corretto

- **Boot loop** (`HS_MP: mempool create failed`): rimossi buffer **static** 128 KB; solo allocazione heap/PSRAM.
- **Elenco video vuoto**: mount SD mancante su `/videos`; file `.mp4.tmp` ora visibili come in elaborazione.
- **Download corrotto / path troncati**: encoding `%2F` nel parametro query `f`.
- **Pulsante download gigante senza testo**: CSS `flex:1` applicato solo a `.vid-title`, non a tutti i link.
- **File MP4 illeggibili** da cifratura streaming durante mux: **revert** a tmp plain + post-encrypt.

### Note

- La cifratura resta limitata dalla velocità della **SD card**; con buffer ampliati e background task lo stop web è percepibilmente più rapido.
- Durante cifratura background il file compare come **Elaborazione…** finché non appare il `.mp4` finale.

## [0.3.0] - 2026-06-23 — Stable: pagina errore boot con livelli GPIO HIGH/LOW

**Commit:** `6496842c9dd7018ec77f247b9570d52807a30638`  
**Data/ora release:** 2026-06-23 01:35:13 +0200  
**Tag:** `v0.3.0`  
**Stato:** **Stable** (minor) — prima release stable dopo v0.2.1-beta; migliora la leggibilità diagnostica della pagina errore boot.  
**Target:** Waveshare ESP32-P4-WIFI6-M (ESP-IDF 5.5.4, esp32p4 rev v1.x)  
**Versione firmware:** `src/mrbin_core/VERSION` → `0.3.0`

### Modificato

#### `CoreWeb.cpp` — pagina errore boot
- Banner errore boot: valori snapshot **D1**, **D2**, **MODE** mostrati come **`HIGH`** / **`LOW`** invece di `0` / `1`.
- Helper locale `gpio_level_label()` per etichettare i livelli GPIO al boot.

### Note

- Comportamento firmware invariato rispetto a v0.2.1-beta; solo presentazione Web della diagnostica GPIO.

## [0.2.1-beta] - 2026-06-22 — Beta: LED WiFi/AP/errore, AP errore boot, snapshot GPIO

**Commit:** `6fdcda634c1eeeea70710706b069b0c9be5f3284`  
**Data/ora release:** 2026-06-22 16:08:19 +0200  
**Tag:** `v0.2.1-beta`  
**Stato:** **Beta** (minor) — estende v0.2.0-beta con gestione LED legata al WiFi e pagina errore boot su AP.  
**Target:** Waveshare ESP32-P4-WIFI6-M (ESP-IDF 5.5.4, esp32p4 rev v1.x)  
**Versione firmware:** `src/mrbin_core/VERSION` → `0.2.1-beta`

### Aggiunto

#### Modulo `CoreStatusLed` — LED GPIO52 legato al WiFi
- Task FreeRTOS **`status_led`** con modalità:
  - **`CORE_LED_STA_CONNECTED`**: dopo connessione STA riuscita (**`IP_EVENT_STA_GOT_IP`**) → **3 lampeggi** ogni **500 ms** (impulso 150 ms), poi LED spento.
  - **`CORE_LED_AP`**: in modalità **AP** → **1 lampeggio ogni 2 s** continuo fino a cambio modalità o `core_web_stop()`.
  - **`CORE_LED_ERROR`**: **100 ms** on/off continuo (priorità su pattern AP).
  - **`CORE_LED_OFF`**: LED spento.
- Inizializzazione in `app_main` via `core_status_led_init()` (GPIO52 output, active-high).
- Costanti in `CoreConfig.h`: `CORE_STATUS_LED_STA_BLINK_MS/COUNT/PULSE_MS`, `CORE_STATUS_LED_AP_BLINK_MS`, `CORE_STATUS_LED_ERROR_BLINK_MS`.

#### Snapshot GPIO al boot (`core_gpio_boot_snapshot_t`)
- `core_gpio_save_boot_snapshot()` subito dopo `gpio_init` — salva livelli **D1**, **D2**, **MODE** al reset.
- Usato per messaggio errore Web e log diagnostici.

#### Errore boot PIR — AP + Web + LED errore
- Se **GPIO29 LOW** e **D1 non attivo** (wake non riconosciuto):
  - Avvio **WiFi AP** `MrBin-XXXXX` / pass `12345678`.
  - **Web GUI** su porta **1510** con banner **rosso** in home/login:
    `ERRORE BOOT PIN non riconosciuto, D1=xxx D2=xxx MODE=xxx` (valori snapshot al boot).
  - LED in modalità **errore** (100 ms), **non** lampeggio AP a 2 s (anche se radio in AP).
  - Loop attesa (TPL resta alimentato).

#### API Web estesa (`CoreWeb.h`)
- `core_web_start(settings, wifi_mode, boot_error)` con enum:
  - `CORE_WEB_WIFI_STA` — connessione STA se SSID presente.
  - `CORE_WEB_WIFI_AP` — AP configurazione (SSID assente o fallback).
  - `CORE_WEB_WIFI_AP_BOOT_ERROR` — AP + pagina errore boot.
- Handler **`IP_EVENT_STA_GOT_IP`** → `core_status_led_notify_sta_connected()`.

### Modificato

#### `CoreWeb.cpp`
- Ripristinato **`wifi_start_ap()`** per config ed errore boot.
- **Config GPIO29 HIGH**:
  - SSID in NVS → STA + Web; LED lampeggia 3×500 ms solo dopo IP ottenuto.
  - SSID vuoto → AP + lampeggio 2 s.
  - STA fallito → fallback AP + lampeggio 2 s.
- **`core_web_stop()`** spegne LED (`CORE_LED_OFF`).
- Pagina login dinamica con classe CSS `.boot-err` (rosso, bordo) se errore boot.

#### `CoreGPIO`
- Rimosso `core_gpio_blink_error_forever()` (sostituito da `CoreStatusLed` + AP/Web).
- Aggiunti `core_gpio_save_boot_snapshot()` / `core_gpio_get_boot_snapshot()`.

#### `mrbin_core_main.cpp`
- `core_gpio_save_boot_snapshot()` + `core_status_led_init()` dopo init GPIO.
- `run_config_mode()`: chiama `core_web_start()` con modalità STA o AP.
- `run_pir_mode()`: errore → `CORE_WEB_WIFI_AP_BOOT_ERROR` con snapshot.

#### `CMakeLists.txt`
- Aggiunto `CoreStatusLed.cpp`.

### Comportamento LED — riepilogo

| Scenario | WiFi | LED GPIO52 |
|----------|------|------------|
| STA connesso (GOT IP) | STA | 3× @ 500 ms, poi off |
| AP config / fallback | AP | 1 flash ogni 2 s |
| Errore boot (no D1) | AP | 100 ms continuo |
| Uscita config (DONE) | stop | off |
| Registrazione PIR | off | off |

### Note Beta / test

1. Errore boot: verificare AP `MrBin-XXXXX`, pagina `/` con testo rosso e valori D1/D2/MODE coerenti col cablaggio.
2. Config con SSID: LED 3 lampeggi **solo dopo** connessione rete (non all’avvio STA).
3. Config senza SSID: AP + lampeggio lento 2 s.
4. In errore boot il lampeggio deve restare **rapido** (100 ms), non lento AP.

### Problemi noti (invariati da v0.2.0-beta)

- Soglia 50 MB SD, fast-boot PSRAM→muxer, docs `CORE_FIRMWARE.md` da aggiornare.

## [0.2.0-beta] - 2026-06-22 — Beta: GPIO29 config, fast-boot PSRAM, Web condizionale, LED errore

**Commit:** `01340dfc65576e8e8849c037a8653e7aeb00b477`  
**Data/ora release:** 2026-06-22 13:19:12 +0200  
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

[0.3.0]: https://github.com/mirkomare/MrBin/releases/tag/v0.3.0
[0.2.1-beta]: https://github.com/mirkomare/MrBin/releases/tag/v0.2.1-beta
[0.2.0-beta]: https://github.com/mirkomare/MrBin/releases/tag/v0.2.0-beta
[0.1.1]: https://github.com/mirkomare/MrBin/releases/tag/v0.1.1
[0.1.0]: https://github.com/mirkomare/MrBin/releases/tag/v0.1.0
