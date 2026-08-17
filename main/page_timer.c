/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pomodoro timer page for the ESP-VoCat-S31 "喵伴".
 *
 * Black background, white text. Set hours / minutes / seconds with +/-,
 * pick one of four presets, start / pause / resume, and get an alarm beep
 * when the countdown reaches zero. Uses the page tick (runs under the LVGL
 * lock) for the countdown so it is smooth and safe.
 */
#include "page_timer.h"

#include "app_sfx.h"
#include "app_guard_mode.h"
#include "esp_xiaozhi_chat_display.h"
#include "lvgl.h"

#define BG lv_color_black()
#define FG lv_color_white()

#define PRESETS_COUNT 4
static const int s_presets[PRESETS_COUNT][3] = {
    { 0, 25,  0 },   /* 25:00 classic pomodoro  */
    { 0,  5,  0 },   /* 5:00  short break       */
    { 0, 15,  0 },   /* 15:00 long break        */
    { 1,  0,  0 },   /* 1:00:00 hour            */
};

static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_state_label = NULL;
static lv_obj_t *s_btn = NULL;
static lv_obj_t *s_btn_label = NULL;
static lv_obj_t *s_reset_btn = NULL;
static lv_obj_t *s_preset_btns[PRESETS_COUNT] = {0};

/* 守护模式 (desk-watch while a pomodoro runs). */
static lv_obj_t *s_guard_btn = NULL;
static lv_obj_t *s_guard_btn_label = NULL;

static int      s_hours = 0, s_minutes = 25, s_seconds = 0;
static int32_t  s_remaining_ms = 0;   /* <=0 = not running */
static bool     s_paused = false;
static uint32_t s_last_tick = 0;

static void refresh_time(void)
{
    if (s_time_label == NULL) {
        return;
    }
    char buf[24];
    if (s_remaining_ms <= 0) {
        snprintf(buf, sizeof(buf), "%d:%02d:%02d", s_hours, s_minutes, s_seconds);
    } else {
        int total_sec = (int)((s_remaining_ms + 999) / 1000);
        snprintf(buf, sizeof(buf), "%d:%02d:%02d",
                 total_sec / 3600, (total_sec / 60) % 60, total_sec % 60);
    }
    lv_label_set_text(s_time_label, buf);
    if (s_state_label) {
        lv_label_set_text(s_state_label, s_remaining_ms <= 0 ? "就绪"
                         : (s_paused ? "已暂停" : "进行中"));
    }
}

static void update_btn_text(void)
{
    if (s_btn_label) {
        lv_label_set_text(s_btn_label,
            (s_remaining_ms <= 0) ? "开始" : (s_paused ? "继续" : "暂停"));
    }
}

static void set_running(bool run, bool paused)
{
    s_paused = paused;
    if (run) {
        s_remaining_ms = (int32_t)s_hours * 3600000 +
                         (int32_t)s_minutes * 60000 +
                         (int32_t)s_seconds * 1000;
        if (s_remaining_ms <= 0) {
            s_remaining_ms = 0;
            s_paused = false;
        } else {
            /* CRITICAL: anchor the countdown to NOW. s_last_tick was set when
             * the page was entered; without resetting it here, the first
             * on_tick computes dt = now - (page-enter time) and instantly
             * subtracts the whole elapsed time -> a 3s timer fires the alarm
             * right away. */
            s_last_tick = ui_page_manager_now_ms();
        }
    } else if (!paused) {
        s_remaining_ms = 0;
    }
    update_btn_text();
    refresh_time();
}

static void btn_cb(lv_event_t *e)
{
    (void)e;
    app_sfx_play(APP_SFX_TAP);
    if (s_remaining_ms <= 0) {
        set_running(true, false);
    } else if (s_paused) {
        s_paused = false;
        s_last_tick = ui_page_manager_now_ms();
        update_btn_text();
    } else {
        s_paused = true;
        update_btn_text();
    }
}

static void reset_cb(lv_event_t *e)
{
    (void)e;
    app_sfx_play(APP_SFX_TAP);
    s_remaining_ms = 0;
    s_paused = false;
    update_btn_text();
    refresh_time();
}

static void preset_cb(lv_event_t *e)
{
    app_sfx_play(APP_SFX_TAP);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= PRESETS_COUNT) {
        return;
    }
    if (s_remaining_ms > 0) {
        return;   /* don't change preset mid-run */
    }
    s_hours = s_presets[idx][0];
    s_minutes = s_presets[idx][1];
    s_seconds = s_presets[idx][2];
    refresh_time();
}

/* 守护模式: toggle the camera-based desk-watch. The camera shares the
 * reference-counted pipeline, so it coexists with the camera page / MJPEG /
 * take_photo without conflict. */
