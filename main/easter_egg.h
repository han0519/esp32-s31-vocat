/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Qixi (七夕) easter egg for the ESP-VoCat-S31 "喵伴".
 *
 * Triggered by tapping the face 5 times quickly: shows a big "七夕快乐"
 * banner and floats red hearts up the screen. Lives on lv_layer_top() so it
 * overlays whatever page is active. Self-terminates after a few seconds.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Init the easter egg overlay (lazy: builds on first show).
 */
esp_err_t easter_egg_init(void);

/**
 * @brief  Show the Qixi banner + floating hearts for a few seconds.
 */
void easter_egg_show(void);

/**
 * @brief  Feed a tap count; returns true when the egg fires. Used by the face
 *         page to detect the "tap 5x fast" combo.
 */
bool easter_egg_tap(void);

#ifdef __cplusplus
}
#endif
