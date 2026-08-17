/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * App-grid launcher (手机桌面) for the ESP-VoCat-S31 "喵伴".
 */
#pragma once

#include "esp_err.h"
#include "ui_page_manager.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const ui_page_t page_launcher;

/**
 * @brief Create a small "返回" button at the top-left of a feature page root.
 *        Tapping it switches back to the emote face (home).
 *
 * @param[in] root  page root LVGL object
 * @return the button object (or NULL on failure)
 */
lv_obj_t *ui_page_make_back_button(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
