#pragma once

#include <stdint.h>

// --- GPIO segnali PIR / TPL5111 (CORE = ESP32-P4) ---
#define CORE_GPIO_D1_WAKE   28   // LOW = accensione per movimento (D1)
#define CORE_GPIO_D2_END    21   // LOW = fine movimento (D2)
#define CORE_GPIO_TPL_DONE  23   // HIGH = DONE verso TPL5111 (spegnimento)

// --- Rete / Web ---
#define CORE_WEB_PORT       1510

// --- Video ---
#define CORE_VIDEO_WIDTH    1920
#define CORE_VIDEO_HEIGHT   1080
#define CORE_VIDEO_FPS      30

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
