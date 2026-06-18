#pragma once

#include "driver/gpio.h"
#include <stdint.h>

// --- GPIO segnali PIR / TPL5111 (CORE = ESP32-P4) ---
#define CORE_GPIO_D1_WAKE   GPIO_NUM_28   // LOW = accensione per movimento (D1)
#define CORE_GPIO_D2_END    GPIO_NUM_21   // LOW = fine movimento (D2)
#define CORE_GPIO_TPL_DONE  GPIO_NUM_23   // HIGH = DONE verso TPL5111 (spegnimento)

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
#define CORE_D2_POST_DELAY_MS  5000   // attesa dopo D2 prima del DONE (configurabile via web)

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

// Credenziali web (user mm / pass 123456) — hash SHA-256 di "mm:123456"
// Generato offline; verifica in CoreAuth.cpp
extern const uint8_t CORE_AUTH_SHA256[32];
