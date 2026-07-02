#include "CoreVideo.h"
#include "CoreConfig.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_video_init.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <fcntl.h>
#include <unistd.h>

static const char *TAG = "core_video";
static bool s_init = false;
static SemaphoreHandle_t s_init_mtx = nullptr;

#define CORE_VIDEO_INIT_RETRY_COUNT  5
#define CORE_VIDEO_INIT_RETRY_MS     500
#define CORE_VIDEO_INIT_FLAGS        (ESP_VIDEO_INIT_FLAGS_MIPI_CSI | ESP_VIDEO_INIT_FLAGS_ISP)

static bool core_video_probe_device(void) {
    int fd = open(CORE_VIDEO_DEV_NAME, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    close(fd);
    return true;
}

static bool core_video_init_once(void);

static void core_video_log_heap(const char *label) {
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    size_t blk_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "%s: int=%u KB dma=%u KB blk_dma=%u KB",
             label, (unsigned)(free_int / 1024), (unsigned)(free_dma / 1024),
             (unsigned)(blk_dma / 1024));
}

bool core_video_is_ready(void) {
    return s_init && core_video_probe_device();
}

// Diagnostica: scansiona il bus SCCB (GPIO7/8) e logga chi risponde.
// Nessun ACK = problema elettrico (cavo/alimentazione/connettore).
// ACK a indirizzo != 0x36 = sensore diverso da OV5647 (es. SC2336 a 0x30).
static void core_video_i2c_scan(void) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t)CORE_CSI_I2C_SDA,
        .scl_io_num = (gpio_num_t)CORE_CSI_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "Scan I2C: bus non disponibile (occupato da SCCB?)");
        return;
    }
    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            found++;
            ESP_LOGW(TAG, "Scan I2C GPIO%d/%d: dispositivo trovato a 0x%02X%s",
                     CORE_CSI_I2C_SDA, CORE_CSI_I2C_SCL, addr,
                     addr == 0x36 ? " (OV5647 atteso)" :
                     addr == 0x30 ? " (possibile SC2336 — driver non abilitato!)" : "");
        }
    }
    if (found == 0) {
        ESP_LOGE(TAG, "Scan I2C GPIO%d/%d: NESSUN dispositivo risponde — "
                 "camera senza alimentazione o flat non a contatto",
                 CORE_CSI_I2C_SDA, CORE_CSI_I2C_SCL);
    }
    i2c_del_master_bus(bus);
}

bool core_video_init(void) {
    if (!s_init_mtx) {
        s_init_mtx = xSemaphoreCreateMutex();
    }
    if (s_init_mtx && xSemaphoreTake(s_init_mtx, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout mutex init camera");
        return core_video_is_ready();
    }

    bool ok = false;
    if (s_init && core_video_probe_device()) {
        ok = true;
        goto out;
    }
    if (s_init) {
        ESP_LOGW(TAG, "Camera non risponde — reinizializzo sottosistema video");
        core_video_deinit();
    }

    core_video_log_heap("Pre-init camera");
    for (int attempt = 1; attempt <= CORE_VIDEO_INIT_RETRY_COUNT; ++attempt) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "Retry init camera (%d/%d)...", attempt, CORE_VIDEO_INIT_RETRY_COUNT);
            esp_video_deinit();
            vTaskDelay(pdMS_TO_TICKS(CORE_VIDEO_INIT_RETRY_MS));
        }
        if (core_video_init_once()) {
            ok = true;
            break;
        }
        esp_video_deinit();
    }
    if (!ok) {
        core_video_log_heap("Init camera fallita");
        core_video_i2c_scan();
    }

out:
    if (s_init_mtx) {
        xSemaphoreGive(s_init_mtx);
    }
    return ok;
}

static bool core_video_init_once(void) {
#if CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE
    esp_video_init_csi_config_t csi_cfg = {
        .sccb_config = {
            .init_sccb = true,
            .i2c_config = {
                .port = I2C_NUM_0,
                .scl_pin = (gpio_num_t)CORE_CSI_I2C_SCL,
                .sda_pin = (gpio_num_t)CORE_CSI_I2C_SDA,
            },
            .freq = 100000,
        },
        .reset_pin = (gpio_num_t)-1,
        .pwdn_pin = (gpio_num_t)-1,
        .dont_init_ldo = false,
    };

    esp_video_init_config_t video_cfg = {
        .csi = &csi_cfg,
    };

    esp_err_t err = esp_video_init_with_flags(&video_cfg, CORE_VIDEO_INIT_FLAGS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init fallita: %s", esp_err_to_name(err));
        esp_video_deinit();
        return false;
    }
    if (!core_video_probe_device()) {
        ESP_LOGE(TAG, "Camera non rilevata su %s (MIPI-CSI / I2C GPIO%d,%d)",
                 CORE_VIDEO_DEV_NAME, CORE_CSI_I2C_SDA, CORE_CSI_I2C_SCL);
        esp_video_deinit();
        return false;
    }
#else
    ESP_LOGE(TAG, "MIPI-CSI non abilitato in sdkconfig");
    return false;
#endif

    s_init = true;
    ESP_LOGI(TAG, "Sottosistema video inizializzato (%s)", CORE_VIDEO_DEV_NAME);
    return true;
}

void core_video_deinit(void) {
    esp_video_deinit();
    s_init = false;
}

bool core_video_prepare_for_stream(void) {
    vTaskDelay(pdMS_TO_TICKS(200));
    return core_video_init();
}
