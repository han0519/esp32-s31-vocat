/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * App-grid launcher (手机桌面) for the ESP-VoCat-S31 "喵伴".
 *
 * Shows a grid of function modules (对话记录 / 摄像头 / 音乐 / 游戏 / 计时器 /
 * 控制中心). Tapping a module jumps to its page. The face page (emote) is the
 * "home screen"; a swipe-up on the face opens this launcher, and a swipe-down
 * here (or the 返回 button) returns to the face.
 */
#include "page_launcher.h"

#include <string.h>
#include "esp_log.h"
#include "app_sfx.h"
#include "esp_xiaozhi_chat_display.h"
#include "lvgl.h"
#include "font_awesome_symbols.h"
#include "easter_egg.h"

#define TAG "PAGE_LAUNCHER"

#define LAUNCHER_COLS 3
#define LAUNCHER_ROWS 2
#define LAUNCHER_N    (LAUNCHER_COLS * LAUNCHER_ROWS)

typedef struct {
    ui_page_id_t page;
    const char  *icon;   /* Font Awesome icon */
    const char  *label;
} launcher_item_t;

static const launcher_item_t s_items[LAUNCHER_N] = {
    { PAGE_DIALOGUE, FONT_AWESOME_COMMENT,      "对话"   },
    { PAGE_CAMERA,   FONT_AWESOME_IMAGE,        "摄像头" },
    { PAGE_MUSIC,    FONT_AWESOME_MUSIC,        "音乐"   },
    { PAGE_GAME,     FONT_AWESOME_PLAY,         "游戏"   },
    { PAGE_TIMER,    FONT_AWESOME_BELL,         "计时器" },
    { PAGE_FACE,     FONT_AWESOME_HOME,         "返回"   },
};

static void item_cb(lv_event_t *e)
{
    app_sfx_play(APP_SFX_TAP);
    ui_page_id_t id = (ui_page_id_t)(intptr_t)lv_event_get_user_data(e);
    extern esp_err_t ui_page_manager_switch(ui_page_id_t);
    ui_page_manager_switch(id);
}

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    app_sfx_play(APP_SFX_TAP);
    extern esp_err_t ui_page_manager_switch(ui_page_id_t);
    ui_page_manager_switch(PAGE_FACE);
}

lv_obj_t *ui_page_make_back_button(lv_obj_t *root)
{
    if (root == NULL) {
        return NULL;
    }
    lv_obj_t *btn = lv_btn_create(root);
    lv_obj_set_size(btn, 48, 30);
    lv_obj_set_pos(btn, 4, 4);
    lv_obj_set_style_radius(btn, 15, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2A34), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A3A4A), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, FONT_AWESOME_ARROW_UP);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    return btn;
}

esp_err_t page_launcher_on_enter(void)
{
    lv_obj_t *root = esp_xiaozhi_chat_display_page_root();
    if (root == NULL) {
        return ESP_FAIL;
    }
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);

    /* Title bar */
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "功能桌面");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    /* App grid */
    lv_obj_t *grid = lv_obj_create(root);
    lv_obj_set_size(grid, LV_HOR_RES - 16, 214);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_row(grid, 10, 0);
    lv_obj_set_style_pad_column(grid, 8, 0);

    int cell_w = (LV_HOR_RES - 16 - 24) / LAUNCHER_COLS;
    for (int i = 0; i < LAUNCHER_N; i++) {
        lv_obj_t *cell = lv_obj_create(grid);
        lv_obj_set_size(cell, cell_w, 76);
        lv_obj_set_style_radius(cell, 16, 0);
        lv_obj_set_style_bg_color(cell, lv_color_hex(0x1E1E24), 0);
        lv_obj_set_style_bg_color(cell, lv_color_hex(0x2A2A34), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 6, 0);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(cell, item_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)s_items[i].page);

        lv_obj_t *icon = lv_label_create(cell);
        lv_label_set_text(icon, s_items[i].icon);
        lv_obj_set_style_text_color(icon, lv_color_hex(0x3D8BFD), 0);
        lv_obj_set_style_text_font(icon, LV_FONT_DEFAULT, 0);

        lv_obj_t *lab = lv_label_create(cell);
        lv_label_set_text(lab, s_items[i].label);
        lv_obj_set_style_text_color(lab, lv_color_white(), 0);
    }

    app_sfx_play(APP_SFX_PAGE);
    return ESP_OK;
}

esp_err_t page_launcher_on_exit(void)
{
    return ESP_OK;
}

void page_launcher_on_tick(uint32_t ms)
{
    (void)ms;
}

bool page_launcher_on_gesture(ui_gesture_t g)
{
    /* Easter egg: tap the desktop 5 times quickly to wake the Qixi banner. */
    if (g == UI_GESTURE_TAP) {
        extern bool easter_egg_tap(void);
        easter_egg_tap();
        return false;   /* don't consume — a tap on an icon still opens its app */
    }
    /* Swipe down returns to the emote face (home). */
    if (g == UI_GESTURE_SWIPE_DOWN) {
        app_sfx_play(APP_SFX_PAGE);
        extern esp_err_t ui_page_manager_switch(ui_page_id_t);
        ui_page_manager_switch(PAGE_FACE);
        return true;
    }
    return false;
}

const ui_page_t page_launcher = {
    .id = PAGE_LAUNCHER,
    .name = "launcher",
    .on_enter = page_launcher_on_enter,
    .on_exit = page_launcher_on_exit,
    .on_tick = page_launcher_on_tick,
    .on_gesture = page_launcher_on_gesture,
};
