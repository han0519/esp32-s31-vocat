/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_xiaozhi_chat_display.h"

#include "ui_touch.h"
#include "esp_board_manager.h"
#include "esp_board_manager_adapter.h"
#include "dev_display_lcd.h"
#include "font_awesome_symbols.h"
#include "font_emoji.h"
#include "expression_emote.h"
#include "ui_page_manager.h"

#include <esp_err.h>
#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_pm.h>
#include <esp_lcd_panel_io.h>
#include <esp_timer.h>
#include <lvgl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* LVGL pointer input read callback (defined below) mirrors ui_touch's state. */
void lvgl_port_touch_mirror_read(lv_indev_t *indev, lv_indev_data_t *data);

#define LANG_STR_INITIALIZING           "正在初始化..."
#define LANG_STR_BATTERY_NEED_CHARGE    "电量低，请充电"

#define DARK_BACKGROUND_COLOR           lv_color_hex(0x121212)
#define DARK_TEXT_COLOR                 lv_color_white()
#define DARK_CHAT_BACKGROUND_COLOR      lv_color_hex(0x1E1E1E)
#define DARK_USER_BUBBLE_COLOR          lv_color_hex(0x1A6C37)
#define DARK_ASSISTANT_BUBBLE_COLOR     lv_color_hex(0x333333)
#define DARK_SYSTEM_BUBBLE_COLOR        lv_color_hex(0x2A2A2A)
#define DARK_SYSTEM_TEXT_COLOR          lv_color_hex(0xAAAAAA)
#define DARK_BORDER_COLOR               lv_color_hex(0x333333)
#define DARK_LOW_BATTERY_COLOR          lv_color_hex(0xFF0000)

#define LIGHT_BACKGROUND_COLOR          lv_color_white()
#define LIGHT_TEXT_COLOR                lv_color_black()
#define LIGHT_CHAT_BACKGROUND_COLOR     lv_color_hex(0xE0E0E0)
#define LIGHT_USER_BUBBLE_COLOR         lv_color_hex(0x95EC69)
#define LIGHT_ASSISTANT_BUBBLE_COLOR    lv_color_white()
#define LIGHT_SYSTEM_BUBBLE_COLOR       lv_color_hex(0xE0E0E0)
#define LIGHT_SYSTEM_TEXT_COLOR         lv_color_hex(0x666666)
#define LIGHT_BORDER_COLOR              lv_color_hex(0xE0E0E0)
#define LIGHT_LOW_BATTERY_COLOR         lv_color_black()

LV_FONT_DECLARE(font_awesome_30_4);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

typedef struct {
    const lv_font_t *text_font;
    const lv_font_t *icon_font;
    const lv_font_t *emoji_font;
} display_fonts_t;

typedef struct {
    lv_color_t background;
    lv_color_t text;
    lv_color_t chat_background;
    lv_color_t user_bubble;
    lv_color_t assistant_bubble;
    lv_color_t system_bubble;
    lv_color_t system_text;
    lv_color_t border;
    lv_color_t low_battery;
} theme_colors_t;

typedef enum {
    DISPLAY_KIND_NONE,
    DISPLAY_KIND_LCD,
} display_kind_t;

typedef struct display {
    display_kind_t kind;
    int width;
    int height;
    esp_pm_lock_handle_t pm_lock;
    lv_display_t *lv_disp;
    lv_obj_t *emotion_label;
    lv_obj_t *network_label;
    lv_obj_t *status_label;
    lv_obj_t *notification_label;
    lv_obj_t *mute_label;
    lv_obj_t *battery_label;
    lv_obj_t *chat_message_label;
    lv_obj_t *low_battery_popup;
    lv_obj_t *low_battery_label;
    const char *battery_icon;
    const char *network_icon;
    bool muted;
    char current_theme_name[32];
    esp_timer_handle_t notification_timer;
} display_t;

typedef struct {
    display_t base;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;
    lv_obj_t *status_bar;
    lv_obj_t *content;
    lv_obj_t *container;
    lv_obj_t *side_bar;
    lv_obj_t *preview_image;
    display_fonts_t fonts;
    theme_colors_t current_theme;
} lcd_display_t;

static const char *TAG = "ESP_XIAOZHI_CHAT_DISPLAY";

static bool s_lvgl_inited;
static bool s_lvgl_port_inited;
static display_t *s_display = NULL;

static const struct {
    const char *text;
    const char *fa_icon;
    const char *unicode_icon;
} s_emotion_table[] = {
    {"neutral", FONT_AWESOME_EMOJI_NEUTRAL, "😶"},
    {"happy", FONT_AWESOME_EMOJI_HAPPY, "🙂"},
    {"laughing", FONT_AWESOME_EMOJI_LAUGHING, "😆"},
    {"funny", FONT_AWESOME_EMOJI_FUNNY, "😂"},
    {"sad", FONT_AWESOME_EMOJI_SAD, "😔"},
    {"angry", FONT_AWESOME_EMOJI_ANGRY, "😠"},
    {"crying", FONT_AWESOME_EMOJI_CRYING, "😭"},
    {"loving", FONT_AWESOME_EMOJI_LOVING, "😍"},
    {"embarrassed", FONT_AWESOME_EMOJI_EMBARRASSED, "😳"},
    {"surprised", FONT_AWESOME_EMOJI_SURPRISED, "😯"},
    {"shocked", FONT_AWESOME_EMOJI_SHOCKED, "😱"},
    {"thinking", FONT_AWESOME_EMOJI_THINKING, "🤔"},
    {"winking", FONT_AWESOME_EMOJI_WINKING, "😉"},
    {"cool", FONT_AWESOME_EMOJI_COOL, "😎"},
    {"relaxed", FONT_AWESOME_EMOJI_RELAXED, "😌"},
    {"delicious", FONT_AWESOME_EMOJI_DELICIOUS, "🤤"},
    {"kissy", FONT_AWESOME_EMOJI_KISSY, "😘"},
    {"confident", FONT_AWESOME_EMOJI_CONFIDENT, "😏"},
    {"sleepy", FONT_AWESOME_EMOJI_SLEEPY, "😴"},
    {"silly", FONT_AWESOME_EMOJI_SILLY, "😜"},
    {"confused", FONT_AWESOME_EMOJI_CONFUSED, "🙄"},
};
#define EMOTION_COUNT (sizeof(s_emotion_table) / sizeof(s_emotion_table[0]))

