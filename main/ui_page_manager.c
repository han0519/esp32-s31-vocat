/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ui_page_manager.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "esp_xiaozhi_chat_display.h"
#include "ui_control_center.h"
#include "lvgl.h"

static const char *TAG = "UI_PAGE_MGR";

static const ui_page_t **s_pages = NULL;
static int s_count = 0;
static int s_cur = -1;
static uint32_t s_tick_ms = 0;
static esp_timer_handle_t s_tick_timer = NULL;

static void tick_timer_cb(void *arg)
{
    (void)arg;
    ui_page_manager_tick((uint32_t)(esp_timer_get_time() / 1000));
}

/* Persistent page indicator (dots) drawn on the LVGL screen, independent of the
 * page root so it survives page-root clears. */
static lv_obj_t *s_indicator = NULL;

static void indicator_rebuild(void)
{
    if (!lvgl_port_lock(1000)) {
        return;
    }
    if (s_indicator) {
        lv_obj_del(s_indicator);
        s_indicator = NULL;
    }
    if (s_count <= 0 || s_cur < 0) {
        lvgl_port_unlock();
        return;
    }
    s_indicator = lv_obj_create(lv_screen_active());
    if (!s_indicator) {
        lvgl_port_unlock();
        return;
    }
    lv_obj_set_size(s_indicator, LV_HOR_RES, 14);
    lv_obj_align(s_indicator, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_flex_flow(s_indicator, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_indicator, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(s_indicator, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_indicator, 0, 0);
    lv_obj_set_style_pad_column(s_indicator, 6, 0);
    lv_obj_clear_flag(s_indicator, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < s_count; ++i) {
        lv_obj_t *dot = lv_obj_create(s_indicator);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, 4, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        if (i == s_cur) {
            lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_30, 0);
        }
    }
    lvgl_port_unlock();
}

esp_err_t ui_page_manager_init(const ui_page_t *pages[], int count)
{
    ESP_RETURN_ON_FALSE(pages && count > 0, ESP_ERR_INVALID_ARG, TAG, "bad pages");
    s_pages = pages;
    s_count = count;
    s_cur = -1;

    if (s_tick_timer == NULL) {
        esp_timer_create_args_t t = {
            .callback = tick_timer_cb,
            .name = "ui_page_tick",
        };
        esp_timer_create(&t, &s_tick_timer);
        esp_timer_start_periodic(s_tick_timer, 50 * 1000);
    }
    return ESP_OK;
}

esp_err_t ui_page_manager_switch(ui_page_id_t id)
{
    if (s_pages == NULL || (int)id < 0 || (int)id >= s_count) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((int)id == s_cur) {
        return ESP_OK;
    }

    /* on_enter/on_exit build and tear down LVGL widgets. When this is called
     * from the touch task (gesture dispatch) we are already inside the port
     * lock; when called from the app task at startup we lock here. The lock is
     * recursive, so nesting is safe. */
    bool locked = lvgl_port_lock(1000);

    if (s_cur >= 0 && s_pages[s_cur]->on_exit) {
        s_pages[s_cur]->on_exit();
    }
    /* Clear any LVGL widgets the previous page built. */
    esp_xiaozhi_chat_display_clear_page_root();

    bool face = (s_pages[id]->id == PAGE_FACE);
    esp_xiaozhi_chat_display_set_face_visible(face);

    s_cur = (int)id;
    if (s_pages[id]->on_enter) {
        s_pages[id]->on_enter();
    }
    indicator_rebuild();
    ESP_LOGI(TAG, "switched to page %d (%s)", s_cur, s_pages[id]->name);

    if (locked) {
        lvgl_port_unlock();
    }
    return ESP_OK;
}

ui_page_id_t ui_page_manager_current(void)
{
    return (s_cur >= 0) ? s_pages[s_cur]->id : PAGE_FACE;
}

void ui_page_manager_tick(uint32_t ms)
{
    s_tick_ms = ms;
    /* on_tick manipulates LVGL objects (face blink, etc.). It runs from an
     * esp_timer callback, NOT the LVGL task, so every LVGL access MUST be
     * under the port lock or the object tree is corrupted -> crash.
     *
     * IMPORTANT: take the lock BEFORE reading s_cur / on_tick. ui_page_manager_
     * switch() also mutates s_cur under this same lock, so reading them only
     * inside the critical section makes the check-and-call atomic w.r.t. page
     * switches. The old code checked s_pages[s_cur]->on_tick BEFORE locking and
     * then re-read s_cur AFTER locking: when a page switch landed in between
     * (e.g. camera -> dialogue, whose on_tick is NULL), the second read returned
     * the new page's NULL on_tick and LVGL jumped through a NULL function
     * pointer -> Guru Meditation (Instruction access fault, MEPC=0) exactly when
     * exiting the camera page. */
    if (!lvgl_port_lock(1000)) {
        return;
    }
    if (s_cur >= 0 && s_pages[s_cur]->on_tick != NULL) {
        s_pages[s_cur]->on_tick(ms);
    }
    lvgl_port_unlock();
}

void ui_page_manager_dispatch_gesture(ui_gesture_t g, int x, int y)
{
    (void)x;
    (void)y;
    /* The pull-down control center intercepts gestures first: while it is open
     * it consumes everything; a swipe-down when closed opens it. */
    if (ui_control_center_on_gesture(g, x, y)) {
        return;
    }
    /* dispatch runs from the touch task, not the LVGL task; on_gesture /
     * ui_page_manager_switch create/move LVGL objects, so lock the port.
     *
     * IMPORTANT: like ui_page_manager_tick(), read s_cur only INSIDE the lock.
     * ui_page_manager_switch() (also called from LVGL event callbacks, e.g. the
     * launcher buttons) mutates s_cur under this same lock, so checking s_cur
     * before locking and re-reading it after is a TOCTOU race that could call a
     * just-exited page's handler. */
    if (!lvgl_port_lock(1000)) {
        return;
    }
    if (s_cur < 0) {
        lvgl_port_unlock();
        return;
    }
    bool consumed = false;
    if (s_pages[s_cur]->on_gesture) {
        consumed = s_pages[s_cur]->on_gesture(g);
    }
    if (consumed) {
        lvgl_port_unlock();
        return;
    }
    if (g == UI_GESTURE_SWIPE_LEFT || g == UI_GESTURE_SWIPE_RIGHT) {
        /* Home screens are FACE + LAUNCHER only. A horizontal swipe ring-navigates
         * between just those two; feature pages are entered ONLY via launcher
         * icons, so a stray horizontal swipe on a feature page does nothing (it
         * cannot crash into an unrequested page). */
        if (s_cur == PAGE_FACE) {
            ui_page_manager_switch(PAGE_LAUNCHER);
        } else if (s_cur == PAGE_LAUNCHER) {
            ui_page_manager_switch(PAGE_FACE);
        }
    } else if (g == UI_GESTURE_SWIPE_DOWN) {
        /* Home gesture: any feature page swiped down returns to the emote
         * face (like Android's home). The launcher page consumes its own
         * swipe-down (see page_launcher_on_gesture), so only feature pages
         * reach this default. */
        if (s_cur != PAGE_FACE) {
            ui_page_manager_switch(PAGE_FACE);
        }
    }
    lvgl_port_unlock();
}

void ui_page_manager_notify_chat(chat_role_t role, const char *text)
{
    if (s_cur < 0) {
        /* Manager not started yet: fall back to the emote toast path. */
        esp_xiaozhi_chat_display_set_chat_message(
            role == CHAT_ROLE_USER ? "user" : (role == CHAT_ROLE_ASSISTANT ? "assistant" : "system"),
            text);
        return;
    }
    if (s_pages[s_cur]->on_chat) {
        s_pages[s_cur]->on_chat(role, text);
    }
}

void ui_page_manager_notify_status(const char *status)
{
    if (s_cur < 0 || status == NULL) {
        return;
    }
    if (s_pages[s_cur]->on_status) {
        s_pages[s_cur]->on_status(status);
    }
}

uint32_t ui_page_manager_now_ms(void)
{
    return s_tick_ms;
}
