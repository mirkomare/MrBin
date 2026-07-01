idf_component_register(
    SRCS
        "mrbin_core_main.cpp"
        "CoreGPIO.cpp"
        "CoreSettings.cpp"
        "CoreCrypto.cpp"
        "CoreSD.cpp"
        "CoreVideo.cpp"
        "CoreRecorder.cpp"
        "CoreEncFs.cpp"
        "CoreAuth.cpp"
        "CoreWeb.cpp"
        "CoreLive.cpp"
        "CoreH264Buffer.cpp"
        "CoreStatusLed.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_wifi_remote
        esp_netif
        esp_event
        esp_http_server
        fatfs
        sdmmc
        esp_driver_sdmmc
        mbedtls
        driver
        esp_timer
        vfs
        esp_capture
        esp_muxer
        esp_video
        esp_video_codec
)

# GCC 14 (IDF 5.5.4) is strict about path formatting in FatFS helpers.
# -Wno-missing-field-initializers: i designated initializer parziali su struct
# dell'SDK (esp_capture/esp_muxer) azzerano i campi non elencati: rumore innocuo.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=format-truncation
    -Wno-error=stringop-truncation
    -Wno-error=missing-field-initializers
    -Wno-missing-field-initializers)
