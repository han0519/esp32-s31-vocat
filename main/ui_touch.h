/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Touch gesture layer for the ESP-VoCat-S31 (CST816S capacitive touch).
 *
 * Polls the touch controller and classifies raw contacts into high-level
 * gestures (tap / long-press / swipe left-right-up-down). Business logic lives
 * in the page manager; this layer only emits gestures via a callback so it can
 * be unit-reasoned about independently of the UI.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_GESTURE_NONE = 0,
    UI_GESTURE_TAP,
    UI_GESTURE_LONG_PRESS,
    UI_GESTURE_SWIPE_LEFT,
    UI_GESTURE_SWIPE_RIGHT,
    UI_GESTURE_SWIPE_UP,
    UI_GESTURE_SWIPE_DOWN,
} ui_gesture_t;

/* Tuning thresholds (in pixels / milliseconds). */
#define UI_SWIPE_MIN_DIST_PX   28
#define UI_LONG_PRESS_MS       450
#define UI_TAP_MAX_MS          320

typedef void (*ui_touch_cb_t)(ui_gesture_t gesture, int x, int y);

/**
 * @brief  Initialize the touch layer. Acquires the "lcd_touch" board handle and
 *         starts an internal polling task.
 */
esp_err_t ui_touch_init(void);

/**
 * @brief  Register the gesture callback. Only one callback is supported.
 */
void ui_touch_set_callback(ui_touch_cb_t cb);

/**
 * @brief  Read the current raw touch state (last polled). Used by the LVGL
 *         pointer input device so LVGL widgets (buttons / sliders) respond to
 *         taps without a second driver reading the touch controller.
 *
 * @param[out] x        Touch X (or -1 when not pressed)
 * @param[out] y        Touch Y (or -1 when not pressed)
 * @param[out] pressed  true while a finger is down
 */
void ui_touch_get_pos(int *x, int *y, bool *pressed);

/**
 * @brief  Stop the polling task and release the touch layer.
 */
esp_err_t ui_touch_deinit(void);

#ifdef __cplusplus
}
#endif
