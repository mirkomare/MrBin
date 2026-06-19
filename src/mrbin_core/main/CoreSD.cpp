#include "CoreSD.h"
#include "CoreConfig.h"

#include "dirent.h"
#include "driver/sdmmc_default_configs.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif
#include "sys/stat.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE
// ESP-Hosted init (constructor) already owns sdmmc_host_init on slot 1 (C6 WiFi).
#define WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT 1
#else
#define WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT 0
#endif

static const char *TAG = "core_sd";
static sdmmc_card_t *s_card = nullptr;
static bool s_mounted = false;
#if SOC_SDMMC_IO_POWER_EXTERNAL
static sd_pwr_ctrl_handle_t s_pwr_ctrl = nullptr;
#endif

#if WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT
static esp_err_t sdmmc_host_init_dummy(void) {
    return ESP_OK;
}

static esp_err_t sdmmc_host_deinit_dummy(void) {
    return ESP_OK;
}
#endif

#if SOC_SDMMC_IO_POWER_EXTERNAL
static bool core_sd_setup_ldo(void) {
    if (s_pwr_ctrl) return true;

    sd_pwr_ctrl_ldo_config_t ldo_cfg = {
        .ldo_chan_id = CORE_SD_LDO_CHAN_ID,
    };
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_pwr_ctrl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LDO SD (canale %d) fallito: %s", CORE_SD_LDO_CHAN_ID, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "LDO SD attivo (canale %d)", CORE_SD_LDO_CHAN_ID);
    return true;
}
#endif

bool core_sd_init(void) {
    if (s_mounted) return true;

#if SOC_SDMMC_IO_POWER_EXTERNAL
    if (!core_sd_setup_ldo()) return false;
#endif

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // Waveshare P4: SD card on slot 0; ESP-Hosted WiFi coprocessor on slot 1.
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
#if WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT
    host.init = &sdmmc_host_init_dummy;
    host.deinit = &sdmmc_host_deinit_dummy;
#endif
#if SOC_SDMMC_IO_POWER_EXTERNAL
    host.pwr_ctrl_handle = s_pwr_ctrl;
#endif

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width = 4;
    slot_cfg.clk = CORE_SD_PIN_CLK;
    slot_cfg.cmd = CORE_SD_PIN_CMD;
    slot_cfg.d0 = CORE_SD_PIN_D0;
    slot_cfg.d1 = CORE_SD_PIN_D1;
    slot_cfg.d2 = CORE_SD_PIN_D2;
    slot_cfg.d3 = CORE_SD_PIN_D3;
    slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "Retry mount SD (%d/3)...", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        ret = esp_vfs_fat_sdmmc_mount(CORE_SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
        if (ret == ESP_OK) break;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount SD fallito: %s", esp_err_to_name(ret));
        s_mounted = false;
        return false;
    }
    s_mounted = true;
    ESP_LOGI(TAG, "SD montata su %s", CORE_SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    return true;
}

bool core_sd_is_mounted(void) {
    return s_mounted;
}

bool core_sd_format(void) {
    if (!s_card) {
        if (!core_sd_init()) return false;
    }
    esp_err_t err = esp_vfs_fat_sdcard_format(CORE_SD_MOUNT_POINT, s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Format SD fallito: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "SD formattata");
    return true;
}

uint64_t core_sd_free_bytes(void) {
    if (!s_mounted) return 0;
    uint64_t total = 0, free_b = 0;
    esp_err_t err = esp_vfs_fat_info(CORE_SD_MOUNT_POINT, &total, &free_b);
    if (err != ESP_OK) return 0;
    return free_b;
}

static bool delete_oldest_day_dir(void) {
    DIR *dir = opendir(CORE_SD_MOUNT_POINT);
    if (!dir) return false;

    char oldest[32] = {0};
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_type != DT_DIR && ent->d_type != DT_UNKNOWN) {
            continue;
        }
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (strlen(ent->d_name) != 8) {
            continue;
        }
        char path[48];
        snprintf(path, sizeof(path), "%s/%.8s", CORE_SD_MOUNT_POINT, ent->d_name);
        struct stat dst;
        if (stat(path, &dst) != 0 || !S_ISDIR(dst.st_mode)) {
            continue;
        }
        if (oldest[0] == 0 || strcmp(ent->d_name, oldest) < 0) {
            strncpy(oldest, ent->d_name, sizeof(oldest) - 1);
        }
    }
    closedir(dir);
    if (oldest[0] == 0) return false;

    char path[48];
    snprintf(path, sizeof(path), "%s/%.8s", CORE_SD_MOUNT_POINT, oldest);
    ESP_LOGW(TAG, "Rimuovo directory più vecchia: %s", path);

    DIR *subdir = opendir(path);
    if (subdir) {
        struct dirent *f;
        while ((f = readdir(subdir)) != nullptr) {
            if (f->d_type != DT_REG && f->d_type != DT_UNKNOWN) {
                continue;
            }
            char fpath[128];
            snprintf(fpath, sizeof(fpath), "%s/%.64s", path, f->d_name);
            struct stat fst;
            if (stat(fpath, &fst) != 0 || !S_ISREG(fst.st_mode)) {
                continue;
            }
            remove(fpath);
        }
        closedir(subdir);
    }
    return rmdir(path) == 0;
}

bool core_sd_ensure_space(uint64_t min_free) {
    if (!s_mounted && !core_sd_init()) return false;
    int guard = 0;
    while (core_sd_free_bytes() < min_free && guard < 32) {
        if (!delete_oldest_day_dir()) break;
        guard++;
    }
    return core_sd_free_bytes() >= min_free;
}

bool core_sd_make_day_dir(char *out_path, size_t out_len) {
    if (!out_path || out_len < 16) return false;
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char day[9];
    snprintf(day, sizeof(day), "%02d%02d%04d", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
    if (out_len < strlen(CORE_SD_MOUNT_POINT) + 1 + strlen(day) + 1) return false;
    snprintf(out_path, out_len, "%s/%s", CORE_SD_MOUNT_POINT, day);
    struct stat st;
    if (stat(out_path, &st) == 0 && S_ISDIR(st.st_mode)) return true;
    return mkdir(out_path, 0755) == 0;
}

bool core_sd_make_recording_path(const char *day_dir, uint32_t core_id,
                                 char *out_path, size_t out_len) {
    if (!day_dir || !out_path) return false;
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    snprintf(out_path, out_len, "%s/%05lu_%02d%02d_%02d%02d%02d.mp4",
             day_dir,
             (unsigned long)core_id,
             t.tm_mday, t.tm_mon + 1,
             t.tm_hour, t.tm_min, t.tm_sec);
    return true;
}
