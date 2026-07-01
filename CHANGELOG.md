# Changelog

Formato basato su [Keep a Changelog](https://keepachangelog.com/it/1.0.0/).

**Versioning:** se non indicato diversamente, incrementare sempre l’**ultimo numero** (patch: `0.3.0` → `0.3.1`). Minor/major solo su richiesta esplicita.

## [0.5.0] - 2026-07-01 — Minor STABLE: video 1080p nativo, cifratura real-time affidabile, PTS a tempo reale, stop pin robusto

**Commit:** `__COMMIT_HASH__`  
**Data/ora release:** `__COMMIT_DATE__`  
**Tag:** `v0.5.0`  
**Stato:** **Stable** — pipeline di registrazione consolidata: qualità video piena, download dei file cifrati funzionante, velocità di riproduzione corretta, rilevamento dello stop affidabile. Validata sul campo dall'utente ("sembra perfetta").  
**Target:** Waveshare ESP32-P4-WIFI6-M (ESP-IDF 5.5.4, esp32p4 rev v1.x)  
**Versione firmware:** `src/mrbin_core/VERSION` → `0.5.0`

### Contesto

Release che chiude quattro problemi emersi nei test della 0.4.0: (1) qualità video bassa, (2) file scaricati "corrotti", (3) stop pin inaffidabile, (4) riproduzione accelerata. In più, chiarita e documentata la logica del timer TPL5110.

### Aggiunto

#### Qualità video: sensore OV5647 a 1920×1080 nativo (`sdkconfig.defaults`)
- Il sensore era configurato con **formato di default RAW8 800×800** (indice 2) e l'immagine veniva **ingrandita** a 1080p (log `V4L2_SRC: Best match 800x800` + `Convert 800x800 to 1920x1080`) → qualità reale bassa.
- Impostato il **default sensore su RAW10 1920×1080 @30** (indice 3) via
  `CONFIG_CAMERA_OV5647_MIPI_DEFAULT_FMT_RAW10_1920X1080_30FPS=y` e
  `CONFIG_CAMERA_OV5647_MIPI_IF_FORMAT_INDEX_DEFAULT=3` in `sdkconfig.defaults`
  (e allineato l'`sdkconfig` attivo). Ora la cattura è **nativa a 1080p**, senza upscaling.

#### File-writer cifrante dedicato del muxer (`CoreRecorder`)
- Nuovo **`esp_muxer_file_writer_t`** (`enc_writer_open/write/seek/close`) installato con
  `esp_muxer_set_file_writer()`: cifra **AES-128-CTR on-the-fly** mentre esp_muxer scrive
  l'MP4 direttamente sul path reale su SD.
- Header `[MRBI][IV]` (20 byte) a offset 0; l'MP4 logico parte al byte 20. Il buffer del
  muxer non viene mutato (copia su scratch prima di cifrare). `on_seek` sposta solo la
  posizione logica, così la riscrittura del box `moov` (`moov_before_mdat`) resta allineata
  e l'header è preservato.
- Nuovo setter interno **`core_recorder_set_mux_key()`**: la chiave AES arriva dai settings.

#### PTS ancorati al tempo reale (`CoreRecorder`)
- Nuovo campo `recorder_capture_t.pts_base_us` e helper **`recorder_next_pts_ms()`**:
  ogni frame riceve `PTS = (esp_timer_now − base_primo_frame) / 1000` ms.
- Stesso orologio per i frame bufferizzati in PSRAM (fase pre-SD) e per quelli live dopo
  l'handoff su SD → **timeline continua**.

#### Rilevamento stop pin disaccoppiato dal frame-rate (`CoreGPIO`)
- Nuovo **timer periodico a 10 ms** (`CORE_REC_STOP_POLL_MS`) che campiona il pin di stop,
  applica il debounce (`CORE_REC_STOP_DEBOUNCE_MS = 50 ms`) e **aggancia un latch**
  (`s_rec_stop_latched`), indipendentemente dalla velocità del loop di cattura.
- Nuove API **`core_gpio_rec_stop_session_end()`** (ferma il timer a fine sessione) e
  `core_gpio_rec_stop_session_begin()` ora avvia il timer. `core_gpio_is_rec_stop_triggered()`
  legge il latch (con fallback inline se il timer non è disponibile).

#### Diagnostica
- `recording_file_is_valid()` verifica e logga il magic dell'header:
  `MP4 OK cifrato: ... (N bytes, header=MRBI)` oppure `SENZA header cifrato`.
- Timing di avvio "da accensione": `Registrazione PSRAM attiva in X ms (Y ms da accensione)`
  e `Primo frame catturato a Z ms da accensione`.

### Modificato
- Il muxer ora riceve il **path reale** (`final_path`) invece del path VFS `/enc`; l'MP4 è
  cifrato dal writer dedicato, non più tramite il VFS `/enc` (che con `moov_before_mdat` +
  buffering stdio + `fstat` sfasato di 20 byte produceva MP4 malformati → download "corrotto").
- `recorder_muxer_feed_frame()` accetta un **PTS esplicito**; i due punti di alimentazione
  frame (loop principale e drain) usano `recorder_next_pts_ms()`.

### Corretto
- **Qualità video bassa**: era upscaling da 800×800; ora 1080p nativo.
- **Download "file corrotto"**: la scrittura cifrata via VFS `/enc` era incompatibile con la
  riscrittura del `moov`; sostituita dal file-writer cifrante del muxer.
- **Stop pin inaffidabile** ("va mandato tante volte, a volte non arriva mai"): il pin era
  campionato una volta per frame e a 1080p il loop rallenta → finestra LOW persa. Ora timer
  dedicato a 10 ms.
- **Riproduzione accelerata**: la durata MP4 era più corta del reale; PTS ora a tempo reale.

### Note tecniche — TPL5110 (nessuna modifica firmware, solo chiarimento)
- Da datasheet TI: la resistenza su `DELAY/M_DRV` imposta **tIP**, che è il **tempo massimo di
  accensione** (watchdog), non un intervallo di sonno. `DONE` spegne in anticipo entro tIP;
  se `DONE` non arriva, il TPL taglia comunque a fine tIP (era il reset ~60 s).
- **Timer mode** (`EN/ONE_SHOT=HIGH`): risveglio periodico ogni tIP → utile per "accensione
  ogni N minuti". **One-shot** (`EN/ONE_SHOT=LOW`, uso attuale): risveglio dal PIR (M_DRV),
  tIP = tetto massimo di accensione. La soluzione da 100k (tIP ≈ 30 min) è corretta.
- Attenzione: finché `DELAY/M_DRV` resta HIGH, i `DONE` sono ignorati → il trigger PIR deve
  essere un impulso breve.

### Compatibilità
- Il modulo `CoreEncFs` (VFS `/enc`) resta nel codice e registrato all'avvio, ma **non è più
  usato dal muxer** (sostituito dal file-writer cifrante). Il formato su disco degli MP4
  cifrati è invariato (header `[MRBI][IV]` + AES-128-CTR), quindi i file delle versioni
  precedenti restano leggibili/scaricabili.

---

## [0.4.0] - 2026-07-01 — Minor: pin config web, spegnimento TPL robusto, cifratura atomica, diagnostica reset

**Commit:** `935b0879943b01aa7deb5fa7cbc7e7561c960e4a`  
**Data/ora release:** 2026-07-01 11:19:44 +0200  
**Tag:** `v0.4.0`  
**Stato:** **Stable** (minor) — configurazione pin registrazione da Web, gestione spegnimento TPL5110 rivista, cifratura MP4 atomica anti-corruzione, recupero file orfani e diagnostica di reset/alimentazione.  
**Target:** Waveshare ESP32-P4-WIFI6-M (ESP-IDF 5.5.4, esp32p4 rev v1.x)  
**Versione firmware:** `src/mrbin_core/VERSION` → `0.4.0`

### Aggiunto

#### Configurazione pin registrazione da Web (`CoreWeb`, `CoreSettings`)
- Nuova sezione **"Pin registrazione PIR"** nella pagina di configurazione: selezione del **pin di stop** (D1 o D2) della registrazione.
- Nuovi campi NVS **`rec_gpio_start`** e **`rec_gpio_stop`** in `core_settings_t`, con validazione (`core_settings_rec_pins_valid`) e default (`core_settings_rec_pins_set_defaults`): default **start = D1 (GPIO28)**, **stop = D2 (GPIO21)**.
- Persistenza dei pin su NVS in `core_settings_save` / `core_settings_load`.
- Applicazione runtime dei pin via **`core_gpio_set_rec_pins()`** dopo il caricamento delle impostazioni.

#### Spegnimento config-mode controllato da GPIO29/MODE (`mrbin_core_main`)
- In modalità **config** il firmware ora monitora **solo GPIO29 (MODE)**: si entra con MODE **HIGH** al boot e, se MODE **torna LOW** (stabile per `CORE_MODE_EXIT_DEBOUNCE_MS = 150 ms`, anti-glitch), si esce **subito** e si avvia lo spegnimento (DONE al TPL, nessun ritardo).
- **Heartbeat** periodico (ogni 5 s) con uptime: `Config attiva: uptime N s (MODE=HIGH)`.
- Arresto pulito del web server (`core_web_stop()`) prima dello spegnimento.

#### Diagnostica reset e alimentazione (`mrbin_core_main`)
- Log **`log_reset_reason()`** all'avvio: distingue **POWERON / BROWNOUT / PANIC / INT_WDT / TASK_WDT / SW / EXT / DEEPSLEEP**, per capire se lo spegnimento improvviso è alimentazione (brownout), TPL (power-cycle pulito) o crash firmware.

#### Recupero registrazioni interrotte (`CoreRecorder`)
- Nuova API **`core_recorder_recover_tmp_files()`**: all'ingresso in config scandisce `/sdcard/<giorno>/`, **cifra e finalizza** i `.mp4.tmp` orfani validi (residui di spegnimenti improvvisi) ed **elimina** quelli corrotti. Raccolta nomi prima dell'elaborazione (evita di modificare la directory durante `readdir`, fragile su FATFS).

#### Log stato pin (`CoreGPIO`)
- **`core_gpio_log_pin_edges()`**: log seriale sulle transizioni HIGH/LOW di D1 e D2 durante la registrazione (debug rumore/floating).
- **`core_gpio_rec_stop_session_begin()`** / **`core_gpio_is_rec_stop_triggered()`**: gestione dedicata dello stop con debounce.

### Modificato

#### Cifratura MP4 atomica anti-corruzione (`CoreRecorder`)
- **`encrypt_mp4_file()`** ora scrive su file temporaneo **`<nome>.mp4.part`** e lo **rinomina** in `.mp4` **solo a cifratura completata**: il file finale non è mai visibile a 0 byte/incompleto. Su qualsiasi errore il `.part` viene rimosso.
- Risolve il bug del **file a 0 byte** creato dalla web/gestione file quando la cifratura (specialmente in background) era in corso o veniva interrotta.

#### Segnale DONE TPL5110 pulsato (`CoreGPIO`)
- **`core_gpio_hold_tpl_done()`** ora emette un **loop di spegnimento**: DONE **HIGH 1000 ms / LOW 100 ms** all'infinito (`CORE_TPL_DONE_PULSE_MS`, `CORE_TPL_DONE_GAP_MS`) finché il TPL toglie alimentazione — spegnimento robusto (modello device polling BLEmax).

#### Stop registrazione con debounce (`CoreGPIO`, `CoreRecorder`)
- Lo stop avviene quando il **pin stop configurato** è LOW **stabile per ≥ `CORE_REC_STOP_DEBOUNCE_MS` (50 ms)**, filtrando i glitch che causavano stop immediati.
- Rimossa la vecchia attesa di rilascio D2; `should_stop()` usa `core_gpio_is_rec_stop_triggered()`.

#### Modalità operativa normale (`mrbin_core_main`)
- **`run_pir_mode()`** registra **sempre e subito** se MODE è LOW (TPL5110 in manual), senza pretendere D1 LOW al boot; lo stop resta sul pin configurato.

#### Autenticazione Web (`CoreAuth`)
- Password utente `mm` cambiata da `123456` a **`GaPaMi`** (`CORE_AUTH_SHA256` = SHA-256 di `mm:GaPaMi`).

#### Listing video robusto (`CoreWeb`)
- I file **`.part`** (cifratura in corso) sono **ignorati** nella lista.
- I `.mp4` finali **sotto la dimensione dell'header crypto** (0 byte/corrotti) non vengono più mostrati (niente più errori al click).
- `snprintf` con precisione esplicita (`%.*s`) e buffer ampliati per evitare `-Wformat-truncation`.

#### Freeze pin hardware (`CoreConfig.h`)
- **`static_assert`** che congela le assegnazioni critiche: **D1=GPIO28, D2=GPIO21, DONE=GPIO23, MODE=GPIO29**.
- Nuove costanti timing: `CORE_TPL_DONE_PULSE_MS`, `CORE_TPL_DONE_GAP_MS`, `CORE_MODE_POLL_MS`, `CORE_MODE_EXIT_DEBOUNCE_MS`, `CORE_REC_STOP_DEBOUNCE_MS`.

### Build
- **`main/CMakeLists.txt`**: aggiunto **`-Wno-missing-field-initializers`** (oltre a `-Wno-error=...` già presenti) per silenziare il rumore dei designated-initializer parziali sulle struct SDK (esp_capture/esp_muxer). Build pulita.

### Note tecniche
- **AES confermato hardware**: `CONFIG_MBEDTLS_HARDWARE_AES=y` + `MBEDTLS_AES_USE_INTERRUPT=y`; su ESP32-P4 `esp_aes_crypt_ctr` cifra l'intero buffer via **DMA** in un'unica operazione.
- Individuato che il tempo di salvataggio elevato dipende dal **doppio passaggio su SD** (mux plain `.tmp` → rilettura → riscrittura cifrata), non dall'AES: ottimizzazione "cifratura al volo" pianificata per una release successiva.

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
