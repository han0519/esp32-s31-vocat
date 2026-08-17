/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SD card (1-bit SDIO) support for VoCat-S31.
 * Pins from schematic: DAT0=GPIO20, CLK=GPIO24, CMD=GPIO25.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_SDCARD_MOUNT_POINT "/sdcard"
#define APP_SDCARD_MUSIC_DIR     "/sdcard"
#define APP_SDCARD_RECORD_DIR    "/sdcard/record"

typedef struct {
    char path[300];   /* playable URI, e.g. "file://sdcard/song.mp3" */
    char title[128];  /* display name (file name) */
} app_sdcard_music_item_t;

/**
 * @brief  Mount the SD card to /sdcard using 1-bit SDIO.
 */
esp_err_t app_sdcard_init(void);

/**
 * @brief  Return true if the card is mounted.
 */
bool app_sdcard_mounted(void);

/**
 * @brief  Mount point string ("/sdcard").
 */
const char *app_sdcard_mount_point(void);

/**
 * @brief  Scan the SD card root for music files.
 * @param out_items  caller-allocated array
 * @param max_items  length of array
 * @return number of files found
 */
int app_sdcard_scan_music(app_sdcard_music_item_t *out_items, int max_items);

/**
 * @brief  Ensure /sdcard/record directory exists.
 */
esp_err_t app_sdcard_ensure_record_dir(void);

/**
 * @brief  Free-space / total-space in bytes.
 */
esp_err_t app_sdcard_info(uint64_t *out_total_bytes, uint64_t *out_free_bytes);

#ifdef __cplusplus
}
#endif
