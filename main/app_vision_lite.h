/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_vision_lite.h — lightweight CPU-only visual analysis for the
 * ESP-VoCat-S31 "喵伴".
 *
 * Provides motion detection, activity grading and a simple "wave" (挥手)
 * detector that run on the low-resolution preview frame WITHOUT any deep
 * learning model. The S31 cannot currently run esp-dl/YOLO11 (unverified
 * support, only ~912KB model partition, tight internal RAM), so this module is
 * the practical offline alternative:
 *
 *   - feed it the PPA RGB565 preview (240x240) each frame
 *   - it downsamples to grayscale, computes inter-frame difference
 *   - reports: motion ratio, activity level (0=没人 1=安静 2=活跃), wave flag
 *
 * Used by the camera page (offline gesture indicator) and the pomodoro "守护
 * 模式" (watch whether the child is still at the desk).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VISION_GRAY_W   120
#define VISION_GRAY_H   120

typedef enum {
    VISION_ACTIVITY_NONE = 0,   /* no movement (nobody at the desk)   */
    VISION_ACTIVITY_QUIET,      /* slight movement (sitting/reading)  */
    VISION_ACTIVITY_ACTIVE,     /* strong movement (writing/hand up)  */
} vision_activity_t;

typedef struct {
    bool             motion;          /* any meaningful movement this frame    */
    int              motion_ratio;    /* 0..100 changed-pixel percentage       */
    vision_activity_t activity;        /* graded activity level                 */
    bool             wave;            /* wave gesture detected                  */
    uint32_t         last_ms;         /* monotonic ms of the last feed          */
} app_vision_result_t;

/**
 * @brief Init the analyzer (allocates the grayscale + prev buffers in PSRAM).
 *        Safe to call repeatedly.
 */
esp_err_t app_vision_lite_init(void);

/**
 * @brief Feed one RGB565 preview frame (w x h). Runs a lightweight downscale to
 *        grayscale + inter-frame diff. Cheap (<1 ms for 240x240 on 320 MHz).
 *        `w`/`h` should be the PPA preview size (CAM_PREVIEW_W/H).
 */
void app_vision_lite_feed_rgb565(const uint8_t *rgb565, int w, int h, uint32_t ms);

/**
 * @brief Feed one raw UYVY frame directly (1280x720). Downscales in software to
 *        grayscale. Slower than feeding the PPA preview, use only when the PPA
 *        path is unavailable (e.g. guard mode without the camera page).
 */
void app_vision_lite_feed_uyvy(const uint8_t *uyvy, int w, int h, uint32_t ms);

/**
 * @brief Get the latest analysis result (thread-safe snapshot).
 */
app_vision_result_t app_vision_lite_get_result(void);

/**
 * @brief Enable / disable analysis. When disabled the analyzer returns a clean
 *        (no motion) result and stops consuming CPU.
 */
void app_vision_lite_set_enabled(bool en);
bool app_vision_lite_is_enabled(void);

#ifdef __cplusplus
}
#endif
