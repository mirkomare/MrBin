/*
 * MrBin CORE — entry point compatibilità Arduino IDE
 *
 * Il firmware reale è un progetto ESP-IDF in questa stessa cartella.
 * Waveshare raccomanda ESP-IDF per ESP32-P4 (camera H.264, SDIO, Wi-Fi6).
 *
 * Build:
 *   cd mrbin_core
 *   idf.py set-target esp32p4 && idf.py build flash monitor
 *
 * Vedi: ../../docs/CORE_FIRMWARE.md
 * Scheda: ../../schede/esp32-p4-wifi6-m.md
 */

#if defined(ARDUINO) && defined(CONFIG_IDF_TARGET_ESP32P4)
void setup() {
    // In Arduino-ESP32 P4, delegare a app_main se disponibile
    // Altrimenti usare il workflow ESP-IDF sopra.
}

void loop() {}
#else
// File di riferimento — compilare con ESP-IDF, non con Arduino IDE standalone.
#endif
