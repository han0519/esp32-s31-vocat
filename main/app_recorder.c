/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SD-card MJPEG recorder.
 */
#include "app_recorder.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "app_camera.h"
#include "app_activity.h"
#include "app_sdcard.h"

static const char *TAG = "RECORDER";

#define REC_SRC_FPS 15   /* camera streaming rate */

static bool s_rec = false;
static FILE *s_fp = NULL;
static int s_frame = 0;
static int s_interval = 3;
static int s_cb_count = 0;
static char s_name[48];      /* e.g. rec_0000123456 */
static char s_path[160];     /* mjpg path */
static char s_manifest[160]; /* json path */
static SemaphoreHandle_t s_mut = NULL;

static void rec_frame_cb(const uint8_t *jpeg, size_t len, void *ctx)
{
    (void)ctx;
    if (s_mut == NULL) {
        return;
    }
    xSemaphoreTake(s_mut, portMAX_DELAY);
    if (!s_rec || s_fp == NULL) {
        xSemaphoreGive(s_mut);
        return;
    }
    if (++s_cb_count < s_interval) {
        xSemaphoreGive(s_mut);
        return;
    }
    s_cb_count = 0;
    if (fwrite(jpeg, 1, len, s_fp) == len) {
        s_frame++;
    }
    xSemaphoreGive(s_mut);
}

esp_err_t app_recorder_start(int fps)
{
    if (s_rec) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!app_sdcard_mounted()) {
        ESP_LOGW(TAG, "SD card not mounted; cannot record");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_mut == NULL) {
        s_mut = xSemaphoreCreateMutex();
    }
    if (app_sdcard_ensure_record_dir() != ESP_OK) {
        return ESP_FAIL;
    }

    /* Unique recording name from boot microseconds. */
    uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    snprintf(s_name, sizeof(s_name), "rec_%u", (unsigned int)sec);
    snprintf(s_path, sizeof(s_path), "%s/%s.mjpg", APP_SDCARD_RECORD_DIR, s_name);
    snprintf(s_manifest, sizeof(s_manifest), "%s/%s.json", APP_SDCARD_RECORD_DIR, s_name);

    s_fp = fopen(s_path, "wb");
    if (s_fp == NULL) {
        ESP_LOGE(TAG, "cannot open %s", s_path);
        return ESP_FAIL;
    }
    s_frame = 0;
    s_cb_count = 0;
    s_interval = (fps > 0) ? (REC_SRC_FPS / fps) : 3;
    if (s_interval < 1) {
        s_interval = 1;
    }
    s_rec = true;

    esp_err_t r = app_camera_register_frame_cb(rec_frame_cb, NULL);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "register frame cb failed: %s", esp_err_to_name(r));
        fclose(s_fp);
        s_fp = NULL;
        s_rec = false;
        return r;
    }
    app_activity_camera_acquire(APP_ACT_CAMERA_REC);   /* keep camera for recording */
    ESP_LOGI(TAG, "recording -> %s (interval=%d, target %dfps)", s_path, s_interval, fps);
    return ESP_OK;
}

esp_err_t app_recorder_stop(void)
{
    if (!s_rec) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mut, portMAX_DELAY);
    s_rec = false;
    FILE *fp = s_fp;
    s_fp = NULL;
    xSemaphoreGive(s_mut);

    int frames = s_frame;
    long size = 0;
    if (fp) {
        size = ftell(fp);
        fclose(fp);
    }
    app_camera_unregister_frame_cb(rec_frame_cb);
    app_activity_camera_release(APP_ACT_CAMERA_REC);   /* camera may sleep now */

    /* Write a small manifest so the web UI can list recordings. */
    FILE *jf = fopen(s_manifest, "w");
    if (jf) {
        int fps = (s_interval > 0) ? (REC_SRC_FPS / s_interval) : REC_SRC_FPS;
        fprintf(jf,
                "{\"name\":\"%s\",\"frames\":%d,\"fps\":%d,\"size\":%ld,\"width\":1280,\"height\":720}",
                s_name, frames, fps, size);
        fclose(jf);
    }
    ESP_LOGI(TAG, "recording stopped: %d frames, %ld bytes -> %s", frames, size, s_name);
    return ESP_OK;
}

bool app_recorder_is_recording(void)
{
    return s_rec;
}

int app_recorder_frame_count(void)
{
    return s_frame;
}
