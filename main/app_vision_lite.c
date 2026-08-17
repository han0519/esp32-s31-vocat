/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_vision_lite.c — lightweight CPU-only visual analysis (see header).
 *
 * Pipeline per frame (all integer math, no FPU dependency beyond what we avoid):
 *   RGB565 preview (240x240) -> 2x2 box average -> gray 120x120
 *   -> |gray - prev_gray| threshold -> motion mask
 *   -> motion_ratio + activity grading + sliding "wave" detector.
 *
 * Buffers are 120x120 bytes (~14 KB each) in PSRAM, allocated once.
 */
#include "app_vision_lite.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "VISION_LITE";

/* Motion pixel threshold: gray delta above this counts as changed. */
#define MOTION_THRESH   18
/* Motion ratio (percent) above which the frame is "moving". */
#define MOTION_RATIO_ON 3
/* Activity grading boundaries (percent). */
#define QUIET_MAX       12
#define ACTIVE_MIN      24
/* Wave detection: motion_ratio must oscillate up/down across this many times
 * within the window while the centroid stays roughly in the middle third. */
#define WAVE_WINDOW     24          /* frames (at ~15fps ≈ 1.6 s) */
#define WAVE_MIN_SWINGS 4

typedef struct {
    /* Current + previous grayscale frames. `gray` is reused by every feed call
     * (no 14 KB stack array -> the camera preview and guard tasks both have
     * only 8 KB stacks and would overflow on a stack buffer). All PSRAM. */
    uint8_t gray[VISION_GRAY_W * VISION_GRAY_H];
    uint8_t prev[VISION_GRAY_W * VISION_GRAY_H];
} vision_buf_t;

static vision_buf_t *s_buf = NULL;          /* PSRAM */
static SemaphoreHandle_t s_mtx = NULL;

static app_vision_result_t s_res;
static bool s_enabled = false;

/* Wave detector state. */
static int  s_ratio_hist[WAVE_WINDOW];
static int  s_ratio_pos = 0;
static int  s_swings = 0;
static bool s_last_up = false;

static void reset_wave_state(void)
{
    memset(s_ratio_hist, 0, sizeof(s_ratio_hist));
    s_ratio_pos = 0;
    s_swings = 0;
    s_last_up = false;
}

esp_err_t app_vision_lite_init(void)
{
    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateMutex();
        if (s_mtx == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_buf == NULL) {
        s_buf = (vision_buf_t *)heap_caps_malloc(sizeof(vision_buf_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
        memset(s_buf, 0, sizeof(vision_buf_t));
        memset(&s_res, 0, sizeof(s_res));
    }
    return ESP_OK;
}

static void analyze(const uint8_t *gray, int gw, int gh, uint32_t ms)
{
    const int n = gw * gh;

    /* 1. frame difference against prev (copy current first for next frame). */
    int changed = 0;
    for (int i = 0; i < n; i++) {
        int diff = (int)gray[i] - (int)s_buf->prev[i];
        if (diff < 0) diff = -diff;
        if (diff > MOTION_THRESH) {
            changed++;
        }
    }
    memcpy(s_buf->prev, gray, (size_t)n);

    int ratio = (n > 0) ? (changed * 100 / n) : 0;

    /* 2. activity grading. */
    vision_activity_t act = VISION_ACTIVITY_NONE;
    if (ratio > MOTION_RATIO_ON) {
        act = VISION_ACTIVITY_QUIET;
    }
    if (ratio > ACTIVE_MIN) {
        act = VISION_ACTIVITY_ACTIVE;
    }

    /* 3. wave detector: motion_ratio oscillates across time. */
    s_ratio_hist[s_ratio_pos] = ratio;
    s_ratio_pos = (s_ratio_pos + 1) % WAVE_WINDOW;
    if (ratio > QUIET_MAX) {
        bool up = (ratio > s_ratio_hist[(s_ratio_pos - 1 + WAVE_WINDOW) % WAVE_WINDOW]);
        if (up != s_last_up && ratio > ACTIVE_MIN) {
            s_swings++;
        }
        s_last_up = up;
    } else {
        s_swings = 0;
        s_last_up = false;
    }
    bool wave = (s_swings >= WAVE_MIN_SWINGS);

    /* 4. snapshot result under the lock. */
    if (xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE) {
        s_res.motion = (ratio > MOTION_RATIO_ON);
        s_res.motion_ratio = ratio;
        s_res.activity = act;
        s_res.wave = wave;
        s_res.last_ms = ms;
        xSemaphoreGive(s_mtx);
    }
}

/* Downscale RGB565 (w x h) to gray gw x gh via 2x2-ish box average. */
static void rgb565_to_gray(const uint8_t *rgb565, int w, int h, uint8_t *gray, int gw, int gh)
{
    for (int gy = 0; gy < gh; gy++) {
        int y0 = gy * h / gh;
        int y1 = (gy + 1) * h / gh;
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > h) y1 = h;
        for (int gx = 0; gx < gw; gx++) {
            int x0 = gx * w / gw;
            int x1 = (gx + 1) * w / gw;
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > w) x1 = w;
            long sum = 0;
            int cnt = 0;
            for (int y = y0; y < y1; y++) {
                const uint16_t *row = (const uint16_t *)(rgb565 + (size_t)y * w * 2);
                for (int x = x0; x < x1; x++) {
                    uint16_t px = row[x];
                    /* RGB565 -> Y (BT.601-ish, integer) */
                    int r = (px >> 11) & 0x1F;
                    int g = (px >> 5) & 0x3F;
                    int b = px & 0x1F;
                    int yv = (r * 77 + g * 150 + b * 29) >> 8;   /* 0..~63 */
                    sum += yv;
                    cnt++;
                }
            }
            gray[gy * gw + gx] = (uint8_t)(cnt ? (sum / cnt) : 0);
        }
    }
}