static void guard_btn_cb(lv_event_t *e)
{
    (void)e;
    app_sfx_play(APP_SFX_TAP);
    if (app_guard_mode_is_running()) {
        app_guard_mode_stop();
    } else {
        esp_err_t r = app_guard_mode_start();
        if (r != ESP_OK && s_guard_btn_label) {
            lv_obj_set_style_bg_color(s_guard_btn, lv_color_hex(0xE5484D), 0);
            lv_label_set_text(s_guard_btn_label, "摄像头启动失败");
        }
    }
}

static void inc_cb(lv_event_t *e)
{
    app_sfx_play(APP_SFX_TAP);
    int which = (int)(intptr_t)lv_event_get_user_data(e);   /* 0=h 1=m 2=s */
    if (s_remaining_ms > 0) {
        return;
    }
    if (which == 0) {
        if (s_hours < 23) s_hours++;
    } else if (which == 1) {
        if (s_minutes < 59) s_minutes++;
    } else {
        if (s_seconds < 59) s_seconds++;
    }
    refresh_time();
}

static void dec_cb(lv_event_t *e)
{
    app_sfx_play(APP_SFX_TAP);
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_remaining_ms > 0) {
        return;
    }
    if (which == 0) {
        if (s_hours > 0) s_hours--;
    } else if (which == 1) {
        if (s_minutes > 0) s_minutes--;
    } else {
        if (s_seconds > 0) s_seconds--;
    }
    refresh_time();
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb,
                          intptr_t user_data, uint32_t bg)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 64, 32);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3A3A4A), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)user_data);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, FG, 0);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *make_spin(lv_obj_t *parent, const char *label, intptr_t idx)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_set_size(col, 72, 92);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_row(col, 2, 0);

    lv_obj_t *lab = lv_label_create(col);
    lv_label_set_text(lab, label);
    lv_obj_set_style_text_color(lab, lv_color_hex(0xBBBBBB), 0);

    make_btn(col, "+", inc_cb, idx, 0x2ECC71);
    /* (the value is the big time label itself, shown once) */
    make_btn(col, "-", dec_cb, idx, 0xE5484D);
    return col;
}

