/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SD card 1-bit SDIO implementation.
 */
#include "app_sdcard.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"

static const char *TAG = "APP_SDCARD";

static bool s_mounted = false;
static sdmmc_card_t *s_card = NULL;

/* GPIO mapping confirmed by schematic image: DAT0=20, CLK=24, CMD=25 */
#define SDCARD_PIN_D0  GPIO_NUM_20
#define SDCARD_PIN_CLK GPIO_NUM_24
#define SDCARD_PIN_CMD GPIO_NUM_25

static int str_ends_with_ignore_case(const char *str, const char *suffix)
{
    size_t len = strlen(str);
    size_t suf = strlen(suffix);
    if (len < suf) return 0;
    const char *p = str + len - suf;
    while (*p && *suffix) {
        char a = *p;
        char b = *suffix;
        if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
        if (b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
        if (a != b) return 0;
        p++;
        suffix++;
    }
    return 1;
}

esp_err_t app_sdcard_init(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = SDCARD_PIN_CLK;
    slot_config.cmd = SDCARD_PIN_CMD;
    slot_config.d0  = SDCARD_PIN_D0;
    slot_config.d1 = GPIO_NUM_NC;
    slot_config.d2 = GPIO_NUM_NC;
    slot_config.d3 = GPIO_NUM_NC;
    slot_config.cd = GPIO_NUM_NC;
    slot_config.wp = GPIO_NUM_NC;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(APP_SDCARD_MOUNT_POINT, &host, &slot_config,
                                            &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed (%s)", esp_err_to_name(ret));
        return ret;
    }

    s_mounted = true;
    uint64_t total_bytes = ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size;
    ESP_LOGI(TAG, "SD card mounted: %llu MB total", total_bytes / (1024 * 1024));
    return ESP_OK;
}

bool app_sdcard_mounted(void)
{
    return s_mounted;
}

const char *app_sdcard_mount_point(void)
{
    return APP_SDCARD_MOUNT_POINT;
}

int app_sdcard_scan_music(app_sdcard_music_item_t *out_items, int max_items)
{
    if (!s_mounted || out_items == NULL || max_items <= 0) {
        return 0;
    }

    DIR *dir = opendir(APP_SDCARD_MUSIC_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "cannot open %s", APP_SDCARD_MUSIC_DIR);
        return 0;
    }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max_items) {
        if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) {
            continue;
        }
        if (!(str_ends_with_ignore_case(ent->d_name, ".mp3") ||
              str_ends_with_ignore_case(ent->d_name, ".wav") ||
              str_ends_with_ignore_case(ent->d_name, ".aac") ||
              str_ends_with_ignore_case(ent->d_name, ".m4a") ||
              str_ends_with_ignore_case(ent->d_name, ".flac"))) {
            continue;
        }
        /* esp_audio_simple_player expects the "file://sdcard/..." form for
         * local files (VFS mount point is registered as "sdcard"). */
        snprintf(out_items[count].path, sizeof(out_items[count].path),
                 "file://sdcard/%s", ent->d_name);
        strncpy(out_items[count].title, ent->d_name,
                sizeof(out_items[count].title) - 1);
        out_items[count].title[sizeof(out_items[count].title) - 1] = '\0';
        count++;
    }
    closedir(dir);

    ESP_LOGI(TAG, "scanned %d music files", count);
    return count;
}

esp_err_t app_sdcard_ensure_record_dir(void)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    struct stat st;
    if (stat(APP_SDCARD_RECORD_DIR, &st) == 0 && S_ISDIR(st.st_mode)) {
        return ESP_OK;
    }
    if (mkdir(APP_SDCARD_RECORD_DIR, 0755) == 0) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t app_sdcard_info(uint64_t *out_total_bytes, uint64_t *out_free_bytes)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    FATFS *fs;
    DWORD free_clusters;
    FRESULT res = f_getfree(APP_SDCARD_MOUNT_POINT, &free_clusters, &fs);
    if (res != FR_OK) {
        return ESP_FAIL;
    }
    uint64_t total_sectors = (uint64_t)(fs->n_fatent - 2) * fs->csize;
    uint64_t free_sectors  = (uint64_t)free_clusters * fs->csize;
    uint64_t sector_size   = fs->ssize;
    if (out_total_bytes) *out_total_bytes = total_sectors * sector_size;
    if (out_free_bytes)  *out_free_bytes  = free_sectors * sector_size;
    return ESP_OK;
}
