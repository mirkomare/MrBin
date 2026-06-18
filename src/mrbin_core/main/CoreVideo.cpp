#include "CoreVideo.h"
#include "CoreConfig.h"

#include "esp_log.h"
#include "esp_video_init.h"
#include <fcntl.h>
#include <unistd.h>

static const char *TAG = "core_video";
static bool s_init = false;

static bool core_video_probe_device(void) {
    int fd = open(CORE_VIDEO_DEV_NAME, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    close(fd);
    return true;
}

bool core_video_is_ready(void) {
    return s_init && core_video_probe_device();
}

bool core_video_init(void) {
    if (s_init) {
        return core_video_is_ready();
    }

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

    esp_err_t err = esp_video_init(&video_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init fallita: %s", esp_err_to_name(err));
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
    if (!s_init) {
        return;
    }
    esp_video_deinit();
    s_init = false;
}