/* ===================== EMOTE (animated expression) backend =====================
 * When enabled, the animated emote face takes over the LCD panel after
 * provisioning completes (see esp_xiaozhi_chat_display_enable_emote). The LVGL
 * display is deleted so only emote drives the panel. If emote init/mount fails,
 * the LVGL display keeps working as a graceful fallback. */
static emote_handle_t s_emote = NULL;
static bool s_emote_active = false;
/* True while the animated face is the active panel renderer. When false the
 * LVGL page UI owns the panel (see esp_xiaozhi_chat_display_set_face_visible). */
static bool s_face_visible = false;
/* LVGL container reused by every PAGE-mode page (dialogue / camera / ...). */
static lv_obj_t *s_page_root = NULL;

typedef struct {
    const char *emotion;
    const char *anim;
} emote_map_t;

/* Maps the app's emotion names to the .eaf animation names that actually exist
 * in the flashed emote asset pack (emote-assets1.bin -> emote_assets_mmap.bin).
 * Only names with a backing .eaf file are valid here; the pack also lists
 * "smile_static" / "snigger_10s" / "yummy_20_s" entries whose .eaf files are
 * absent, so referencing them makes emote_set_anim_emoji() fail to load. */
static const emote_map_t s_emote_map[] = {
    {"neutral",     "smile_05s"},
    {"happy",       "smile_05s"},
    {"laughing",    "laugh_05s_10"},
    {"funny",       "laugh_05s_10"},
    {"sad",         "sad_05s15s"},
    {"angry",       "angry_20s"},
    {"crying",      "cry_10s_20s"},
    {"loving",      "shy_20s_40s"},
    {"embarrassed", "shy_20s_40s"},
    {"surprised",   "shocked_05s_"},
    {"shocked",     "shocked_05s_"},
    {"thinking",    "ponder_05s"},
    {"winking",     "question_05s"},
    {"cool",        "confident_08"},
    {"relaxed",     "leisure_05s_"},
    {"delicious",   "leisure_05s_"},
    {"kissy",       "shy_20s_40s"},
    {"confident",   "confident_08"},
    {"sleepy",      "asleep_215s"},
    {"silly",       "mock_05s"},
    {"confused",    "question_05s"},
};
#define EMOTE_MAP_COUNT (sizeof(s_emote_map) / sizeof(s_emote_map[0]))

static const char *emote_anim_for(const char *emotion)
{
    if (emotion == NULL) {
        return "smile_05s";
    }
    for (size_t i = 0; i < EMOTE_MAP_COUNT; ++i) {
        if (strcmp(s_emote_map[i].emotion, emotion) == 0) {
            return s_emote_map[i].anim;
        }
    }
    return "smile_05s";
}

/* Forward declarations: LVGL-port lock/unlock helpers (defined further below)
 * are also used by the page-manager-facing helpers above. */
static bool esp_xiaozhi_chat_display_lock(display_t *disp, int timeout_ms);
static void esp_xiaozhi_chat_display_unlock(display_t *disp);

static void emote_set_status_event(const char *status)
{
    if (s_emote == NULL || status == NULL) {
        return;
    }
    if (strncmp(status, "Listening", 8) == 0) {
        emote_set_event_msg(s_emote, EMOTE_MGR_EVT_LISTEN, NULL);
    } else if (strncmp(status, "Speaking", 8) == 0) {
        emote_set_event_msg(s_emote, EMOTE_MGR_EVT_SPEAK, NULL);
    } else if (strncmp(status, "Ready", 5) == 0 ||
               strncmp(status, "Connected", 9) == 0 ||
               strncmp(status, "Disconnected", 12) == 0) {
        emote_set_event_msg(s_emote, EMOTE_MGR_EVT_IDLE, NULL);
    } else {
        emote_set_event_msg(s_emote, EMOTE_MGR_EVT_SET, NULL);
    }
}

static void emote_on_flush_ready(int x_start, int y_start, int x_end, int y_end,
                                 const void *data, emote_handle_t handle)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)emote_get_user_data(handle);
    if (panel != NULL && s_face_visible) {
        esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, data);
    } else {
        /* Face hidden: LVGL owns the panel, so we must not draw over it. Notify
         * completion ourselves so emote's internal flush wait never deadlocks. */
        emote_notify_flush_finished(handle);
    }
}

static bool emote_on_flush_done(const esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    /* Unified color-transfer-done dispatcher. BOTH renderers share the same
     * LCD panel IO, and only one callback slot exists. Route the completion
     * event to whichever renderer actually issued the transfer:
     *  - FACE mode (emote drives the panel): notify emote's flush wait.
     *  - PAGE mode (LVGL drives the panel): call lv_disp_flush_ready() so LVGL
     *    never blocks forever on a flush (previously emote overwrote the LVGL
     *    callback and switching back to a page only drew the first block, then
     *    everything froze). */
    if (s_face_visible) {
        emote_handle_t handle = (emote_handle_t)user_ctx;
        if (handle) {
            emote_notify_flush_finished(handle);
        }
    } else if (s_display != NULL && s_display->lv_disp != NULL) {
        lv_disp_flush_ready(s_display->lv_disp);
    }
    return true;
}

