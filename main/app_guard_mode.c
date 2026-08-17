/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_guard_mode.c — 番茄时钟"守护模式" backend (see header).
 */
#include "app_guard_mode.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "app_camera.h"
#include "app_camera_preview.h"
#include "app_vision_lite.h"
#include "app_activity.h"

static const char *TAG = "GUARD_MODE";

/* DVP capture resolution (matches app_camera.c's internal CAM_WIDTH/HEIGHT). */
#define GUARD_SRC_W 1280
#define GUARD_SRC_H 720

/* Analyze the preview every 250 ms (4 fps) — plenty for motion/activity and
 * leaves the CPU to LVGL + audio. */
#define GUARD_ANALYZE_MS    250
/* Nobody-at-the-desk alert after this long without meaningful motion. */
#define GUARD_NOBODY_ALERT_MS 60000

static SemaphoreHandle_t s_mtx = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;
static volatile bool s_exited = false;

static uint8_t *s_frame = NULL;
static size_t   s_frame_len = 0;
static uint32_t s_frame_seq = 0;

static guard_status_t s_status = GUARD_STATUS_IDLE;
static int s_last_ratio = 0;
static uint32_t s_last_active_ms = 0;
static uint32_t s_absent_start_ms = 0;

#define GUARD_STACK 12 * 1024

static void on_yuv(const uint8_t *uyvy, int w, int h, void *ctx)
{
    (void)w;
    (void)h;
    (void)ctx;
    if (uyvy == NULL || !s_running || s_mtx == NULL) {
        return;
    }
    if (xSemaphoreTake(s_mtx, 0) != pdTRUE) {
        return;
    }
    size_t len = (size_t)GUARD_SRC_W * GUARD_SRC_H * 2;
    if (s_frame && s_frame_len >= len) {
        memcpy(s_frame, uyvy, len);
        s_frame_len = len;
    } else {
        uint8_t *nb = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (nb) {
            if (s_frame) heap_caps_free(s_frame);
            s_frame = nb;
            memcpy(s_frame, uyvy, len);
            s_frame_len = len;
        }
    }
    s_frame_seq++;
    xSemaphoreGive(s_mtx);
}

static void guard_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "guard task started");

    /* PPA output buffer: 64-byte aligned PSRAM (required by ppa_core). */
    size_t out_size = CAM_PREVIEW_W * CAM_PREVIEW_H * 2;
    uint8_t *out = (uint8_t *)heap_caps_aligned_alloc(64, out_size,
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out == NULL) {
        ESP_LOGE(TAG, "guard out buf alloc failed");
        s_exited = true;
        vTaskDelete(NULL);
        return;
    }

    uint32_t last_seq = 0;
    uint32_t last_analyze = 0;
    while (s_running) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        bool has = (s_frame != NULL && s_frame_len >= (size_t)(GUARD_SRC_W * GUARD_SRC_H * 2));
        uint32_t seq = s_frame_seq;
        xSemaphoreGive(s_mtx);
        if (!has || seq == last_seq) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        last_seq = seq;

        if (now - last_analyze < GUARD_ANALYZE_MS) {
            continue;
        }
        last_analyze = now;

        xSemaphoreTake(s_mtx, portMAX_DELAY);
        uint8_t *yuv = s_frame;
        size_t len = s_frame_len;
        xSemaphoreGive(s_mtx);

        if (len >= (size_t)(GUARD_SRC_W * GUARD_SRC_H * 2) &&
            app_camera_preview_process(yuv, GUARD_SRC_W, GUARD_SRC_H, out) == ESP_OK) {
            app_vision_lite_feed_rgb565(out, CAM_PREVIEW_W, CAM_PREVIEW_H, now);
        }
        vTaskDelay(pdMS_TO_TICKS(GUARD_ANALYZE_MS / 2));
    }

    heap_caps_free(out);
    if (xSemaphoreTake(s_mtx, 0) == pdTRUE) {
        if (s_frame) {
            heap_caps_free(s_frame);
            s_frame = NULL;
        }
        s_frame_len = 0;
        xSemaphoreGive(s_mtx);
    }
    s_exited = true;
    vTaskDelete(NULL);
}

esp_err_t app_guard_mode_start(void)
{
    if (s_running) {
        return ESP_OK;
    }
    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateMutex();
        if (s_mtx == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    app_activity_camera_acquire(APP_ACT_CAMERA_GUARD);   /* demand-driven start */

    app_vision_lite_init();
    app_vision_lite_set_enabled(true);

    s_running = true;
    s_exited = false;
    s_status = GUARD_STATUS_STARTING;
    s_last_active_ms = 0;
    s_absent_start_ms = 0;

    app_camera_register_yuv_cb(on_yuv, NULL);

    BaseType_t t = xTaskCreatePinnedToCoreWithCaps(guard_task, "guard", GUARD_STACK, NULL,
                                                   3, &s_task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (t != pdPASS) {
        ESP_LOGE(TAG, "guard task create failed");
        app_camera_register_yuv_cb(NULL, NULL);
        app_vision_lite_set_enabled(false);
        app_activity_camera_release(APP_ACT_CAMERA_GUARD);
        s_running = false;
        return ESP_FAIL;
    }
    s_status = GUARD_STATUS_QUIET;
    ESP_LOGI(TAG, "guard mode started");
    return ESP_OK;
}

esp_err_t app_guard_mode_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }
    s_running = false;
    app_camera_register_yuv_cb(NULL, NULL);
    app_vision_lite_set_enabled(false);

    int guard = 0;
    while (!s_exited && guard++ < 50) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_task = NULL;
    s_exited = false;

    app_activity_camera_release(APP_ACT_CAMERA_GUARD);   /* stop on demand */
    if (xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE) {
        s_status = GUARD_STATUS_IDLE;
        s_last_ratio = 0;
        xSemaphoreGive(s_mtx);
    }
    ESP_LOGI(TAG, "guard mode stopped");
    return ESP_OK;
}

bool app_guard_mode_is_running(void)
{
    return s_running;
}

guard_info_t app_guard_mode_get_info(void)
{
    guard_info_t info;
    memset(&info, 0, sizeof(info));
    if (s_mtx == NULL) {
        return info;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    info.status = s_status;
    info.motion_ratio = s_last_ratio;
    info.absent_ms = s_absent_start_ms;
    xSemaphoreGive(s_mtx);
    return info;
}
