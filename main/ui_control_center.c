/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pull-down control center for the ESP-VoCat-S31 "喵伴" pet.
 *
 * A frosted-white (semi-transparent, rounded) panel that slides down from the
 * top when the user swipes down. It hosts a "重置 WiFi" button plus volume and
 * brightness sliders. Swiping up / tapping the empty area closes it. The panel
 * lives on lv_layer_top() so it overlays the animated face and every page.
 */
#include "ui_control_center.h"

#include <string.h>
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "wifi_provisioning.h"
#include "esp_xiaozhi_chat_app.h"
#include "esp_xiaozhi_chat_display.h"
#include "ui_page_manager.h"
#include "app_sfx.h"
#include "lvgl.h"

static const char *TAG = "CTRL_CENTER";

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

/* The 360x360 round screen has an inscribed circle of radius 180. A wide panel
 * pushed to the top (y≈12) puts its corners OUTSIDE the visible circle (at
 * y=12 the circle is only ~129px wide), clipping the collapse button. Make the
 * panel narrower and center it vertically so every row sits inside the circle:
 *   - PANEL_W 232  -> at y≥87 the circle is ≥300px wide, plenty of margin
 *   - PANEL_H 186  -> content (28+30+30+34 + pads/gaps = 180) fits; centered at
 *                     y=(360-186)/2=87, bottom 273 (circle ~310px wide)
 */
#define PANEL_W            232
#define PANEL_H            186
#define PANEL_OPEN_Y       ((LV_VER_RES - PANEL_H) / 2)   /* vertically centered */
#define PANEL_RADIUS       24
#define PANEL_BG_COLOR     lv_color_hex(0x1A1A1A)
#define PANEL_BG_OPA       LV_OPA_80   /* frosted look: translucent dark   */
#define PANEL_TEXT_COLOR   lv_color_white()
#define PANEL_ACCENT       lv_color_hex(0x3D8BFD)
#define PANEL_DANGER       lv_color_hex(0xE5484D)
#define PANEL_BORDER       lv_color_hex(0x444444)
#define PANEL_SLIDER_TRACK lv_color_hex(0x3A3A3A)
#define PANEL_SLIDER_KNOB  lv_color_white()

static lv_obj_t *s_panel = NULL;
static lv_obj_t *s_vol_slider = NULL;
static lv_obj_t *s_bri_slider = NULL;
static lv_obj_t *s_wifi_btn = NULL;
static lv_obj_t *s_collapse_btn = NULL;
static lv_obj_t *s_vol_label = NULL;
static lv_obj_t *s_bri_label = NULL;
static bool s_open = false;
static bool s_inited = false;

static int s_wi_fi_confirm = 0;   /* 0 = normal, 1 = "再点一次确认" */

/* --- event handlers ------------------------------------------------------ */

static void vol_slider_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target_obj(e);
    int v = (int)lv_slider_get_value(sl);
    /* Snap the knob to the value that was actually applied (set_volume can
     * clamp / fail), so the UI never lies about the real state. */
    esp_err_t r = esp_xiaozhi_chat_app_set_volume(v);
    if (r == ESP_OK && s_vol_label) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", esp_xiaozhi_chat_app_get_volume());
        lv_label_set_text(s_vol_label, buf);
    }
}

static void bri_slider_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target_obj(e);
    int v = (int)lv_slider_get_value(sl);
    /* set_brightness clamps to a visible floor (screen would be unrecoverable
     * at 0). Snap the knob to the value actually applied so dragging to 0
     * visibly dims to the floor instead of "doing nothing". */
    esp_err_t r = esp_xiaozhi_chat_app_set_brightness(v);
    if (r == ESP_OK) {
        int applied = esp_xiaozhi_chat_app_get_brightness();
        lv_slider_set_value(sl, applied, LV_ANIM_OFF);
        if (s_bri_label) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", applied);
            lv_label_set_text(s_bri_label, buf);
        }
    }
}

static void wifi_btn_cb(lv_event_t *e)
{
    (void)e;
    app_sfx_play(APP_SFX_TAP);
    if (s_wi_fi_confirm == 0) {
        /* First tap: arm the confirmation. */
        s_wi_fi_confirm = 1;
        if (s_wifi_btn) {
            lv_obj_set_style_bg_color(s_wifi_btn, PANEL_DANGER, 0);
            lv_label_set_text(lv_obj_get_child(s_wifi_btn, 0), "再点一次确认重置");
        }
    } else {
        /* Second tap: erase creds + reboot into provisioning. Never returns. */
        if (s_wifi_btn) {
            lv_obj_set_style_bg_color(s_wifi_btn, PANEL_DANGER, 0);
            lv_label_set_text(lv_obj_get_child(s_wifi_btn, 0), "正在重置 WiFi...");
        }
        wifi_creds_erase_and_reboot();
    }
}

