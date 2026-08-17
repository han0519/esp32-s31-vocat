/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Qixi (七夕) easter egg implementation. See easter_egg.h.
 *
 * Triggered by tapping the face 5 times quickly: a big art-style "七夕快乐"
 * banner (30px Chinese font) with floating red hearts. The hearts are drawn as
 * a bitmap heart (LVGL image) — the on-board fonts don't contain a ♥ glyph, so
 * a little pixel heart is rendered into an RGB565 buffer at init and reused by
 * N lv_image widgets that float up the screen.
 */
#include "easter_egg.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "esp_xiaozhi_chat_display.h"
#include "ui_page_manager.h"

#define TAG "EASTER_EGG"

#include "qixi_banner.h"
#include "heart_red.h"

/* Artistic calligraphy "七夕快乐" banner (pre-rendered RGB565 from STXINGKA). */
static lv_image_dsc_t s_banner_dsc = {0};

/* Solid red heart bitmap (RGB565) — the text glyph '♥' is missing from the
 * on-board font and renders as a tofu box, so we use a real bitmap heart. */
static lv_image_dsc_t s_heart_dsc = {0};

#define EGG_HEARTS       12
#define EGG_DURATION_MS  7000
#define HEART_SZ         HEART_RED_W   /* heart sprite size (px) */

static bool s_inited = false;
static bool s_active = false;
static bool s_was_face = false;   /* face mode was active when the egg fired */
static lv_obj_t *s_layer = NULL;
static lv_obj_t *s_hearts[EGG_HEARTS] = {0};
static lv_obj_t *s_banner = NULL;
static esp_timer_handle_t s_timer = NULL;
static int s_tap_count = 0;
static int64_t s_last_tap_ms = 0;

/* --- floating red hearts (solid bitmap, no GIF sprite) ------------------ */

/* Run on the LVGL task: hide the hearts, then hand the panel back to the
 * emote face if the egg hijacked it (see easter_egg_show). */
static void egg_do_hide(void *arg)
{
    (void)arg;
    if (s_layer) {
        lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
    }
    s_active = false;
    /* Restore the face ONLY if the user is still on the face page and we
     * actually switched it away (a page switch during the egg already
     * restored PAGE mode). */
    if (s_was_face && ui_page_manager_current() == PAGE_FACE) {
        esp_xiaozhi_chat_display_set_face_visible(true);
    }
    s_was_face = false;
}

static void hide_timer_cb(void *arg)
{
    (void)arg;
    lv_async_call(egg_do_hide, NULL);
}

/* Heart float animation ready callback: reset to bottom and re-launch a NEW
 * animation object (each is independent; never reuses a stack lv_anim_t after
 * it has been consumed by the animation system). */
static void heart_ready_cb(lv_anim_t *a)
{
    lv_obj_t *h = (lv_obj_t *)a->var;
    if (h == NULL) {
        return;
    }
    lv_obj_set_y(h, LV_VER_RES + 24);
    lv_obj_set_x(h, rand() % (LV_HOR_RES - HEART_SZ));
    lv_anim_t na;
    lv_anim_init(&na);
    lv_anim_set_var(&na, h);
    lv_anim_set_exec_cb(&na, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&na, LV_VER_RES + 24, -HEART_SZ - 8);
    lv_anim_set_time(&na, 2800 + (rand() % 2200));
    lv_anim_set_path_cb(&na, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&na, heart_ready_cb);
    lv_anim_start(&na);
}

