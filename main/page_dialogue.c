/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dialogue / transcript page for the ESP-VoCat-S31 "喵伴" pet.
 *
 * Shows the recent conversation (user + assistant + system) as chat bubbles
 * with timestamps and a live status line. Auto-scrolls to the newest message.
 * The full history is persisted in flash by app_chat_history; this page is a
 * live, scrollable view of it.
 */
#include "page_dialogue.h"

#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "app_chat_history.h"
#include "esp_xiaozhi_chat_display.h"

LV_FONT_DECLARE(font_puhui_20_4);

/* Layout constants (360px wide round panel). */
#define BUBBLE_MAX_W   (LV_HOR_RES - 28)
#define BUBBLE_RADIUS  14
#define BUBBLE_PAD     8

static lv_obj_t *s_scroll = NULL;
static lv_obj_t *s_status = NULL;
static bool s_ready = false;

static lv_color_t bubble_color(chat_role_t role)
{
    switch (role) {
    case CHAT_ROLE_USER:      return lv_color_hex(0x1B5E20);   /* dark green */
    case CHAT_ROLE_SYSTEM:    return lv_color_hex(0x333333);   /* gray       */
    default:                  return lv_color_hex(0x2A2A2A);   /* assistant  */
    }
}

static lv_color_t bubble_text_color(chat_role_t role)
{
    (void)role;
    return lv_color_white();
}

static void fmt_ts(uint32_t ts, char *out, size_t n)
{
    if (ts > 1000000000) {
        time_t t = (time_t)ts;
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
    } else {
        snprintf(out, n, "--:--");
    }
}

static void add_bubble(chat_role_t role, const char *text, uint32_t ts)
{
    if (!s_ready || s_scroll == NULL) {
        return;
    }
    const char *who = (role == CHAT_ROLE_USER) ? "你"
                     : (role == CHAT_ROLE_ASSISTANT) ? "喵伴" : "系统";
    char tsbuf[16];
    fmt_ts(ts, tsbuf, sizeof(tsbuf));

    /* Message bubble container (left = user, right = assistant/system). */
    lv_obj_t *wrap = lv_obj_create(s_scroll);
    lv_obj_set_width(wrap, LV_HOR_RES - 16);
    lv_obj_set_height(wrap, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(wrap, 0, 0);
    lv_obj_set_style_border_width(wrap, 0, 0);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_START,
                          (role == CHAT_ROLE_USER) ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *bubble = lv_obj_create(wrap);
    lv_obj_set_width(bubble, LV_SIZE_CONTENT);
    lv_obj_set_width(bubble, LV_PCT(78));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, BUBBLE_RADIUS, 0);
    lv_obj_set_style_bg_color(bubble, bubble_color(role), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, BUBBLE_PAD, 0);
    lv_obj_set_style_pad_row(bubble, 2, 0);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *meta = lv_label_create(bubble);
    char meta_buf[64];
    snprintf(meta_buf, sizeof(meta_buf), "%s · %s", who, tsbuf);
    lv_label_set_text(meta, meta_buf);
    lv_obj_set_style_text_color(meta, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(meta, &font_puhui_20_4, 0);

    lv_obj_t *body = lv_label_create(bubble);
    lv_label_set_text(body, text ? text : "");
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_style_text_color(body, bubble_text_color(role), 0);
    lv_obj_set_style_text_font(body, &font_puhui_20_4, 0);

    /* Auto-scroll to the newest message. */
    lv_obj_scroll_to_view(wrap, LV_ANIM_ON);
}

static void apply_status_color(const char *status)
{
    if (s_status == NULL || status == NULL) {
        return;
    }
    lv_color_t c = lv_color_hex(0x777777);
    if (strstr(status, "Error") || strstr(status, "错误") || strstr(status, "Recorder")) {
        c = lv_color_hex(0xE5484D);
    } else if (strstr(status, "Speaking") || strstr(status, "Listening")) {
        c = lv_color_hex(0x3D8BFD);
    }
    lv_obj_set_style_text_color(s_status, c, 0);
}

esp_err_t page_dialogue_on_enter(void)
{
    lv_obj_t *root = esp_xiaozhi_chat_display_page_root();
    if (root == NULL) {
        return ESP_FAIL;
    }
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);

    /* Home / back button (top-left) + title. */
    extern lv_obj_t *ui_page_make_back_button(lv_obj_t *);
    ui_page_make_back_button(root);
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "对话记录");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &font_puhui_20_4, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_status = lv_label_create(root);
    lv_label_set_text(s_status, "就绪");
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xBBBBBB), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 32);

    s_scroll = lv_obj_create(root);
    lv_obj_set_size(s_scroll, LV_HOR_RES - 8, LV_VER_RES - 66);
    lv_obj_align(s_scroll, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_flex_flow(s_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_scroll, 0, 0);
    lv_obj_set_style_pad_all(s_scroll, 3, 0);
    lv_obj_set_style_pad_row(s_scroll, 4, 0);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_AUTO);

    s_ready = true;

    /* Replay stored history (oldest -> newest). */
    size_t n = app_chat_history_count();
    for (size_t i = 0; i < n; ++i) {
        chat_turn_t t;
        if (app_chat_history_get(i, &t) == ESP_OK) {
            add_bubble(t.role, t.text, t.timestamp);
        }
    }
    return ESP_OK;
}

esp_err_t page_dialogue_on_exit(void)
{
    s_ready = false;
    return ESP_OK;
}

void page_dialogue_on_chat(chat_role_t role, const char *text)
{
    add_bubble(role, text, (uint32_t)time(NULL));
}

void page_dialogue_on_status(const char *status)
{
    if (s_status) {
        lv_label_set_text(s_status, status ? status : "");
        apply_status_color(status);
    }
}

const ui_page_t page_dialogue = {
    .id = PAGE_DIALOGUE,
    .name = "dialogue",
    .on_enter = page_dialogue_on_enter,
    .on_exit = page_dialogue_on_exit,
    .on_gesture = NULL,    /* default swipe navigation */
    .on_tick = NULL,
    .on_chat = page_dialogue_on_chat,
    .on_status = page_dialogue_on_status,
};
