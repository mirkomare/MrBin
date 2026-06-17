# MrBin CORE — guida firmware

Firmware per **Waveshare ESP32-P4-WIFI6-M** integrato nell'ecosistema BLEmax come device polling alimentato da **TPL5111** e **PIR**.

## Funzionalità

| Modulo | Descrizione |
|--------|-------------|
| **GPIO** | D1=GPIO28, D2=GPIO21, DONE=GPIO23 |
| **Registrazione** | H.264 1920×1080 @ 30 fps → MP4 su SD, cifrato AES-128 |
| **Naming file** | Dir `DDMMAAAA`, file `ID_DDMM_hhmmss.mp4` |
| **Persistenza** | ID CORE 5 cifre + chiave AES-128 in NVS (create al primo avvio) |
| **Web GUI** | Porta **1510**, login `mm` / `123456` (hash SHA-256 nello sketch) |
| **Pagine web** | Impostazioni (ID, WiFi, delay D2, chiave, format SD) + elenco video con play decifrato |

## Build (ESP-IDF)

Prerequisiti: [ESP-IDF 5.5+](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/get-started/index.html), componenti `esp_capture` e `esp_muxer` (in `idf_component.yml`).

```bash
cd c:/CURSOR/Mrbin/src/mrbin_core
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor
```

## Modalità di avvio

### 1. Wake PIR (D1 — GPIO28 LOW)
1. WiFi e BLE spenti
2. Monta SD, verifica spazio (elimina directory giorno più vecchia se necessario)
3. Crea cartella `DDMMAAAA` e avvia registrazione
4. Attende **D2** (GPIO21 LOW)
5. Attende delay (default **5 s**, configurabile)
6. **DONE** su GPIO23 HIGH → loop spegnimento TPL

### 2. Configurazione (GPIO28 non LOW)
- Avvia WiFi STA con credenziali salvate
- Web GUI su `http://<ip>:1510/`
- Login: user **mm**, password **123456**

## Formato file criptato

```
[4 byte magic MRBI][16 byte IV][payload MP4 cifrato AES-128-CTR]
```

La chiave è in NVS; la web GUI la mostra in hex (sola lettura). Il playback `/play` decifra in streaming.

## Struttura sorgenti

```
main/
  mrbin_core_main.cpp   # entry, routing D1 vs config
  CoreGPIO.*            # segnali PIR/TPL
  CoreSettings.*        # NVS: ID, chiave, WiFi, delay
  CoreSD.*              # mount, format, naming, spazio
  CoreCrypto.*          # AES-128-CTR
  CoreRecorder.*        # esp_capture H264 + cifratura
  CoreWeb.*             # HTTP server + HTML
  CoreAuth.*            # login hash
```

## Note

- La registrazione usa `esp_capture` / encoder hardware H.264 del P4; verificare camera OV5647 collegata.
- Pin SDIO SD e CSI seguono il pinout Waveshare (vedi wiki board).
- Per integrazione futura BLEmax: il CORE resta autonomo; eventuale UART/BLE in modalità config.