static void build(void)
{
    if (s_layer != NULL) {
        return;
    }
    lv_obj_t *top = lv_layer_top();
    s_layer = lv_obj_create(top);
    lv_obj_set_size(s_layer, LV_HOR_RES, LV_VER_RES);
    /* Opaque dark backdrop so the heart bitmaps' black corners are invisible
     * (RGB565 images have no alpha; a transparent layer would show black boxes). */
    lv_obj_set_style_bg_color(s_layer, lv_color_hex(0x07070C), 0);
    lv_obj_set_style_bg_opa(s_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_layer, 0, 0);
    lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);

    /* Big artistic-calligraphy banner (pre-rendered RGB565 ~330x78 from
     * STXINGKA 华文行楷 — several times larger than the old 20px text and far
     * cheaper than a full-size CJK font that would blow the app partition). */
    s_banner_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_banner_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_banner_dsc.header.w = QIXI_BANNER_W;
    s_banner_dsc.header.h = QIXI_BANNER_H;
    s_banner_dsc.data_size = QIXI_BANNER_W * QIXI_BANNER_H * 2;
    s_banner_dsc.data = qixi_banner_data;

    s_banner = lv_image_create(s_layer);
    lv_image_set_src(s_banner, &s_banner_dsc);
    lv_obj_center(s_banner);

    /* Floating red hearts — solid bitmap, no GIF sprite, no text glyph. */
    s_heart_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_heart_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_heart_dsc.header.w = HEART_RED_W;
    s_heart_dsc.header.h = HEART_RED_H;
    s_heart_dsc.data_size = HEART_RED_W * HEART_RED_H * 2;
    s_heart_dsc.data = heart_red_data;

    for (int i = 0; i < EGG_HEARTS; i++) {
        lv_obj_t *h = lv_image_create(s_layer);
        lv_image_set_src(h, &s_heart_dsc);
        lv_obj_set_size(h, HEART_SZ, HEART_SZ);
        lv_obj_clear_flag(h, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(h, rand() % (LV_HOR_RES - HEART_SZ), LV_VER_RES + 24);
        s_hearts[i] = h;
    }
}

static void start_hearts(void)
{
    for (int i = 0; i < EGG_HEARTS; i++) {
        lv_obj_t *h = s_hearts[i];
        if (h == NULL) {
            continue;
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, h);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
        lv_anim_set_values(&a, LV_VER_RES + 24, -HEART_SZ - 8);
        lv_anim_set_time(&a, 2800 + (rand() % 2200));
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_ready_cb(&a, heart_ready_cb);
        lv_anim_set_delay(&a, i * 120);
        lv_anim_start(&a);
    }
}

/* Run on the LVGL task: build/show the hearts overlay. Called via lv_async_call
 * so all LVGL object work happens on the LVGL render task (safe from the gesture
 * dispatch task that detected the combo). */
static void egg_do_show(void *arg)
{
    (void)arg;
    if (!s_inited || s_active) {
        return;
    }
    /* CRITICAL: while the emote face owns the panel (FACE mode) LVGL flushing is
     * STOPPED (lvgl_port_stop), so objects on lv_layer_top() are never rendered
     * — the hearts were invisible before. Switch to PAGE mode first so the
     * LVGL renderer draws the overlay; the hide timer hands the panel back. */
    s_was_face = (ui_page_manager_current() == PAGE_FACE);
    if (s_was_face) {
        esp_xiaozhi_chat_display_set_face_visible(false);
    }
    build();
    lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(s_banner);
    start_hearts();
    s_active = true;

    if (s_timer == NULL) {
        esp_timer_create_args_t t = {
            .callback = hide_timer_cb,
            .name = "easter_egg",
        };
        esp_timer_create(&t, &s_timer);
    }
    esp_timer_stop(s_timer);
    esp_timer_start_once(s_timer, EGG_DURATION_MS * 1000ULL);
}

esp_err_t easter_egg_init(void)
{
    s_inited = true;
    ESP_LOGI(TAG, "easter egg armed (tap function-desktop x5)");
    return ESP_OK;
}

void easter_egg_show(void)
{
    if (!s_inited || s_active) {
        return;
    }
    lv_async_call(egg_do_show, NULL);
}

bool easter_egg_tap(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    if (now - s_last_tap_ms > 1500) {
        s_tap_count = 0;   /* combo expired */
    }
    s_last_tap_ms = now;
    s_tap_count++;
    ESP_LOGI(TAG, "easter egg tap %d/5", s_tap_count);
    if (s_tap_count >= 5) {
        s_tap_count = 0;
        ESP_LOGI(TAG, "easter egg FIRE");
        easter_egg_show();
        return true;
    }
    return false;
}