void app_vision_lite_feed_rgb565(const uint8_t *rgb565, int w, int h, uint32_t ms)
{
    if (!s_enabled || s_buf == NULL || rgb565 == NULL || w <= 0 || h <= 0) {
        return;
    }
    rgb565_to_gray(rgb565, w, h, s_buf->gray, VISION_GRAY_W, VISION_GRAY_H);
    analyze(s_buf->gray, VISION_GRAY_W, VISION_GRAY_H, ms);
}

/* Downscale UYVY (w x h) to gray gw x gh. UYVY: each 4 bytes = U Y0 V Y1. */
static void uyvy_to_gray(const uint8_t *uyvy, int w, int h, uint8_t *gray, int gw, int gh)
{
    for (int gy = 0; gy < gh; gy++) {
        int y0 = gy * h / gh;
        int y1 = (gy + 1) * h / gh;
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > h) y1 = h;
        for (int gx = 0; gx < gw; gx++) {
            int x0 = gx * w / gw;
            int x1 = (gx + 1) * w / gw;
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > w) x1 = w;
            long sum = 0;
            int cnt = 0;
            for (int y = y0; y < y1; y++) {
                const uint8_t *row = uyvy + (size_t)y * w * 2;
                for (int x = x0; x < x1; x++) {
                    /* Even x -> Y after U; odd x -> Y after V. */
                    int xi = x & ~1;
                    int yv = row[xi + 1];
                    sum += yv;
                    cnt++;
                }
            }
            gray[gy * gw + gx] = (uint8_t)(cnt ? (sum / cnt) : 0);
        }
    }
}

void app_vision_lite_feed_uyvy(const uint8_t *uyvy, int w, int h, uint32_t ms)
{
    if (!s_enabled || s_buf == NULL || uyvy == NULL || w <= 0 || h <= 0) {
        return;
    }
    uyvy_to_gray(uyvy, w, h, s_buf->gray, VISION_GRAY_W, VISION_GRAY_H);
    analyze(s_buf->gray, VISION_GRAY_W, VISION_GRAY_H, ms);
}

app_vision_result_t app_vision_lite_get_result(void)
{
    app_vision_result_t r;
    memset(&r, 0, sizeof(r));
    if (s_mtx == NULL) {
        return r;
    }
    if (xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE) {
        r = s_res;
        xSemaphoreGive(s_mtx);
    }
    return r;
}

void app_vision_lite_set_enabled(bool en)
{
    s_enabled = en;
    if (en && s_buf) {
        memset(s_buf->prev, 0, sizeof(s_buf->prev));
        reset_wave_state();
    }
    if (!en && s_mtx) {
        if (xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE) {
            memset(&s_res, 0, sizeof(s_res));
            xSemaphoreGive(s_mtx);
        }
    }
    ESP_LOGI(TAG, "vision analyzer %s", en ? "enabled" : "disabled");
}

bool app_vision_lite_is_enabled(void)
{
    return s_enabled;
}
