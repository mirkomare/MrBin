#include "CoreSD.h"
#include "CoreConfig.h"

#include "dirent.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sys/stat.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "core_sd";
static sdmmc_card_t *s_card = nullptr;
static bool s_mounted = false;

bool core_sd_init(void) {
    if (s_mounted) return true;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width = 4;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(CORE_SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount SD fallito: %s", esp_err_to_name(ret));
        s_mounted = false;
        return false;
    }
    s_mounted = true;
    ESP_LOGI(TAG, "SD montata su %s", CORE_SD_MOUNT_POINT);
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
        if (ent->d_type != DT_DIR) continue;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (strlen(ent->d_name) != 8) continue;
        if (oldest[0] == 0 || strcmp(ent->d_name, oldest) < 0) {
            strncpy(oldest, ent->d_name, sizeof(oldest) - 1);
        }
    }
    closedir(dir);
    if (oldest[0] == 0) return false;

    char path[64];
    snprintf(path, sizeof(path), "%s/%s", CORE_SD_MOUNT_POINT, oldest);
    ESP_LOGW(TAG, "Rimuovo directory più vecchia: %s", path);

    DIR *subdir = opendir(path);
    if (subdir) {
        struct dirent *f;
        while ((f = readdir(subdir)) != nullptr) {
            if (f->d_type != DT_REG) continue;
            char fpath[96];
            snprintf(fpath, sizeof(fpath), "%s/%s", path, f->d_name);
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
    char day[16];
    snprintf(day, sizeof(day), "%02d%02d%04d", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
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
