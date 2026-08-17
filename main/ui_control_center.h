/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pull-down control center for the ESP-VoCat-S31 "喵伴" pet.
 *
 * A frosted-white panel that slides down from the top when the user swipes down
 * anywhere. It hosts a "重置 WiFi" button and volume / brightness sliders, so
 * the pet can be tuned without a phone. Swiping down again, swiping up, or
 * tapping the empty area closes it.
 */
#pragma once

#include "esp_err.h"
#include "ui_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Initialize the control center (creates its LVGL objects once).
 *         Must be called after the LVGL display is up (lvgl_port).
 */
esp_err_t ui_control_center_init(void);

/**
 * @brief  Open / close the pull-down panel.
 */
void ui_control_center_open(void);
void ui_control_center_close(void);

/**
 * @brief  True when the panel is currently open.
 */
bool ui_control_center_is_open(void);

/**
 * @brief  Feed a gesture to the control center. Returns true if the gesture was
 *         consumed (panel toggled) — the caller should NOT also treat it as a
 *         page navigation in that case.
 */
bool ui_control_center_on_gesture(ui_gesture_t g, int x, int y);

#ifdef __cplusplus
}
#endif