esp_err_t esp_xiaozhi_chat_display_enable_emote(void)
{
    if (s_emote_active) {
        return ESP_OK;
    }
    if (s_display == NULL) {
        ESP_LOGW(TAG, "emote: display not initialized yet");
        return ESP_ERR_INVALID_STATE;
    }

    void *lcd_handle = NULL;
    esp_err_t ret = esp_board_manager_get_device_handle("display_lcd", &lcd_handle);
    if (ret != ESP_OK || lcd_handle == NULL) {
        ESP_LOGW(TAG, "emote: no display_lcd handle (%s)", esp_err_to_name(ret));
        return ret;
    }
    dev_display_lcd_handles_t *lcd_handles = (dev_display_lcd_handles_t *)lcd_handle;
    if (lcd_handles->io_handle == NULL || lcd_handles->panel_handle == NULL) {
        ESP_LOGW(TAG, "emote: LCD io/panel handles NULL");
        return ESP_ERR_INVALID_STATE;
    }

    const int w = s_display->width;
    const int h = s_display->height;

    emote_config_t cfg = {
        .flags = {
            .swap = true,
            .double_buffer = true,
            .buff_dma = false,
            .buff_spiram = true,
        },
        .gfx_emote = {
            .h_res = w,
            .v_res = h,
            .fps = 25,
        },
        .buffers = {
            .buf_pixels = (size_t)(w * 16),
        },
        .task = {
            .task_priority = 5,
            .task_stack = 6 * 1024,
            .task_affinity = 0,
            .task_stack_in_ext = true,   /* keep the 6KB stack out of internal SRAM */
        },
        .flush_cb = emote_on_flush_ready,
        .user_data = (void *)lcd_handles->panel_handle,
    };

    s_emote = emote_init(&cfg);
    if (s_emote == NULL) {
        ESP_LOGE(TAG, "emote_init failed; keeping LVGL display");
        return ESP_FAIL;
    }

    emote_data_t src = {
        .type = EMOTE_SOURCE_PARTITION,
        .source = { .partition_label = "assets" },
        .flags = { .mmap_enable = 1 },
    };
    ret = emote_mount_and_load_assets(s_emote, &src);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "emote mount+load failed (%s); keeping LVGL display", esp_err_to_name(ret));
        emote_deinit(s_emote);
        s_emote = NULL;
        return ret;
    }

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = emote_on_flush_done,
    };
    esp_lcd_panel_io_register_event_callbacks(lcd_handles->io_handle, &cbs, s_emote);

    s_emote_active = true;
    /* The animated face now owns the panel. Switch the renderer into FACE mode
     * (stops LVGL flushing; keeps its objects alive for PAGE-mode pages). */
    esp_xiaozhi_chat_display_set_face_visible(true);

    ESP_LOGI(TAG, "Emote display active (%dx%d)", w, h);
    emote_set_anim_emoji(s_emote, "smile_05s");
    emote_set_status_event("Ready");
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_set_face_visible(bool visible)
{
    if (s_display == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lcd_display_t *lcd = (lcd_display_t *)s_display;

    /* LVGL object ops are not thread-safe: take the port lock. The flush
     * start/stop (lvgl_port_stop/resume) is performed OUTSIDE the lock to avoid
     * re-entering the same mutex. */
    if (visible) {
        /* FACE mode: emote drives the panel, LVGL must not flush over it. */
        if (esp_xiaozhi_chat_display_lock(s_display, 1000)) {
            if (s_page_root != NULL) {
                lv_obj_add_flag(s_page_root, LV_OBJ_FLAG_HIDDEN);
            }
            if (lcd->container != NULL) {
                lv_obj_add_flag(lcd->container, LV_OBJ_FLAG_HIDDEN);
            }
            if (lcd->status_bar != NULL) {
                lv_obj_add_flag(lcd->status_bar, LV_OBJ_FLAG_HIDDEN);
            }
            esp_xiaozhi_chat_display_unlock(s_display);
        }
        s_face_visible = true;
        lvgl_port_stop();
    } else {
        /* PAGE mode: LVGL owns the panel; emote keeps running but its flush is
         * a no-op (see emote_on_flush_ready). */
        if (esp_xiaozhi_chat_display_lock(s_display, 1000)) {
            if (lcd->container != NULL) {
                lv_obj_add_flag(lcd->container, LV_OBJ_FLAG_HIDDEN);
            }
            if (lcd->status_bar != NULL) {
                lv_obj_add_flag(lcd->status_bar, LV_OBJ_FLAG_HIDDEN);
            }
            if (s_page_root != NULL) {
                lv_obj_clear_flag(s_page_root, LV_OBJ_FLAG_HIDDEN);
            }
            esp_xiaozhi_chat_display_unlock(s_display);
        }
        s_face_visible = false;
        lvgl_port_resume();
    }
    return ESP_OK;
}

lv_obj_t *esp_xiaozhi_chat_display_page_root(void)
{
    return s_page_root;
}

esp_err_t esp_xiaozhi_chat_display_clear_page_root(void)
{
    if (s_page_root != NULL && esp_xiaozhi_chat_display_lock(s_display, 1000)) {
        lv_obj_clean(s_page_root);
        esp_xiaozhi_chat_display_unlock(s_display);
    }
    return ESP_OK;
}

static theme_colors_t esp_xiaozhi_chat_display_dark_theme(void)
{
    theme_colors_t t = {0};
    t.background = DARK_BACKGROUND_COLOR;
    t.text = DARK_TEXT_COLOR;
    t.chat_background = DARK_CHAT_BACKGROUND_COLOR;
    t.user_bubble = DARK_USER_BUBBLE_COLOR;
    t.assistant_bubble = DARK_ASSISTANT_BUBBLE_COLOR;
    t.system_bubble = DARK_SYSTEM_BUBBLE_COLOR;
    t.system_text = DARK_SYSTEM_TEXT_COLOR;
    t.border = DARK_BORDER_COLOR;
    t.low_battery = DARK_LOW_BATTERY_COLOR;
    return t;
}

static theme_colors_t esp_xiaozhi_chat_display_light_theme(void)
{
    theme_colors_t t = {0};
    t.background = LIGHT_BACKGROUND_COLOR;
    t.text = LIGHT_TEXT_COLOR;
    t.chat_background = LIGHT_CHAT_BACKGROUND_COLOR;
    t.user_bubble = LIGHT_USER_BUBBLE_COLOR;
    t.assistant_bubble = LIGHT_ASSISTANT_BUBBLE_COLOR;
    t.system_bubble = LIGHT_SYSTEM_BUBBLE_COLOR;
    t.system_text = LIGHT_SYSTEM_TEXT_COLOR;
    t.border = LIGHT_BORDER_COLOR;
    t.low_battery = LIGHT_LOW_BATTERY_COLOR;
    return t;
}

static inline lcd_display_t *esp_xiaozhi_chat_display_from_base(display_t *disp)
{
    return (lcd_display_t *)disp;
}

static bool esp_xiaozhi_chat_display_lock(display_t *disp, int timeout_ms)
{
    if (disp == NULL) {
        return false;
    }
    if (disp->kind == DISPLAY_KIND_LCD) {
        return lvgl_port_lock(timeout_ms);
    }
    (void)timeout_ms;
    return true;
}

static void esp_xiaozhi_chat_display_unlock(display_t *disp)
{
    if (disp == NULL) {
        return;
    }
    if (disp->kind == DISPLAY_KIND_LCD) {
        lvgl_port_unlock();
    }
}

static void esp_xiaozhi_chat_display_notification(void *arg)
{
    display_t *disp = (display_t *)arg;
    if (disp == NULL || !esp_xiaozhi_chat_display_lock(disp, 30000)) {
        return;
    }
    if (disp->notification_label != NULL) {
        lv_obj_add_flag(disp->notification_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (disp->status_label != NULL) {
        lv_obj_clear_flag(disp->status_label, LV_OBJ_FLAG_HIDDEN);
    }
    esp_xiaozhi_chat_display_unlock(disp);
}

static esp_err_t esp_xiaozhi_chat_display_state_init(display_t *disp)
{
    ESP_RETURN_ON_FALSE(disp, ESP_ERR_INVALID_STATE, TAG, "Display not initialized");

    disp->battery_icon = NULL;
    disp->network_icon = NULL;
    disp->muted = false;
    strlcpy(disp->current_theme_name, "light", sizeof(disp->current_theme_name));

    esp_timer_create_args_t timer_args = {
        .callback = esp_xiaozhi_chat_display_notification,
        .arg = disp,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "notification_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &disp->notification_timer));

    esp_err_t ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "display_update", &disp->pm_lock);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "Power management not supported");
    } else {
        ESP_ERROR_CHECK(ret);
    }

    return ESP_OK;
}

static esp_err_t esp_xiaozhi_chat_display_setup_ui(lcd_display_t *lcd)
{
    display_t *disp = &lcd->base;
    if (!esp_xiaozhi_chat_display_lock(disp, 30000)) {
        return ESP_ERR_INVALID_STATE;
    }
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, lcd->fonts.text_font, 0);
    lv_obj_set_style_text_color(screen, lcd->current_theme.text, 0);
    lv_obj_set_style_bg_color(screen, lcd->current_theme.background, 0);

    lcd->container = lv_obj_create(screen);
    lv_obj_set_size(lcd->container, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(lcd->container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(lcd->container, 0, 0);
    lv_obj_set_style_border_width(lcd->container, 0, 0);
    lv_obj_set_style_pad_row(lcd->container, 0, 0);
    lv_obj_set_style_bg_color(lcd->container, lcd->current_theme.background, 0);
    lv_obj_set_style_border_color(lcd->container, lcd->current_theme.border, 0);

    lcd->status_bar = lv_obj_create(lcd->container);
    lv_obj_set_size(lcd->status_bar, LV_HOR_RES, lcd->fonts.text_font->line_height);
    lv_obj_set_style_radius(lcd->status_bar, 0, 0);
    lv_obj_set_style_bg_color(lcd->status_bar, lcd->current_theme.background, 0);
    lv_obj_set_style_text_color(lcd->status_bar, lcd->current_theme.text, 0);

    lcd->content = lv_obj_create(lcd->container);
    lv_obj_set_scrollbar_mode(lcd->content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(lcd->content, 0, 0);
    lv_obj_set_width(lcd->content, LV_HOR_RES);
    lv_obj_set_flex_grow(lcd->content, 1);
    lv_obj_set_style_pad_all(lcd->content, 5, 0);
    lv_obj_set_style_bg_color(lcd->content, lcd->current_theme.chat_background, 0);
    lv_obj_set_style_border_color(lcd->content, lcd->current_theme.border, 0);

    lv_obj_set_flex_flow(lcd->content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(lcd->content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_EVENLY);
    disp->emotion_label = lv_label_create(lcd->content);
    lv_obj_set_style_text_font(disp->emotion_label, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(disp->emotion_label, lcd->current_theme.text, 0);
    lv_label_set_text(disp->emotion_label, FONT_AWESOME_AI_CHIP);
    lcd->preview_image = lv_image_create(lcd->content);
    lv_obj_set_size(lcd->preview_image, disp->width * 0.5, disp->height * 0.5);
    lv_obj_align(lcd->preview_image, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(lcd->preview_image, LV_OBJ_FLAG_HIDDEN);
    disp->chat_message_label = lv_label_create(lcd->content);
    lv_label_set_text(disp->chat_message_label, "");
    lv_obj_set_width(disp->chat_message_label, LV_HOR_RES * 0.9);
    lv_label_set_long_mode(disp->chat_message_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(disp->chat_message_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(disp->chat_message_label, lcd->current_theme.text, 0);

    lv_obj_set_flex_flow(lcd->status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(lcd->status_bar, 0, 0);
    lv_obj_set_style_border_width(lcd->status_bar, 0, 0);
    lv_obj_set_style_pad_column(lcd->status_bar, 0, 0);
    lv_obj_set_style_pad_left(lcd->status_bar, 2, 0);
    lv_obj_set_style_pad_right(lcd->status_bar, 2, 0);

    disp->network_label = lv_label_create(lcd->status_bar);
    lv_label_set_text(disp->network_label, "");
    lv_obj_set_style_text_font(disp->network_label, lcd->fonts.icon_font, 0);
    lv_obj_set_style_text_color(disp->network_label, lcd->current_theme.text, 0);
    disp->notification_label = lv_label_create(lcd->status_bar);
    lv_obj_set_flex_grow(disp->notification_label, 1);
    lv_obj_set_style_text_align(disp->notification_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(disp->notification_label, lcd->current_theme.text, 0);
    lv_label_set_text(disp->notification_label, "");
    lv_obj_add_flag(disp->notification_label, LV_OBJ_FLAG_HIDDEN);
    disp->status_label = lv_label_create(lcd->status_bar);
    lv_obj_set_flex_grow(disp->status_label, 1);
    lv_label_set_long_mode(disp->status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(disp->status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(disp->status_label, lcd->current_theme.text, 0);
    lv_label_set_text(disp->status_label, LANG_STR_INITIALIZING);
    disp->mute_label = lv_label_create(lcd->status_bar);
    lv_label_set_text(disp->mute_label, "");
    lv_obj_set_style_text_font(disp->mute_label, lcd->fonts.icon_font, 0);
    lv_obj_set_style_text_color(disp->mute_label, lcd->current_theme.text, 0);
    disp->battery_label = lv_label_create(lcd->status_bar);
    lv_label_set_text(disp->battery_label, "");
    lv_obj_set_style_text_font(disp->battery_label, lcd->fonts.icon_font, 0);
    lv_obj_set_style_text_color(disp->battery_label, lcd->current_theme.text, 0);
    disp->low_battery_popup = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(disp->low_battery_popup, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(disp->low_battery_popup, LV_HOR_RES * 0.9, lcd->fonts.text_font->line_height * 2);
    lv_obj_align(disp->low_battery_popup, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(disp->low_battery_popup, lcd->current_theme.low_battery, 0);
    lv_obj_set_style_radius(disp->low_battery_popup, 10, 0);
    disp->low_battery_label = lv_label_create(disp->low_battery_popup);
    lv_label_set_text(disp->low_battery_label, LANG_STR_BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(disp->low_battery_label, lv_color_white(), 0);
    lv_obj_center(disp->low_battery_label);
    lv_obj_add_flag(disp->low_battery_popup, LV_OBJ_FLAG_HIDDEN);

    /* Reusable container for PAGE-mode pages (dialogue / camera / ...). Hidden
     * until a page switches the panel away from the animated face. */
    if (s_page_root == NULL) {
        s_page_root = lv_obj_create(lv_screen_active());
        lv_obj_set_size(s_page_root, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_flex_flow(s_page_root, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_page_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(s_page_root, lv_color_black(), 0);
        lv_obj_set_style_border_width(s_page_root, 0, 0);
        lv_obj_set_style_pad_all(s_page_root, 4, 0);
        lv_obj_add_flag(s_page_root, LV_OBJ_FLAG_HIDDEN);
    }
    esp_xiaozhi_chat_display_unlock(disp);

    return ESP_OK;
}

static esp_err_t esp_xiaozhi_chat_display_destroy(display_t *disp)
{
    ESP_RETURN_ON_FALSE(disp, ESP_ERR_INVALID_STATE, TAG, "Display not initialized");

    if (disp->notification_timer != NULL) {
        esp_timer_stop(disp->notification_timer);
        esp_timer_delete(disp->notification_timer);
        disp->notification_timer = NULL;
    }
    if (disp->kind == DISPLAY_KIND_LCD) {
        lcd_display_t *lcd = esp_xiaozhi_chat_display_from_base(disp);
        if (lcd->content != NULL) {
            lv_obj_del(lcd->content);
            lcd->content = NULL;
        }
        if (lcd->status_bar != NULL) {
            lv_obj_del(lcd->status_bar);
            lcd->status_bar = NULL;
        }
        if (lcd->side_bar != NULL) {
            lv_obj_del(lcd->side_bar);
            lcd->side_bar = NULL;
        }
        if (lcd->container != NULL) {
            lv_obj_del(lcd->container);
            lcd->container = NULL;
        }
        if (disp->lv_disp != NULL) {
            lv_display_delete(disp->lv_disp);
            disp->lv_disp = NULL;
        }
        if (lcd->panel != NULL) {
            esp_lcd_panel_del(lcd->panel);
            lcd->panel = NULL;
        }
        if (lcd->panel_io != NULL) {
            esp_lcd_panel_io_del(lcd->panel_io);
            lcd->panel_io = NULL;
        }
        free(esp_xiaozhi_chat_display_from_base(disp));
    } else {
        if (disp->network_label != NULL) {
            lv_obj_del(disp->network_label);
            lv_obj_del(disp->notification_label);
            lv_obj_del(disp->status_label);
            lv_obj_del(disp->mute_label);
            lv_obj_del(disp->battery_label);
            lv_obj_del(disp->emotion_label);
            disp->network_label = NULL;
        }
        if (disp->low_battery_popup != NULL) {
            lv_obj_del(disp->low_battery_popup);
            disp->low_battery_popup = NULL;
        }
        if (disp->pm_lock != NULL) {
            esp_pm_lock_delete(disp->pm_lock);
            disp->pm_lock = NULL;
        }
        free(disp);
    }

    return ESP_OK;
}

/* LVGL flush_wait callback.
 *
 * WHY THIS EXISTS (crash root cause, confirmed via on-device serial dump):
 * The emote engine overwrites the shared SPI panel_io `on_color_trans_done`
 * callback with `emote_on_flush_done`, which routes flush completion to either
 * emote (FACE mode) or LVGL (PAGE mode) depending on the global `s_face_visible`
 * flag. When the control center closes, `cc_do_close` first moves the panel
 * (queueing an async SPI flush) and then sets `s_face_visible = true`. If the
 * flush completes AFTER the face switch, `emote_on_flush_done` sees
 * `s_face_visible == true` and notifies emote instead of calling
 * `lv_disp_flush_ready()` -> `disp->flushing` never clears -> LVGL spins in
 * `wait_for_flushing()`'s `while(disp->flushing)` (lv_refr.c:1458), hogging the
 * core and starving the IDLE task until `task_wdt` reboots the board.
 *
 * Setting `flush_wait_cb` makes `wait_for_flushing()` take the branch that
 * BLOCKS (yielding the CPU) and then FORCES `disp->flushing = 0`, so even if
 * the flush-ready event is lost to the race we never spin on the core and never
 * trigger the WDT. This is a hard guarantee against the IDLE starvation reboot.
 */
static void lvgl_port_flush_wait_cb(lv_display_t *disp)
{
    (void)disp;
    /* Yield the core long enough for the flush-done ISR to run. After this
     * returns, wait_for_flushing() sets disp->flushing = 0 unconditionally, so
     * this callback never spins regardless of whether the ISR arrived. */
    vTaskDelay(1);
}

static display_t *esp_xiaozhi_chat_display_create(esp_lcd_panel_io_handle_t panel_io,
                                                  esp_lcd_panel_handle_t panel,
                                                  int width, int height,
                                                  int offset_x, int offset_y,
                                                  bool mirror_x, bool mirror_y, bool swap_xy,
                                                  display_fonts_t fonts)
{
    lcd_display_t *lcd = (lcd_display_t *)calloc(1, sizeof(lcd_display_t));
    if (lcd == NULL) {
        ESP_LOGE(TAG, "Failed to allocate lcd display");
        return NULL;
    }
    lcd->base.width = width;
    lcd->base.height = height;
    lcd->base.kind = DISPLAY_KIND_LCD;
    lcd->panel_io = panel_io;
    lcd->panel = panel;
    lcd->fonts = fonts;
    lcd->current_theme = esp_xiaozhi_chat_display_light_theme();
    esp_xiaozhi_chat_display_state_init(&lcd->base);

    uint16_t *buffer = (uint16_t *)heap_caps_malloc((size_t)width * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (buffer != NULL) {
        for (int i = 0; i < width; ++i) {
            buffer[i] = 0xFFFF;
        }
        for (int y = 0; y < height; ++y) {
            esp_lcd_panel_draw_bitmap(panel, 0, y, width, y + 1, buffer);
        }
        heap_caps_free(buffer);
    }
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    if (!s_lvgl_inited) {
        lv_init();
        s_lvgl_inited = true;
    }
    if (!s_lvgl_port_inited) {
        lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
        port_cfg.task_priority = 3;         /* keep UI responsive (emote=5) */
        port_cfg.timer_period_ms = 20;      /* 50Hz tick -> smooth animations */
        /* Pin LVGL to CPU1: the emote face task runs on CPU0 at prio 5 and the
         * WiFi task on CPU0 at prio 23. With task_affinity=-1 FreeRTOS could
         * place LVGL on CPU0 too, where the boot burst (WiFi connect + emote
         * animation + camera + audio) starved BOTH LVGL and IDLE0 past the WDT
         * timeout -> task_wdt reboot right after WiFi connected. Splitting the
         * renderer onto CPU1 keeps the UI responsive during boot. */
        port_cfg.task_affinity = 1;
        ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));
        s_lvgl_port_inited = true;
    }

    /* Double-buffered, PSRAM-backed LVGL. buffer_size = height*40px per buffer
     * (~28 KB each). Double buffering lets LVGL draw into the back buffer while
     * the SPI DMA flushes the front one -> no tearing, higher effective frame
     * rate on the 360x360 panel.
     * buff_spiram = 1: the ST77916 QSPI driver needs a ~14 KB "private TX
     * buffer" per draw_bitmap; when the LVGL buffer also lived in internal DMA
     * SRAM the two competed and the SPI bounce buffer allocation failed
     * ("Failed to allocate priv TX buffer"), freezing the screen. Moving the
     * LVGL buffer to PSRAM frees internal DMA SRAM for the SPI TX buffer. */
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = NULL,
        .buffer_size = (uint32_t)(width * 40),
        .double_buffer = true,
        .trans_size = 0,
        .hres = (uint32_t)width,
        .vres = (uint32_t)height,
        .monochrome = false,
        .rotation = { .swap_xy = swap_xy, .mirror_x = mirror_x, .mirror_y = mirror_y },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_dma = 1, .buff_spiram = 1, .sw_rotate = 0, .swap_bytes = 1, .full_refresh = 0, .direct_mode = 0 },
    };
    lcd->base.lv_disp = lvgl_port_add_disp(&display_cfg);
    if (lcd->base.lv_disp == NULL) {
        ESP_LOGE(TAG, "Failed to add display");
        esp_xiaozhi_chat_display_destroy(&lcd->base);
        return NULL;
    }
    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(lcd->base.lv_disp, offset_x, offset_y);
    }
    /* Give LVGL a flush_wait callback. This is a hard guard against the
     * `wait_for_flushing()` busy-spin that otherwise starves the IDLE task and
     * trips task_wdt when the emote engine swallows a flush-ready event during
     * the PAGE<->FACE renderer switch (see lvgl_port_flush_wait_cb above). */
    lv_display_set_flush_wait_cb(lcd->base.lv_disp, lvgl_port_flush_wait_cb);
    esp_xiaozhi_chat_display_setup_ui(lcd);

    /* Register a POINTER input device that reads the raw touch state from
     * ui_touch (the gesture layer), so LVGL widgets (buttons, sliders) react
     * to taps. Only ONE consumer reads the CST816S controller (ui_touch);
     * this indev just mirrors the latest position to LVGL. */
    static lv_indev_t *s_indev = NULL;
    if (s_indev == NULL && lcd->base.lv_disp != NULL) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, lvgl_port_touch_mirror_read);
        lv_indev_set_disp(s_indev, lcd->base.lv_disp);
        ESP_LOGI(TAG, "LVGL pointer indev registered (mirrors ui_touch)");
    }

    return &lcd->base;
}

/* LVGL pointer input read callback: mirrors ui_touch's latest state. */
void lvgl_port_touch_mirror_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    int x = -1, y = -1;
    bool pressed = false;
    ui_touch_get_pos(&x, &y, &pressed);
    if (pressed && x >= 0 && y >= 0) {
        data->point.x = (lv_coord_t)x;
        data->point.y = (lv_coord_t)y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->point.x = 0;
        data->point.y = 0;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static esp_err_t esp_xiaozhi_chat_display_required(void)
{
    return s_display != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t esp_xiaozhi_chat_display_init(void)
{
    esp_err_t ret = ESP_OK;
    void *lcd_handle = NULL;
    ret = esp_board_manager_get_device_handle("display_lcd", &lcd_handle);
    if (ret != ESP_OK || lcd_handle == NULL) {
        ESP_LOGW(TAG, "display_lcd handle not available: %s", esp_err_to_name(ret));
        esp_board_manager_adapter_set_lcd_backlight(100);
        return ESP_OK;
    }
    dev_display_lcd_handles_t *lcd_handles = (dev_display_lcd_handles_t *)lcd_handle;
    if (lcd_handles->io_handle == NULL || lcd_handles->panel_handle == NULL) {
        ESP_LOGW(TAG, "LCD io/panel handles NULL");
        esp_board_manager_adapter_set_lcd_backlight(100);
        return ESP_OK;
    }
    dev_display_lcd_config_t *lcd_cfg = NULL;
    ret = esp_board_manager_get_device_config("display_lcd", (void **)&lcd_cfg);
    if (ret != ESP_OK || lcd_cfg == NULL) {
        s_display = esp_xiaozhi_chat_display_create(
                        lcd_handles->io_handle, lcd_handles->panel_handle,
                        320, 240, 0, 0, 1, 1, 0,
        (display_fonts_t) {
            .text_font = &font_puhui_20_4,
            .icon_font = &font_awesome_20_4,
            .emoji_font = font_emoji_64_init(),
        });
    } else {
        s_display = esp_xiaozhi_chat_display_create(
                        lcd_handles->io_handle, lcd_handles->panel_handle,
                        (int)lcd_cfg->lcd_width, (int)lcd_cfg->lcd_height, 0, 0,
                        (bool)lcd_cfg->mirror_x, (bool)lcd_cfg->mirror_y, (bool)lcd_cfg->swap_xy,
        (display_fonts_t) {
            .text_font = &font_puhui_20_4,
            .icon_font = &font_awesome_20_4,
            .emoji_font = font_emoji_64_init(),
        });
    }
    if (s_display != NULL) {
        ESP_LOGI(TAG, "LCD display created");
    }
    esp_board_manager_adapter_set_lcd_backlight(100);
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_get_width(int *width)
{
    if (width == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *width = s_display ? s_display->width : 0;
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_get_height(int *height)
{
    if (height == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *height = s_display ? s_display->height : 0;
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_set_status(const char *status)
{
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "Invalid status");
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    if (s_emote_active) {
        emote_set_status_event(status);
        return ESP_OK;
    }

    if (!esp_xiaozhi_chat_display_lock(s_display, 30000)) {
        return ESP_OK;
    }
    if (s_display->status_label != NULL) {
        lv_label_set_text(s_display->status_label, status);
        lv_obj_clear_flag(s_display->status_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_display->notification_label != NULL) {
        lv_obj_add_flag(s_display->notification_label, LV_OBJ_FLAG_HIDDEN);
    }
    esp_xiaozhi_chat_display_unlock(s_display);

    /* Forward status to the active page (e.g. dialogue page shows it). The face
     * page does not consume status (it drives emotion directly), so no loop. */
    ui_page_manager_notify_status(status);
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_set_subtitle(const char *subtitle)
{
    ESP_RETURN_ON_FALSE(subtitle, ESP_ERR_INVALID_ARG, TAG, "Invalid subtitle");
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    if (s_emote_active) {
        emote_set_status_event(subtitle);
        return ESP_OK;
    }

    if (!esp_xiaozhi_chat_display_lock(s_display, 30000)) {
        return ESP_OK;
    }
    if (s_display->status_label != NULL) {
        lv_label_set_text(s_display->status_label, subtitle);
        lv_obj_clear_flag(s_display->status_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_display->notification_label != NULL) {
        lv_obj_add_flag(s_display->notification_label, LV_OBJ_FLAG_HIDDEN);
    }
    esp_xiaozhi_chat_display_unlock(s_display);

    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_set_volume(int volume)
{
    ESP_RETURN_ON_FALSE(volume >= 0 && volume <= 100, ESP_ERR_INVALID_ARG, TAG, "Invalid volume");
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    esp_board_manager_adapter_set_lcd_backlight(volume);

    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_set_brightness(int brightness)
{
    ESP_RETURN_ON_FALSE(brightness >= 0 && brightness <= 100, ESP_ERR_INVALID_ARG, TAG, "Invalid brightness");
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    esp_board_manager_adapter_set_lcd_backlight(brightness);

    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_set_notification(const char *notification, int duration_ms)
{
    ESP_RETURN_ON_FALSE(notification, ESP_ERR_INVALID_ARG, TAG, "Invalid notification");
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    if (s_emote_active) {
        emote_set_event_msg(s_emote, EMOTE_MGR_EVT_SYS, notification);
        return ESP_OK;
    }

    if (!esp_xiaozhi_chat_display_lock(s_display, 30000)) {
        return ESP_OK;
    }
    if (s_display->notification_label != NULL) {
        lv_label_set_text(s_display->notification_label, notification);
        lv_obj_clear_flag(s_display->notification_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_display->status_label != NULL) {
        lv_obj_add_flag(s_display->status_label, LV_OBJ_FLAG_HIDDEN);
    }
    esp_xiaozhi_chat_display_unlock(s_display);
    if (s_display->notification_timer != NULL) {
        esp_timer_stop(s_display->notification_timer);
        esp_timer_start_once(s_display->notification_timer, (uint64_t)duration_ms * 1000ULL);
    }
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_set_emotion(const char *emotion)
{
    ESP_RETURN_ON_FALSE(emotion, ESP_ERR_INVALID_ARG, TAG, "Invalid emotion");
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    if (s_emote_active) {
        emote_set_anim_emoji(s_emote, emote_anim_for(emotion));
        return ESP_OK;
    }
    /* LVGL fallback (no emote): show the emotion icon on the status label. */
    if (!esp_xiaozhi_chat_display_lock(s_display, 30000) || s_display->emotion_label == NULL) {
        return ESP_OK;
    }
    const char *icon = s_emotion_table[0].fa_icon;
    for (size_t i = 0; i < EMOTION_COUNT; ++i) {
        if (strcmp(s_emotion_table[i].text, emotion) == 0) {
            icon = s_emotion_table[i].fa_icon;
            break;
        }
    }
    lv_label_set_text(s_display->emotion_label, icon);
    esp_xiaozhi_chat_display_unlock(s_display);
    return ESP_OK;
}

/* Petting / laughing reactions used by the face page gestures. Play the
 * official emote animation on top of the current one. */
void esp_xiaozhi_chat_display_pet(void)
{
    if (s_emote_active && s_emote != NULL) {
        emote_set_anim_emoji(s_emote, "shy_20s_40s");
        emote_insert_anim_dialog(s_emote, "shy_20s_40s", 2500);
    }
}

void esp_xiaozhi_chat_display_laugh(void)
{
    if (s_emote_active && s_emote != NULL) {
        emote_set_anim_emoji(s_emote, "laugh_05s_10");
        emote_insert_anim_dialog(s_emote, "laugh_05s_10", 2500);
    }
}

esp_err_t esp_xiaozhi_chat_display_set_chat_message(const char *role, const char *content)
{
    ESP_RETURN_ON_FALSE(role, ESP_ERR_INVALID_ARG, TAG, "Invalid role");
    ESP_RETURN_ON_FALSE(content, ESP_ERR_INVALID_ARG, TAG, "Invalid content");
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    if (s_emote_active) {
        if (strcmp(role, "user") == 0) {
            emote_set_event_msg(s_emote, EMOTE_MGR_EVT_SYS, content);
        } else {
            emote_set_event_msg(s_emote, EMOTE_MGR_EVT_SPEAK, content);
        }
        return ESP_OK;
    }

    if (strlen(content) == 0) {
        return ESP_OK;
    }
    if (s_display->kind == DISPLAY_KIND_LCD) {
        lcd_display_t *lcd = esp_xiaozhi_chat_display_from_base(s_display);
        if (!esp_xiaozhi_chat_display_lock(s_display, 30000) || lcd->content == NULL) {
            return ESP_OK;
        }
        if (s_display->chat_message_label != NULL) {
            lv_label_set_text(s_display->chat_message_label, content);
        }
        esp_xiaozhi_chat_display_unlock(s_display);
        return ESP_OK;
    }
    if (!esp_xiaozhi_chat_display_lock(s_display, 30000) || s_display->chat_message_label == NULL) {
        return ESP_OK;
    }
    lv_label_set_text(s_display->chat_message_label, content);
    esp_xiaozhi_chat_display_unlock(s_display);
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_set_icon(const char *icon)
{
    ESP_RETURN_ON_FALSE(icon, ESP_ERR_INVALID_ARG, TAG, "Invalid icon");
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    if (s_display->kind == DISPLAY_KIND_LCD) {
        if (!esp_xiaozhi_chat_display_lock(s_display, 30000) || s_display->emotion_label == NULL) {
            return ESP_OK;
        }
        lv_obj_set_style_text_font(s_display->emotion_label, &font_awesome_30_4, 0);
        lv_label_set_text(s_display->emotion_label, icon);
        lv_obj_clear_flag(s_display->emotion_label, LV_OBJ_FLAG_HIDDEN);
        lcd_display_t *lcd = esp_xiaozhi_chat_display_from_base(s_display);
        if (lcd->preview_image != NULL) {
            lv_obj_add_flag(lcd->preview_image, LV_OBJ_FLAG_HIDDEN);
        }
        esp_xiaozhi_chat_display_unlock(s_display);
        return ESP_OK;
    }
    if (!esp_xiaozhi_chat_display_lock(s_display, 30000) || s_display->emotion_label == NULL) {
        return ESP_OK;
    }
    lv_label_set_text(s_display->emotion_label, icon);
    esp_xiaozhi_chat_display_unlock(s_display);
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_set_image(const void *image)
{
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    if (s_display->kind != DISPLAY_KIND_LCD) {
        return ESP_OK;
    }
    lcd_display_t *lcd = esp_xiaozhi_chat_display_from_base(s_display);
    const lv_img_dsc_t *img_dsc = (const lv_img_dsc_t *)image;
    if (!esp_xiaozhi_chat_display_lock(s_display, 30000) || lcd->content == NULL) {
        return ESP_OK;
    }
    if (lcd->preview_image != NULL) {
        if (img_dsc != NULL) {
            int scale = (img_dsc->header.w > 0) ? (128 * s_display->width / img_dsc->header.w) : 256;
            lv_image_set_scale(lcd->preview_image, scale);
            lv_image_set_src(lcd->preview_image, img_dsc);
            lv_obj_clear_flag(lcd->preview_image, LV_OBJ_FLAG_HIDDEN);
            if (s_display->emotion_label != NULL) {
                lv_obj_add_flag(s_display->emotion_label, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_add_flag(lcd->preview_image, LV_OBJ_FLAG_HIDDEN);
            if (s_display->emotion_label != NULL) {
                lv_obj_clear_flag(s_display->emotion_label, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    esp_xiaozhi_chat_display_unlock(s_display);
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_set_theme(const char *theme_name)
{
    ESP_RETURN_ON_FALSE(theme_name, ESP_ERR_INVALID_ARG, TAG, "Invalid theme name");
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    if (s_display->kind == DISPLAY_KIND_LCD) {
        lcd_display_t *lcd = esp_xiaozhi_chat_display_from_base(s_display);
        if (!esp_xiaozhi_chat_display_lock(s_display, 30000)) {
            return ESP_OK;
        }
        if (strcasecmp(theme_name, "dark") == 0) {
            lcd->current_theme = esp_xiaozhi_chat_display_dark_theme();
        } else if (strcasecmp(theme_name, "light") == 0) {
            lcd->current_theme = esp_xiaozhi_chat_display_light_theme();
        } else {
            ESP_LOGE(TAG, "Invalid theme name: %s", theme_name);
            esp_xiaozhi_chat_display_unlock(s_display);
            return ESP_ERR_INVALID_ARG;
        }
        lv_obj_t *screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lcd->current_theme.background, 0);
        lv_obj_set_style_text_color(screen, lcd->current_theme.text, 0);
        if (lcd->container != NULL) {
            lv_obj_set_style_bg_color(lcd->container, lcd->current_theme.background, 0);
            lv_obj_set_style_border_color(lcd->container, lcd->current_theme.border, 0);
        }
        if (lcd->status_bar != NULL) {
            lv_obj_set_style_bg_color(lcd->status_bar, lcd->current_theme.background, 0);
            lv_obj_set_style_text_color(lcd->status_bar, lcd->current_theme.text, 0);
        }
        if (lcd->content != NULL) {
            lv_obj_set_style_bg_color(lcd->content, lcd->current_theme.chat_background, 0);
            lv_obj_set_style_border_color(lcd->content, lcd->current_theme.border, 0);
        }
        if (s_display->low_battery_popup != NULL) {
            lv_obj_set_style_bg_color(s_display->low_battery_popup, lcd->current_theme.low_battery, 0);
        }
        strlcpy(s_display->current_theme_name, theme_name, sizeof(s_display->current_theme_name));
        esp_xiaozhi_chat_display_unlock(s_display);
        return ESP_OK;
    }
    strlcpy(s_display->current_theme_name, theme_name, sizeof(s_display->current_theme_name));
    return ESP_OK;
}
