/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Page state machine for the ESP-VoCat-S31 pet UI.
 *
 * Pages are either the animated FACE (driven by esp_emote_expression, which owns
 * the panel) or a PAGE (driven by LVGL, used for the read-only dialogue / camera
 * / music / game pages). Only one renderer can drive the panel at a time, so the
 * manager toggles between them via esp_xiaozhi_chat_display_set_face_visible().
 *
 * Swipe left/right navigates between pages by default; a page may consume a
 * gesture (return true from on_gesture) to override navigation.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "ui_touch.h"
#include "app_chat_history.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PAGE_FACE = 0,       /* emote face (home / 表情主页) */
    PAGE_LAUNCHER,       /* app-grid desktop (手机桌面, 上滑唤出) */
    PAGE_DIALOGUE,
    PAGE_CAMERA,
    PAGE_MUSIC,
    PAGE_GAME,
    PAGE_TIMER,
    PAGE_MAX,
} ui_page_id_t;

typedef struct {
    ui_page_id_t id;
    const char  *name;
    esp_err_t (*on_enter)(void);
    esp_err_t (*on_exit)(void);
    bool      (*on_gesture)(ui_gesture_t g);   /* return true to consume */
    void      (*on_tick)(uint32_t ms);
    void      (*on_chat)(chat_role_t role, const char *text);
    void      (*on_status)(const char *status);
} ui_page_t;

/**
 * @brief  Register the page table. Pages are indexed by their position in the
 *         array; navigation wraps around. Pass an array of pointers to the
 *         page structs (not copied).
 */
esp_err_t ui_page_manager_init(const ui_page_t *pages[], int count);

/**
 * @brief  Switch to a page by id. Handles render-mode toggle and calls
 *         on_exit / on_enter. No-op if already on that page.
 */
esp_err_t ui_page_manager_switch(ui_page_id_t id);

/**
 * @brief  Current page id (PAGE_FACE if manager not started).
 */
ui_page_id_t ui_page_manager_current(void);

/**
 * @brief  Periodic tick forwarded to the active page (ms since boot).
 */
void ui_page_manager_tick(uint32_t ms);

/**
 * @brief  Dispatch a gesture to the active page; performs default swipe
 *         navigation if the page does not consume it. Matches ui_touch_cb_t.
 */
void ui_page_manager_dispatch_gesture(ui_gesture_t g, int x, int y);

/**
 * @brief  Forward a chat turn to the active page (live transcript update).
 */
void ui_page_manager_notify_chat(chat_role_t role, const char *text);

/**
 * @brief  Forward a status string to the active page (Listening/Speaking/Error...).
 */
void ui_page_manager_notify_status(const char *status);

/**
 * @brief  Monotonic milliseconds since boot (as fed to on_tick). Useful for
 *         pages that need to time transient reactions from a gesture handler.
 */
uint32_t ui_page_manager_now_ms(void);

#ifdef __cplusplus
}
#endif
