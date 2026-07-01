#pragma once

#include "driver/gpio.h"
#include <stdint.h>

// =====================================================================
// MAPPA PIN CONGELATA — NON MODIFICARE (hardware MrBin CORE / TPL5110)
// Ruolo start/stop tra D1 e D2 si sceglie dalla Web interface, NON qui.
//   D1   = GPIO28   wake movimento PIR (LOW attivo)
//   D2   = GPIO21   fine movimento    (LOW attivo)
//   DONE = D10 = GPIO23   DONE verso TPL5110 (HIGH = spegnimento)
//   MODE = GPIO29   HIGH = config WiFi/Web, LOW = PIR
// =====================================================================
#define CORE_GPIO_D1_WAKE   GPIO_NUM_28   // D1  — LOW = accensione per movimento
#define CORE_GPIO_D2_END    GPIO_NUM_21   // D2  — LOW = fine movimento
#define CORE_GPIO_TPL_DONE  GPIO_NUM_23   // DONE (D10) — HIGH = DONE verso TPL5110 (spegnimento)
#define CORE_GPIO_MODE_CFG  GPIO_NUM_29   // MODE — HIGH = config (WiFi/Web), LOW = PIR

#if defined(__cplusplus)
static_assert(CORE_GPIO_D1_WAKE  == GPIO_NUM_28, "PIN CONGELATO: D1 deve restare GPIO28");
static_assert(CORE_GPIO_D2_END   == GPIO_NUM_21, "PIN CONGELATO: D2 deve restare GPIO21");
static_assert(CORE_GPIO_TPL_DONE == GPIO_NUM_23, "PIN CONGELATO: DONE/D10 deve restare GPIO23");
static_assert(CORE_GPIO_MODE_CFG == GPIO_NUM_29, "PIN CONGELATO: MODE deve restare GPIO29");
#endif
// LED errore boot PIR senza D1 — header 2x20 alto-sx, HIGH=on, pull-down esterno
#define CORE_GPIO_STATUS_LED      GPIO_NUM_52
#define CORE_STATUS_LED_ON_LEVEL  1
#define CORE_STATUS_LED_ERROR_BLINK_MS  100
#define CORE_STATUS_LED_AP_BLINK_MS     2000
#define CORE_STATUS_LED_STA_BLINK_MS    500
#define CORE_STATUS_LED_STA_BLINK_COUNT 3
#define CORE_STATUS_LED_STA_PULSE_MS    150

// --- Rete / Web ---
#define CORE_WEB_PORT       1510

// --- Video ---
#define CORE_VIDEO_WIDTH    1920
#define CORE_VIDEO_HEIGHT   1080
#define CORE_VIDEO_FPS      30
#define CORE_VIDEO_DEV_NAME "/dev/video0"

// --- Live preview (MJPEG HW — compatibile browser via multipart) ---
#define CORE_LIVE_WIDTH     CORE_VIDEO_WIDTH
#define CORE_LIVE_HEIGHT    CORE_VIDEO_HEIGHT
#define CORE_LIVE_FPS       CORE_VIDEO_FPS

// --- SDMMC (Waveshare ESP32-P4-WIFI6-M, 4-bit SDIO) ---
#define CORE_SD_PIN_CLK     GPIO_NUM_43
#define CORE_SD_PIN_CMD     GPIO_NUM_44
#define CORE_SD_PIN_D0      GPIO_NUM_39
#define CORE_SD_PIN_D1      GPIO_NUM_40
#define CORE_SD_PIN_D2      GPIO_NUM_41
#define CORE_SD_PIN_D3      GPIO_NUM_42
// Slot 0 IO alimentato da LDO_VO4 su VDD_IO_5 (Waveshare P4)
#define CORE_SD_LDO_CHAN_ID 4

// --- MIPI-CSI SCCB (camera OV5647) ---
#define CORE_CSI_I2C_SCL    GPIO_NUM_8
#define CORE_CSI_I2C_SDA    GPIO_NUM_7

// --- Timing ---
#define CORE_D2_POST_DELAY_MS  5000   // attesa dopo pin stop prima del DONE (configurabile via web)
#define CORE_REC_STOP_DEBOUNCE_MS  50 // pin stop LOW stabile per fermare (anti-glitch)
#define CORE_TPL_DONE_PULSE_MS  1000  // DONE HIGH per impulso nel loop di spegnimento
#define CORE_TPL_DONE_GAP_MS     100  // riposo LOW tra un impulso DONE e il successivo
#define CORE_MODE_POLL_MS         50  // intervallo polling GPIO29 (MODE) in config
#define CORE_MODE_EXIT_DEBOUNCE_MS 150 // MODE LOW stabile per uscire da config e spegnere

// --- Storage ---
#define CORE_SD_MOUNT_POINT "/sdcard"
#define CORE_NVS_NAMESPACE  "mrbin_core"

// --- ID CORE ---
#define CORE_ID_DIGITS      5
#define CORE_ID_MIN         10000
#define CORE_ID_MAX         99999

// --- Crypto ---
#define CORE_AES_KEY_BYTES  16   // AES-128
#define CORE_CRYPTO_MAGIC   0x4D524249   // "MRBI" — header file criptati

// --- SD spazio minimo per nuova registrazione (bytes) ---
#define CORE_SD_MIN_FREE_BYTES  (50ULL * 1024ULL * 1024ULL)

// --- Registrazione fast-boot (buffer PSRAM pre-SD + mux MP4 su SD) ---
#define CORE_H264_PSRAM_MAX_BYTES   (12U * 1024U * 1024U)   // ~12 MB in PSRAM
#define CORE_MUXER_RAM_CACHE_BYTES  (512U * 1024U)          // cache muxer → meno flush SD
#define CORE_REC_ENCRYPT_IO_BYTES   (512U * 1024U)          // buffer cifratura (heap, non static)
#define CORE_CRYPTO_STREAM_IO_BYTES (64U * 1024U)           // buffer decifratura stream web
#define CORE_SD_FILE_BUF_BYTES      (32U * 1024U)           // setvbuf FILE durante I/O SD
#define CORE_SD_PREP_TASK_STACK     4096
#define CORE_SD_PREP_TASK_PRIO      3
#define CORE_REC_ACQUIRE_TIMEOUT_MS 100

// Credenziali web (user mm / pass GaPaMi) — hash SHA-256 di "mm:GaPaMi"
// Generato offline; verifica in CoreAuth.cpp
extern const uint8_t CORE_AUTH_SHA256[32];