/* Tap on the panel background (not on a control) closes the panel. LVGL bubbles
 * the CLICKED event from any child (slider/button) up to the panel, so we must
 * ignore clicks whose target is a child — otherwise tapping a slider would also
 * close the panel and the slider could never be adjusted ("改不回去"). */
static void panel_bg_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_panel) {
        return;
    }
    ui_control_center_close();
}

/* Explicit, unambiguous close affordance (the swipe-up path is sometimes hard
 * to trigger while dragging a slider). */
static void collapse_btn_cb(lv_event_t *e)
{
    (void)e;
    app_sfx_play(APP_SFX_TAP);
    ui_control_center_close();
}

/* --- build --------------------------------------------------------------- */

static void build_panel(void)
{
    lv_obj_t *top = lv_layer_top();

    s_panel = lv_obj_create(top);
    lv_obj_set_size(s_panel, PANEL_W, PANEL_H);
    lv_obj_set_pos(s_panel, (LV_HOR_RES - PANEL_W) / 2, -PANEL_H);  /* off-screen */
    lv_obj_set_style_radius(s_panel, PANEL_RADIUS, 0);
    lv_obj_set_style_bg_color(s_panel, PANEL_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(s_panel, PANEL_BG_OPA, 0);
    lv_obj_set_style_border_width(s_panel, 1, 0);
    lv_obj_set_style_border_color(s_panel, PANEL_BORDER, 0);
    lv_obj_set_style_border_opa(s_panel, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(s_panel, 14, 0);
    lv_obj_set_style_pad_row(s_panel, 10, 0);
    lv_obj_set_style_clip_corner(s_panel, true, 0);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* Tap on the panel itself (not consumed by a child) closes it. The panel
     * must stay CLICKABLE so the background tap handler fires; the touch
     * gesture layer is an independent poller, so this does NOT affect swipes. */
    lv_obj_add_event_cb(s_panel, panel_bg_cb, LV_EVENT_CLICKED, NULL);

    /* --- Header row: title (grows) + collapse button --- */
    lv_obj_t *hdr = lv_obj_create(s_panel);
    lv_obj_set_size(hdr, PANEL_W - 28, 28);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS " 控制中心");
    lv_obj_set_style_text_font(title, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(title, PANEL_TEXT_COLOR, 0);
    lv_obj_set_flex_grow(title, 1);

    s_collapse_btn = lv_btn_create(hdr);
    lv_obj_set_size(s_collapse_btn, 52, 24);
    lv_obj_set_style_radius(s_collapse_btn, 12, 0);
    lv_obj_set_style_bg_color(s_collapse_btn, PANEL_BORDER, 0);
    lv_obj_set_style_bg_opa(s_collapse_btn, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(s_collapse_btn, collapse_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(s_collapse_btn);
    lv_label_set_text(cl, "收起");
    lv_obj_set_style_text_font(cl, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(cl, PANEL_TEXT_COLOR, 0);
    lv_obj_center(cl);

    /* --- Volume row --- */
    lv_obj_t *vol_row = lv_obj_create(s_panel);
    lv_obj_set_size(vol_row, PANEL_W - 28, 30);
    lv_obj_set_flex_flow(vol_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(vol_row, 0, 0);
    lv_obj_set_style_border_width(vol_row, 0, 0);
    lv_obj_set_style_bg_opa(vol_row, LV_OPA_TRANSP, 0);

    lv_obj_t *vol_icon = lv_label_create(vol_row);
    lv_label_set_text(vol_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(vol_icon, &font_awesome_20_4, 0);
    lv_obj_set_style_text_color(vol_icon, PANEL_TEXT_COLOR, 0);
    lv_obj_set_width(vol_icon, 26);

    s_vol_slider = lv_slider_create(vol_row);
    lv_obj_set_width(s_vol_slider, 150);
    lv_obj_set_flex_grow(s_vol_slider, 1);
    lv_obj_set_style_bg_color(s_vol_slider, PANEL_SLIDER_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_vol_slider, PANEL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol_slider, PANEL_SLIDER_KNOB, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_vol_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_slider_set_range(s_vol_slider, 0, 100);
    lv_slider_set_value(s_vol_slider, esp_xiaozhi_chat_app_get_volume(), LV_ANIM_OFF);
    lv_obj_add_event_cb(s_vol_slider, vol_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_vol_label = lv_label_create(vol_row);
    lv_obj_set_width(s_vol_label, 30);
    lv_obj_set_style_text_align(s_vol_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_vol_label, PANEL_TEXT_COLOR, 0);
    char vb[8];
    snprintf(vb, sizeof(vb), "%d", esp_xiaozhi_chat_app_get_volume());
    lv_label_set_text(s_vol_label, vb);

    /* --- Brightness row --- */
    lv_obj_t *bri_row = lv_obj_create(s_panel);
    lv_obj_set_size(bri_row, PANEL_W - 28, 30);
    lv_obj_set_flex_flow(bri_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bri_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(bri_row, 0, 0);
    lv_obj_set_style_border_width(bri_row, 0, 0);
    lv_obj_set_style_bg_opa(bri_row, LV_OPA_TRANSP, 0);

    lv_obj_t *bri_icon = lv_label_create(bri_row);
    lv_label_set_text(bri_icon, LV_SYMBOL_UP);
    lv_obj_set_style_text_font(bri_icon, &font_awesome_20_4, 0);
    lv_obj_set_style_text_color(bri_icon, PANEL_TEXT_COLOR, 0);
    lv_obj_set_width(bri_icon, 26);

    s_bri_slider = lv_slider_create(bri_row);
    lv_obj_set_width(s_bri_slider, 150);
    lv_obj_set_flex_grow(s_bri_slider, 1);
    lv_obj_set_style_bg_color(s_bri_slider, PANEL_SLIDER_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bri_slider, PANEL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bri_slider, PANEL_SLIDER_KNOB, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_bri_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_slider_set_range(s_bri_slider, 0, 100);
    lv_slider_set_value(s_bri_slider, esp_xiaozhi_chat_app_get_brightness(), LV_ANIM_OFF);
    lv_obj_add_event_cb(s_bri_slider, bri_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_bri_label = lv_label_create(bri_row);
    lv_obj_set_width(s_bri_label, 30);
    lv_obj_set_style_text_align(s_bri_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_bri_label, PANEL_TEXT_COLOR, 0);
    char bb[8];
    snprintf(bb, sizeof(bb), "%d", esp_xiaozhi_chat_app_get_brightness());
    lv_label_set_text(s_bri_label, bb);

    /* --- WiFi reset button --- */
    s_wifi_btn = lv_btn_create(s_panel);
    lv_obj_set_width(s_wifi_btn, PANEL_W - 28);
    lv_obj_set_height(s_wifi_btn, 34);
    lv_obj_set_style_radius(s_wifi_btn, 17, 0);
    lv_obj_set_style_bg_color(s_wifi_btn, PANEL_DANGER, 0);
    lv_obj_set_style_bg_opa(s_wifi_btn, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(s_wifi_btn, wifi_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *wl = lv_label_create(s_wifi_btn);
    lv_label_set_text(wl, LV_SYMBOL_WIFI " 重置 WiFi");
    lv_obj_set_style_text_font(wl, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(wl, lv_color_white(), 0);
    lv_obj_center(wl);
}

/* --- public API ---------------------------------------------------------- */

esp_err_t ui_control_center_init(void)
{
    /* Deferred (lazy) construction: the LVGL objects are only created on the
     * first open() because init() may run right after the emote face took over
     * the panel (lvgl_port_stop()), where lvgl_port_lock() can fail. open() is
     * always called from the LVGL task context with the face switched back to
     * PAGE mode, so building there is safe. */
    s_inited = true;
    ESP_LOGI(TAG, "control center armed (lazy build)");
    return ESP_OK;
}

static void ensure_built(void)
{
    if (s_panel != NULL) {
        return;
    }
    if (!lvgl_port_lock(1000)) {
        return;
    }
    build_panel();
    lvgl_port_unlock();
    ESP_LOGI(TAG, "control center panel built");
}

/* Remember whether the face (official emote) was active before the control
 * center opened, so close() can hand the panel back to it. */
static bool s_saved_face = false;
static bool s_pending_open = false;

/* All LVGL object work happens on the LVGL task via lv_async_call. Building /
 * positioning a layer-top panel from any other task raced with the LVGL render
 * task and crashed in lv_event_send / lv_obj_destructor (Load access fault on
 * first open only, then fine — classic cross-task LVGL corruption). */
static void cc_do_open(void *user_data)
{
    (void)user_data;
    ensure_built();
    if (s_panel == NULL) {
        if (s_saved_face && ui_page_manager_current() == PAGE_FACE) {
            esp_xiaozhi_chat_display_set_face_visible(true);
        }
        s_saved_face = false;
        s_open = false;
        return;
    }
    s_wi_fi_confirm = 0;
    if (s_wifi_btn) {
        lv_obj_set_style_bg_color(s_wifi_btn, PANEL_DANGER, 0);
        lv_label_set_text(lv_obj_get_child(s_wifi_btn, 0), LV_SYMBOL_WIFI " 重置 WiFi");
    }
    /* Refresh slider positions from the current values. */
    lv_slider_set_value(s_vol_slider, esp_xiaozhi_chat_app_get_volume(), LV_ANIM_OFF);
    lv_slider_set_value(s_bri_slider, esp_xiaozhi_chat_app_get_brightness(), LV_ANIM_OFF);
    /* Cancel any in-flight close animation before repositioning. */
    lv_anim_del(s_panel, (lv_anim_exec_xcb_t)lv_obj_set_y);
    /* cc_do_close hid the panel; un-hide it so it is rendered again. */
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(s_panel, PANEL_OPEN_Y);   /* vertically centered on round screen */
    s_open = true;
    app_sfx_play(APP_SFX_PAGE);
}

/* Slide the panel up off-screen, then hide it. Runs on the LVGL task. */
static void cc_close_anim_ready_cb(lv_anim_t *a)
{
    lv_obj_t *panel = (lv_obj_t *)a->var;
    if (panel == NULL) {
        return;
    }
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    /* Hand the panel back to the official emote face if it was active before. */
    if (s_saved_face && ui_page_manager_current() == PAGE_FACE) {
        esp_xiaozhi_chat_display_set_face_visible(true);
    }
    s_saved_face = false;
}

static void cc_do_close(void *user_data)
{
    (void)user_data;
    if (s_panel) {
        /* Animate the slide-up so closing feels smooth instead of instant. */
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_panel);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
        lv_anim_set_values(&a, lv_obj_get_y(s_panel), -PANEL_H - 8);
        lv_anim_set_time(&a, 160);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
        lv_anim_set_ready_cb(&a, cc_close_anim_ready_cb);
        lv_anim_start(&a);
    } else if (s_saved_face && ui_page_manager_current() == PAGE_FACE) {
        esp_xiaozhi_chat_display_set_face_visible(true);
        s_saved_face = false;
    }
}

void ui_control_center_open(void)
{
    if (!s_inited || s_open || s_pending_open) {
        return;
    }
    s_pending_open = true;
    s_saved_face = (ui_page_manager_current() == PAGE_FACE);
    /* The panel is an LVGL layer-top overlay. When the official emote face owns
     * the panel (FACE mode) LVGL flushing is stopped, so we must switch back to
     * LVGL (PAGE mode) before the overlay can be drawn. */
    esp_xiaozhi_chat_display_set_face_visible(false);
    /* All LVGL object creation / positioning on the LVGL task. */
    lv_async_call(cc_do_open, NULL);
}

void ui_control_center_close(void)
{
    if (!s_inited || !s_open) {
        return;
    }
    /* Clear the flag FIRST so gestures stop being consumed even if the LVGL
     * work below fails (this was the cause of "swipes stop working" and the
     * frozen face). */
    s_open = false;
    lv_async_call(cc_do_close, NULL);
}

bool ui_control_center_is_open(void)
{
    return s_open;
}

bool ui_control_center_on_gesture(ui_gesture_t g, int x, int y)
{
    if (!s_inited) {
        return false;
    }
    if (s_open) {
        /* While open, the panel's own LVGL pointer input handles the sliders
         * and buttons. The gesture layer must NOT close on a vertical swipe
         * that starts INSIDE the panel (y >= panel top + a little slack):
         * dragging a slider vertically is classified as SWIPE_UP/DOWN, and
         * closing mid-drag made the sliders unusable ("改不回去").
         *
         * Closing happens via:
         *   - the "收起" button,
         *   - a tap on the panel background (panel CLICKED handler ignores
         *     child taps), or
         *   - a swipe that BEGAN in the top strip ABOVE the panel (y < 60) —
         *     an intentional dismiss that cannot be a slider drag.
         */
        if ((g == UI_GESTURE_SWIPE_UP || g == UI_GESTURE_SWIPE_DOWN) && y < 60) {
            ui_control_center_close();
        }
        return true;   /* consume everything else while open (don't navigate) */
    }
    /* Only pull the control center down while the face page is active.
     * On every other page a swipe-down must stay unhandled so the page
     * manager can navigate normally. */
    if (g == UI_GESTURE_SWIPE_DOWN && ui_page_manager_current() == PAGE_FACE) {
        ui_control_center_open();
        return true;
    }
    return false;
}