esp_err_t page_timer_on_enter(void)
{
    lv_obj_t *root = esp_xiaozhi_chat_display_page_root();
    if (root == NULL) {
        return ESP_FAIL;
    }
    lv_obj_set_style_bg_color(root, BG, 0);

    extern lv_obj_t *ui_page_make_back_button(lv_obj_t *);
    ui_page_make_back_button(root);
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "番茄时钟");
    lv_obj_set_style_text_color(title, FG, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    /* Big countdown / set time display */
    s_time_label = lv_label_create(root);
    lv_obj_set_style_text_color(s_time_label, FG, 0);
    lv_obj_set_style_text_font(s_time_label, lv_obj_get_style_text_font(root, 0), 0);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, 32);

    s_state_label = lv_label_create(root);
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(0x888888), 0);
    lv_obj_align(s_state_label, LV_ALIGN_TOP_MID, 0, 56);

    /* Hours / minutes / seconds +/- row */
    lv_obj_t *row = lv_obj_create(root);
    lv_obj_set_size(row, LV_HOR_RES - 20, 96);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    make_spin(row, "时", 0);
    make_spin(row, "分", 1);
    make_spin(row, "秒", 2);

    /* Start / Pause + Reset */
    s_btn = lv_btn_create(root);
    lv_obj_set_size(s_btn, 124, 40);
    lv_obj_align(s_btn, LV_ALIGN_TOP_MID, 0, 178);
    lv_obj_set_style_radius(s_btn, 20, 0);
    lv_obj_set_style_bg_color(s_btn, lv_color_hex(0xE5484D), 0);
    lv_obj_set_style_bg_color(s_btn, lv_color_hex(0xFF6B6E), LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_label = lv_label_create(s_btn);
    lv_label_set_text(s_btn_label, "开始");
    lv_obj_set_style_text_color(s_btn_label, lv_color_white(), 0);
    lv_obj_center(s_btn_label);

    s_reset_btn = lv_btn_create(root);
    lv_obj_set_size(s_reset_btn, 68, 40);
    lv_obj_align(s_reset_btn, LV_ALIGN_TOP_MID, 0, 178);
    lv_obj_set_style_radius(s_reset_btn, 20, 0);
    lv_obj_set_style_bg_color(s_reset_btn, lv_color_hex(0x3A3A4A), 0);
    lv_obj_set_style_translate_x(s_reset_btn, 100, 0);
    lv_obj_add_event_cb(s_reset_btn, reset_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(s_reset_btn);
    lv_label_set_text(rl, "重置");
    lv_obj_set_style_text_color(rl, FG, 0);
    lv_obj_center(rl);

    /* Presets */
    lv_obj_t *pres = lv_obj_create(root);
    lv_obj_set_size(pres, LV_HOR_RES - 20, 38);
    lv_obj_align(pres, LV_ALIGN_TOP_MID, 0, 222);
    lv_obj_set_flex_flow(pres, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pres, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(pres, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pres, 0, 0);
    lv_obj_set_style_pad_all(pres, 0, 0);
    lv_obj_set_style_pad_column(pres, 8, 0);
    static const char *names[PRESETS_COUNT] = { "25分", "5分", "15分", "1小时" };
    for (int i = 0; i < PRESETS_COUNT; i++) {
        lv_obj_t *b = make_btn(pres, names[i], preset_cb, i, 0x4C8CFF);
        lv_obj_set_size(b, 68, 32);
        s_preset_btns[i] = b;
    }

    /* 守护模式: watch the child through the camera while the timer runs.
     * The button label doubles as the status indicator (on_tick updates it). */
    s_guard_btn = lv_btn_create(root);
    lv_obj_set_size(s_guard_btn, 190, 32);
    lv_obj_align(s_guard_btn, LV_ALIGN_TOP_MID, 0, 266);
    lv_obj_set_style_radius(s_guard_btn, 16, 0);
    lv_obj_set_style_bg_color(s_guard_btn, lv_color_hex(0x4C8CFF), 0);
    lv_obj_set_style_bg_color(s_guard_btn, lv_color_hex(0x3A6FD0), LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_guard_btn, guard_btn_cb, LV_EVENT_CLICKED, NULL);
    s_guard_btn_label = lv_label_create(s_guard_btn);
    lv_label_set_text(s_guard_btn_label, "🛡 守护模式");
    lv_obj_set_style_text_color(s_guard_btn_label, FG, 0);
    lv_obj_center(s_guard_btn_label);

    s_last_tick = ui_page_manager_now_ms();
    set_running(false, false);
    app_sfx_play(APP_SFX_PAGE);
    return ESP_OK;
}

esp_err_t page_timer_on_exit(void)
{
    /* Leave guard mode when the page is left: the camera must not keep running
     * hidden in the background (it would hold the DVP + PSRAM buffers). */
    app_guard_mode_stop();
    return ESP_OK;
}

void page_timer_on_tick(uint32_t ms)
{
    /* 守护模式 live status (independent of the countdown). */
    static uint32_t s_last_guard_update = 0;
    if (app_guard_mode_is_running() && s_guard_btn_label &&
        ms - s_last_guard_update >= 500) {
        s_last_guard_update = ms;
        guard_info_t g = app_guard_mode_get_info();
        if (g.status == GUARD_STATUS_ABSENT) {
            lv_obj_set_style_bg_color(s_guard_btn, lv_color_hex(0xE5484D), 0);
            lv_label_set_text(s_guard_btn_label, "⚠ 无人·离开书桌");
            /* Alert the parent while the pomodoro is still running. */
            if (s_remaining_ms > 0) {
                app_sfx_play(APP_SFX_ALARM);
            }
        } else if (g.status == GUARD_STATUS_ACTIVE) {
            lv_obj_set_style_bg_color(s_guard_btn, lv_color_hex(0x2ECC71), 0);
            lv_label_set_text(s_guard_btn_label, "🛡 守护中·活跃");
        } else {
            lv_obj_set_style_bg_color(s_guard_btn, lv_color_hex(0xF5B041), 0);
            lv_label_set_text(s_guard_btn_label, "🛡 守护中·安静");
        }
    } else if (!app_guard_mode_is_running() && s_guard_btn_label &&
               ms - s_last_guard_update >= 500) {
        s_last_guard_update = ms;
        lv_obj_set_style_bg_color(s_guard_btn, lv_color_hex(0x4C8CFF), 0);
        lv_label_set_text(s_guard_btn_label, "🛡 守护模式");
    }

    if (s_remaining_ms <= 0 || s_paused) {
        return;
    }
    int32_t dt = (int32_t)(ms - s_last_tick);
    s_last_tick = ms;
    if (dt <= 0) {
        return;
    }
    /* Clamp dt: if a tick was delayed (UI lock contention, page switch) we do
     * not want to jump the countdown by a huge amount. 1s max per tick keeps
     * the display honest. */
    if (dt > 1000) {
        dt = 1000;
    }
    s_remaining_ms -= dt;
    if (s_remaining_ms <= 0) {
        s_remaining_ms = 0;
        refresh_time();
        app_sfx_play(APP_SFX_ALARM);
        update_btn_text();
        if (s_state_label) {
            lv_label_set_text(s_state_label, "时间到！");
        }
        return;
    }
    refresh_time();
}

const ui_page_t page_timer = {
    .id = PAGE_TIMER,
    .name = "timer",
    .on_enter = page_timer_on_enter,
    .on_exit = page_timer_on_exit,
    .on_tick = page_timer_on_tick,
};
