/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "expression_emote.h"
#include "emote_defs.h"
#include "emote_table.h"
#include "emote_layout.h"
#include "widget/gfx_font_lvgl.h"
#include "cJSON.h"

// ===== Constants and Macros =====
static const char *TAG = "Expression_setup";

LV_FONT_DECLARE(font_maison_neue_book_12);
LV_FONT_DECLARE(font_maison_neue_book_26);
LV_FONT_DECLARE(font_puhui_basic_20_4);
LV_FONT_DECLARE(font_puhui_14_1);   /* vocat patch: 14px 1bpp Puhui Basic, sharper on round display */

#define OBJ_TYPE_STR_TABLE_SIZE (sizeof(obj_type_str_table) / sizeof(obj_type_str_table[0]))
#define OBJ_CREATION_TABLE_SIZE (sizeof(obj_creation_table) / sizeof(obj_creation_table[0]))

// ===== Type Definitions =====
typedef gfx_obj_t *(*obj_creator_t)(emote_handle_t handle);
typedef void (*obj_configurator_t)(gfx_obj_t *obj);

typedef struct {
    emote_obj_type_t type;
    obj_creator_t creator;
    obj_configurator_t configurator;
} obj_creation_entry_t;

typedef struct {
    const char *type_str;
    obj_creator_t creator;
    obj_configurator_t configurator;
} obj_type_str_entry_t;

typedef struct {
    const char *name;
    int value;
} align_map_t;

typedef struct {
    const char *name;
    gfx_text_align_t value;
} text_align_map_t;

typedef struct {
    const char *name;
    gfx_label_long_mode_t value;
} long_mode_map_t;

typedef struct {
    const char *name;
    emote_obj_type_t value;
} element_type_map_t;

// ===== Static Function Declarations =====
// Object creators
static gfx_obj_t *emote_create_anim_obj(emote_handle_t handle);
static gfx_obj_t *emote_create_img_obj(emote_handle_t handle);
static gfx_obj_t *emote_create_qrcode_obj(emote_handle_t handle);
static gfx_obj_t *emote_create_label_obj(emote_handle_t handle);
static gfx_obj_t *emote_create_timer_obj(emote_handle_t handle);

// Object configurators
static void emote_config_anim_obj(gfx_obj_t *obj);
static void emote_config_img_obj(gfx_obj_t *obj);
static void emote_config_qrcode_obj(gfx_obj_t *obj);
static void emote_config_label_obj(gfx_obj_t *obj);
static void emote_config_label_toast_obj(gfx_obj_t *obj);
static void emote_config_label_clock_obj(gfx_obj_t *obj);
static void emote_config_label_battery_obj(gfx_obj_t *obj);

// Helper functions
static int emote_convert_align_str(const char *str);
static gfx_text_align_t emote_convert_text_align_str(const char *str);
static gfx_label_long_mode_t emote_convert_long_mode_str(const char *str);
static emote_obj_type_t emote_get_element_type(const char *name);
static void emote_status_timer_callback(void *data);

// Object management
static gfx_obj_t *emote_create_object(emote_handle_t handle, emote_obj_type_t type);
static emote_custom_obj_entry_t *emote_find_custom_obj(emote_handle_t handle, const char *name);
static esp_err_t emote_register_custom_obj(emote_handle_t handle, const char *name, gfx_obj_t *obj);

// ===== Static Variables =====
static const obj_type_str_entry_t obj_type_str_table[] = {
    { EMOTE_OBJ_TYPE_ANIM,    emote_create_anim_obj,  emote_config_anim_obj  },
    { EMOTE_OBJ_TYPE_IMAGE,   emote_create_img_obj,   emote_config_img_obj   },
    { EMOTE_OBJ_TYPE_LABEL,   emote_create_label_obj, emote_config_label_obj },
    { EMOTE_OBJ_TYPE_QRCODE,  emote_create_qrcode_obj, emote_config_qrcode_obj },
    { EMOTE_OBJ_TYPE_TIMER,   emote_create_timer_obj, NULL                   },
};

static const obj_creation_entry_t obj_creation_table[] = {
    { EMOTE_DEF_OBJ_LEBAL_DEFAULT,  emote_create_label_obj, emote_config_label_obj          },
    { EMOTE_DEF_OBJ_ANIM_EYE,       emote_create_anim_obj,  emote_config_anim_obj          },
    { EMOTE_DEF_OBJ_ANIM_LISTEN,    emote_create_anim_obj,  emote_config_anim_obj          },
    { EMOTE_DEF_OBJ_ANIM_EMERG_DLG, emote_create_anim_obj,  emote_config_anim_obj          },
    { EMOTE_DEF_OBJ_ICON_STATUS,    emote_create_img_obj,   emote_config_img_obj           },
    { EMOTE_DEF_OBJ_ICON_CHARGE,    emote_create_img_obj,   emote_config_img_obj           },
    { EMOTE_DEF_OBJ_LABEL_TOAST,    emote_create_label_obj, emote_config_label_toast_obj   },
    { EMOTE_DEF_OBJ_LABEL_CLOCK,    emote_create_label_obj, emote_config_label_clock_obj  },
    { EMOTE_DEF_OBJ_LABEL_BATTERY,  emote_create_label_obj, emote_config_label_battery_obj },
    { EMOTE_DEF_OBJ_QRCODE,         emote_create_qrcode_obj, emote_config_qrcode_obj       },
    { EMOTE_DEF_OBJ_TIMER_STATUS,   emote_create_timer_obj, NULL                           },
};

// ===== Static Function Implementations =====

// Helper functions
static int emote_convert_align_str(const char *str)
{
    if (!str) {
        return GFX_ALIGN_DEFAULT;
    }

    static const align_map_t align_map[] = {
        { "GFX_ALIGN_TOP_LEFT", GFX_ALIGN_TOP_LEFT },
        { "GFX_ALIGN_TOP_MID", GFX_ALIGN_TOP_MID },
        { "GFX_ALIGN_TOP_RIGHT", GFX_ALIGN_TOP_RIGHT },
        { "GFX_ALIGN_LEFT_MID", GFX_ALIGN_LEFT_MID },
        { "GFX_ALIGN_CENTER", GFX_ALIGN_CENTER },
        { "GFX_ALIGN_RIGHT_MID", GFX_ALIGN_RIGHT_MID },
        { "GFX_ALIGN_BOTTOM_LEFT", GFX_ALIGN_BOTTOM_LEFT },
        { "GFX_ALIGN_BOTTOM_MID", GFX_ALIGN_BOTTOM_MID },
        { "GFX_ALIGN_BOTTOM_RIGHT", GFX_ALIGN_BOTTOM_RIGHT },
        { "GFX_ALIGN_OUT_TOP_LEFT", GFX_ALIGN_OUT_TOP_LEFT },
        { "GFX_ALIGN_OUT_TOP_MID", GFX_ALIGN_OUT_TOP_MID },
        { "GFX_ALIGN_OUT_TOP_RIGHT", GFX_ALIGN_OUT_TOP_RIGHT },
        { "GFX_ALIGN_OUT_LEFT_TOP", GFX_ALIGN_OUT_LEFT_TOP },
        { "GFX_ALIGN_OUT_LEFT_MID", GFX_ALIGN_OUT_LEFT_MID },
        { "GFX_ALIGN_OUT_LEFT_BOTTOM", GFX_ALIGN_OUT_LEFT_BOTTOM },
        { "GFX_ALIGN_OUT_RIGHT_TOP", GFX_ALIGN_OUT_RIGHT_TOP },
        { "GFX_ALIGN_OUT_RIGHT_MID", GFX_ALIGN_OUT_RIGHT_MID },
        { "GFX_ALIGN_OUT_RIGHT_BOTTOM", GFX_ALIGN_OUT_RIGHT_BOTTOM },
        { "GFX_ALIGN_OUT_BOTTOM_LEFT", GFX_ALIGN_OUT_BOTTOM_LEFT },
        { "GFX_ALIGN_OUT_BOTTOM_MID", GFX_ALIGN_OUT_BOTTOM_MID },
        { "GFX_ALIGN_OUT_BOTTOM_RIGHT", GFX_ALIGN_OUT_BOTTOM_RIGHT },
    };

    for (size_t i = 0; i < sizeof(align_map) / sizeof(align_map[0]); i++) {
        if (strcmp(str, align_map[i].name) == 0) {
            return align_map[i].value;
        }
    }

    return GFX_ALIGN_DEFAULT;
}

static gfx_text_align_t emote_convert_text_align_str(const char *str)
{
    if (!str) {
        return GFX_TEXT_ALIGN_CENTER;
    }

    static const text_align_map_t text_align_map[] = {
        { "GFX_TEXT_ALIGN_AUTO", GFX_TEXT_ALIGN_AUTO },
        { "GFX_TEXT_ALIGN_LEFT", GFX_TEXT_ALIGN_LEFT },
        { "GFX_TEXT_ALIGN_CENTER", GFX_TEXT_ALIGN_CENTER },
        { "GFX_TEXT_ALIGN_RIGHT", GFX_TEXT_ALIGN_RIGHT },
    };

    for (size_t i = 0; i < sizeof(text_align_map) / sizeof(text_align_map[0]); i++) {
        if (strcmp(str, text_align_map[i].name) == 0) {
            return text_align_map[i].value;
        }
    }

    return GFX_TEXT_ALIGN_CENTER;
}

static gfx_label_long_mode_t emote_convert_long_mode_str(const char *str)
{
    if (!str) {
        return GFX_LABEL_LONG_CLIP;
    }

    static const long_mode_map_t long_mode_map[] = {
        { "GFX_LABEL_LONG_WRAP", GFX_LABEL_LONG_WRAP },
        { "GFX_LABEL_LONG_SCROLL", GFX_LABEL_LONG_SCROLL },
        { "GFX_LABEL_LONG_CLIP", GFX_LABEL_LONG_CLIP },
        { "GFX_LABEL_LONG_SNAP", GFX_LABEL_LONG_SCROLL_SNAP },
    };

    for (size_t i = 0; i < sizeof(long_mode_map) / sizeof(long_mode_map[0]); i++) {
        if (strcmp(str, long_mode_map[i].name) == 0) {
            return long_mode_map[i].value;
        }
    }

    return GFX_LABEL_LONG_CLIP;
}

static emote_obj_type_t emote_get_element_type(const char *name)
{
    if (!name) {
        return EMOTE_DEF_OBJ_MAX;
    }

    static const element_type_map_t element_type_map[] = {
        { EMT_DEF_ELEM_DEFAULT_LABEL,  EMOTE_DEF_OBJ_LEBAL_DEFAULT  },
        { EMT_DEF_ELEM_EYE_ANIM,        EMOTE_DEF_OBJ_ANIM_EYE       },
        { EMT_DEF_ELEM_EMERG_DLG,       EMOTE_DEF_OBJ_ANIM_EMERG_DLG },
        { EMT_DEF_ELEM_TOAST_LABEL,     EMOTE_DEF_OBJ_LABEL_TOAST    },
        { EMT_DEF_ELEM_CLOCK_LABEL,     EMOTE_DEF_OBJ_LABEL_CLOCK    },
        { EMT_DEF_ELEM_LISTEN_ANIM,     EMOTE_DEF_OBJ_ANIM_LISTEN    },
        { EMT_DEF_ELEM_STATUS_ICON,     EMOTE_DEF_OBJ_ICON_STATUS    },
        { EMT_DEF_ELEM_CHARGE_ICON,     EMOTE_DEF_OBJ_ICON_CHARGE    },
        { EMT_DEF_ELEM_BAT_LEFT_LABEL,  EMOTE_DEF_OBJ_LABEL_BATTERY  },
        { EMT_DEF_ELEM_TIMER_STATUS,    EMOTE_DEF_OBJ_TIMER_STATUS   },
        { EMT_DEF_ELEM_QRCODE,          EMOTE_DEF_OBJ_QRCODE         },
    };

    for (size_t i = 0; i < sizeof(element_type_map) / sizeof(element_type_map[0]); i++) {
        if (strcmp(name, element_type_map[i].name) == 0) {
            return element_type_map[i].value;
        }
    }

    return EMOTE_DEF_OBJ_MAX;
}

static void emote_status_timer_callback(void *data)
{
    emote_handle_t handle = (emote_handle_t)data;
    if (handle) {
        emote_set_label_clock(handle);
        emote_set_bat_status_label(handle);
        emote_set_bat_status_charge(handle);
    }
}

// Object creators
static gfx_obj_t *emote_create_anim_obj(emote_handle_t handle)
{
    return gfx_anim_create(handle->gfx_disp);
}

static gfx_obj_t *emote_create_img_obj(emote_handle_t handle)
{
    return gfx_img_create(handle->gfx_disp);
}

static gfx_obj_t *emote_create_qrcode_obj(emote_handle_t handle)
{
    return gfx_qrcode_create(handle->gfx_disp);
}

static gfx_obj_t *emote_create_label_obj(emote_handle_t handle)
{
    return gfx_label_create(handle->gfx_disp);
}

static gfx_obj_t *emote_create_timer_obj(emote_handle_t handle)
{
    return (gfx_obj_t *)gfx_timer_create(handle->gfx_handle, emote_status_timer_callback, 1000, handle);
}

// Object configurators
static void emote_config_anim_obj(gfx_obj_t *obj)
{
    if (obj) {
        gfx_obj_set_pos(obj, 0, 0);
    }
}

static void emote_config_img_obj(gfx_obj_t *obj)
{
    if (obj) {
        gfx_obj_set_visible(obj, false);
    }
}

static void emote_config_qrcode_obj(gfx_obj_t *obj)
{
    if (obj) {
        // TODO: Implement gfx_qrcode_set_size if available
        // gfx_qrcode_set_size(obj, 150);
        gfx_obj_set_visible(obj, false);
    }
}

static void emote_config_label_obj(gfx_obj_t *obj)
{
    ESP_LOGI(TAG, "emote_config_label_obj: %p", obj);
    if (!obj) {
        return;
    }

    // Set default label properties
    gfx_obj_align(obj, GFX_ALIGN_CENTER, 0, 0);
    gfx_obj_set_size(obj, EMOTE_DEF_LABEL_WIDTH, EMOTE_DEF_LABEL_HEIGHT);
    gfx_label_set_color(obj, GFX_COLOR_HEX(EMOTE_DEF_FONT_COLOR));
    gfx_label_set_text_align(obj, GFX_TEXT_ALIGN_CENTER);
    gfx_label_set_long_mode(obj, GFX_LABEL_LONG_SCROLL);
    gfx_label_set_scroll_speed(obj, EMOTE_DEF_SCROLL_SPEED);
    gfx_label_set_scroll_loop(obj, true);
    // Use default font size 26
    gfx_label_set_font(obj, (void *)&font_puhui_basic_20_4);
    gfx_obj_set_visible(obj, true);
}

static void emote_config_label_toast_obj(gfx_obj_t *obj)
{
    if (!obj) {
        return;
    }

    gfx_obj_align(obj, GFX_ALIGN_TOP_MID, 0, EMOTE_DEF_LABEL_Y_OFFSET);
    gfx_obj_set_size(obj, EMOTE_DEF_LABEL_WIDTH, EMOTE_DEF_LABEL_HEIGHT);
    gfx_label_set_text(obj, "");
    gfx_label_set_color(obj, GFX_COLOR_HEX(EMOTE_DEF_FONT_COLOR));
    gfx_label_set_text_align(obj, GFX_TEXT_ALIGN_CENTER);
    gfx_label_set_long_mode(obj, GFX_LABEL_LONG_SCROLL);
    gfx_label_set_scroll_speed(obj, EMOTE_DEF_SCROLL_SPEED);
    gfx_label_set_scroll_loop(obj, true);
    /* vocat patch: 14px 1bpp Puhui Basic. The bundled 20_4 font is 4bpp
     * and produces blurry / chunky Chinese glyphs at 360x360 — switching to
     * the 14px 1bpp variant makes the on-screen caption crisp and readable.
     * (Size is set by the asset-patch index.json layout; this function only
     * sets the font and long-mode behaviour.) */
    gfx_label_set_font(obj, (void *)&font_puhui_14_1);
    gfx_obj_set_visible(obj, true);
}

static void emote_config_label_clock_obj(gfx_obj_t *obj)
{
    if (!obj) {
        return;
    }

    gfx_obj_align(obj, GFX_ALIGN_TOP_MID, 0, EMOTE_DEF_LABEL_Y_OFFSET);
    gfx_obj_set_size(obj, EMOTE_DEF_LABEL_WIDTH, EMOTE_DEF_LABEL_HEIGHT);
    gfx_label_set_text(obj, "");
    gfx_label_set_color(obj, GFX_COLOR_HEX(EMOTE_DEF_FONT_COLOR));
    gfx_label_set_text_align(obj, GFX_TEXT_ALIGN_CENTER);
    gfx_label_set_long_mode(obj, GFX_LABEL_LONG_SCROLL);
    gfx_label_set_scroll_speed(obj, EMOTE_DEF_SCROLL_SPEED);
    gfx_label_set_scroll_loop(obj, true);
    gfx_label_set_font(obj, (void *)&font_maison_neue_book_26);
    gfx_obj_set_visible(obj, true);
}

static void emote_config_label_battery_obj(gfx_obj_t *obj)
{
    if (!obj) {
        return;
    }

    gfx_obj_align(obj, GFX_ALIGN_TOP_MID, 0, EMOTE_DEF_LABEL_Y_OFFSET);
    gfx_obj_set_size(obj, EMOTE_DEF_LABEL_WIDTH, EMOTE_DEF_LABEL_HEIGHT);
    gfx_label_set_text(obj, "");
    gfx_label_set_color(obj, GFX_COLOR_HEX(EMOTE_DEF_FONT_COLOR));
    gfx_label_set_text_align(obj, GFX_TEXT_ALIGN_CENTER);
    gfx_label_set_long_mode(obj, GFX_LABEL_LONG_SCROLL);
    gfx_label_set_scroll_speed(obj, EMOTE_DEF_SCROLL_SPEED);
    gfx_label_set_scroll_loop(obj, true);
    gfx_label_set_font(obj, (void *)&font_maison_neue_book_12);
    gfx_obj_set_visible(obj, true);
}

// Object management
static gfx_obj_t *emote_create_object(emote_handle_t handle, emote_obj_type_t type)
{
    if (!handle) {
        return NULL;
    }

    gfx_obj_t *existing = handle->def_objects[type].obj;
    if (existing) {
        return existing;
    }

    gfx_handle_t gfx_handle = handle->gfx_handle;
    if (!gfx_handle) {
        return NULL;
    }

    gfx_emote_lock(gfx_handle);

    // Look up object creation entry in table
    const obj_creation_entry_t *entry = NULL;
    for (size_t i = 0; i < OBJ_CREATION_TABLE_SIZE; i++) {
        if (obj_creation_table[i].type == type) {
            entry = &obj_creation_table[i];
            break;
        }
    }

    gfx_obj_t *obj = NULL;
    if (entry && entry->creator) {
        obj = entry->creator(handle);
        if (obj && entry->configurator) {
            entry->configurator(obj);
        }
    }

    gfx_emote_unlock(gfx_handle);

    if (obj) {
        handle->def_objects[type].obj = obj;
    }

    return obj;
}

static emote_custom_obj_entry_t *emote_find_custom_obj(emote_handle_t handle, const char *name)
{
    if (!handle || !name) {
        return NULL;
    }

    emote_custom_obj_entry_t *entry = handle->custom_objects;
    while (entry) {
        if (entry->name && strcmp(entry->name, name) == 0) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

static esp_err_t emote_register_custom_obj(emote_handle_t handle, const char *name, gfx_obj_t *obj)
{
    esp_err_t ret = ESP_OK;
    emote_custom_obj_entry_t *entry = NULL;

    ESP_GOTO_ON_FALSE(handle && name && obj, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    // Check if already exists
    if (emote_find_custom_obj(handle, name)) {
        ESP_LOGW(TAG, "Custom object '%s' already exists", name);
        ret = ESP_ERR_INVALID_STATE;
        goto error;
    }

    // Create new entry
    entry = (emote_custom_obj_entry_t *)calloc(1, sizeof(emote_custom_obj_entry_t));
    ESP_GOTO_ON_FALSE(entry, ESP_ERR_NO_MEM, error, TAG, "Failed to allocate custom object entry");

    entry->name = strdup(name);
    ESP_GOTO_ON_FALSE(entry->name, ESP_ERR_NO_MEM, error, TAG, "Failed to duplicate name string");

    entry->obj = obj;
    entry->next = handle->custom_objects;
    handle->custom_objects = entry;

    ESP_LOGD(TAG, "Registered custom object: %s", name);
    return ESP_OK;

error:
    if (entry) {
        free(entry);
    }
    return ret;
}

gfx_obj_t *emote_create_obj_by_name(emote_handle_t handle, const char *name)
{
    ESP_LOGD(TAG, "create object by name: %s", name);

    // First check predefined types
    emote_obj_type_t type = emote_get_element_type(name);
    if (type != EMOTE_DEF_OBJ_MAX) {
        gfx_obj_t *obj = handle->def_objects[type].obj;
        if (!obj) {
            obj = emote_create_object(handle, type);
        }
        return obj;
    }

    // Check custom objects
    emote_custom_obj_entry_t *entry = emote_find_custom_obj(handle, name);
    if (entry) {
        return entry->obj;
    }

    ESP_LOGE(TAG, "Unknown element: %s", name);
    return NULL;
}

// ===== Public Function Implementations =====

esp_err_t emote_apply_anim_layout(emote_handle_t handle, const char *name, cJSON *layout)
{
    esp_err_t ret = ESP_OK;
    gfx_obj_t *obj = NULL;

    ESP_GOTO_ON_FALSE(handle && name && layout, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    cJSON *align = cJSON_GetObjectItem(layout, "align");
    cJSON *x = cJSON_GetObjectItem(layout, "x");
    cJSON *y = cJSON_GetObjectItem(layout, "y");

    ESP_GOTO_ON_FALSE(cJSON_IsString(align) && cJSON_IsNumber(x) && cJSON_IsNumber(y),
                      ESP_ERR_INVALID_ARG, error, TAG, "Anim %s: missing align/x/y fields", name);

    const char *align_str = align->valuestring;
    int xVal = x->valueint;
    int yVal = y->valueint;

    bool autoMirror = false;
    cJSON *animObj = cJSON_GetObjectItem(layout, EMOTE_OBJ_TYPE_ANIM);
    if (cJSON_IsObject(animObj)) {
        cJSON *mirror = cJSON_GetObjectItem(animObj, "mirror");
        if (cJSON_IsString(mirror)) {
            const char *mirrorStr = mirror->valuestring;
            autoMirror = (strcmp(mirrorStr, "auto") == 0 || strcmp(mirrorStr, "true") == 0);
        }
    }

    obj = emote_create_obj_by_name(handle, name);
    ESP_GOTO_ON_FALSE(obj, ESP_ERR_INVALID_STATE, error, TAG, "Failed to create anim: %s", name);

    /* Size the animation object to the display resolution so the 360x360 emoji
     * face fills the screen. Without an explicit size the object defaults to
     * 0x0 and the animation never renders. Width/height may be overridden via
     * the layout JSON (optional fields). */
    cJSON *w_json = cJSON_GetObjectItem(layout, "width");
    cJSON *h_json = cJSON_GetObjectItem(layout, "height");
    int obj_w = cJSON_IsNumber(w_json) ? w_json->valueint : handle->h_res;
    int obj_h = cJSON_IsNumber(h_json) ? h_json->valueint : handle->v_res;

    gfx_emote_lock(handle->gfx_handle);
    gfx_obj_set_size(obj, obj_w, obj_h);
    gfx_obj_align(obj, emote_convert_align_str(align_str), xVal, yVal);
    if (autoMirror) {
        gfx_anim_set_auto_mirror(obj, true);
    }
    gfx_obj_set_visible(obj, false);
    gfx_emote_unlock(handle->gfx_handle);

    return ESP_OK;

error:
    return ret;
}

esp_err_t emote_apply_image_layout(emote_handle_t handle, const char *name, cJSON *layout)
{
    esp_err_t ret = ESP_OK;
    gfx_obj_t *obj = NULL;

    ESP_GOTO_ON_FALSE(handle && name && layout, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    cJSON *align = cJSON_GetObjectItem(layout, "align");
    cJSON *x = cJSON_GetObjectItem(layout, "x");
    cJSON *y = cJSON_GetObjectItem(layout, "y");

    ESP_GOTO_ON_FALSE(cJSON_IsString(align) && cJSON_IsNumber(x) && cJSON_IsNumber(y),
                      ESP_ERR_INVALID_ARG, error, TAG, "Image %s: missing align/x/y fields", name);

    obj = emote_create_obj_by_name(handle, name);
    ESP_GOTO_ON_FALSE(obj, ESP_ERR_INVALID_STATE, error, TAG, "Failed to create image: %s", name);

    gfx_emote_lock(handle->gfx_handle);
    gfx_obj_align(obj, emote_convert_align_str(align->valuestring), x->valueint, y->valueint);
    gfx_obj_set_visible(obj, false);
    gfx_emote_unlock(handle->gfx_handle);

    return ESP_OK;

error:
    return ret;
}

esp_err_t emote_apply_label_layout(emote_handle_t handle, const char *name, cJSON *layout)
{
    esp_err_t ret = ESP_OK;
    gfx_obj_t *obj = NULL;

    ESP_GOTO_ON_FALSE(handle && name && layout, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    cJSON *align = cJSON_GetObjectItem(layout, "align");
    cJSON *x = cJSON_GetObjectItem(layout, "x");
    cJSON *y = cJSON_GetObjectItem(layout, "y");

    ESP_GOTO_ON_FALSE(cJSON_IsString(align) && cJSON_IsNumber(x) && cJSON_IsNumber(y),
                      ESP_ERR_INVALID_ARG, error, TAG, "Label %s: missing align/x/y fields", name);

    cJSON *width = cJSON_GetObjectItem(layout, "width");
    cJSON *height = cJSON_GetObjectItem(layout, "height");
    int w = cJSON_IsNumber(width) ? width->valueint : 0;
    int h = cJSON_IsNumber(height) ? height->valueint : 0;

    uint32_t color = EMOTE_DEF_FONT_COLOR;
    const char *textAlign = "center";
    const char *longModeType = "clip";
    bool longModeLoop = false;
    int longModeSpeed = EMOTE_DEF_SCROLL_SPEED;
    int longModeSnapInterval = 1500;

    cJSON *labelObj = cJSON_GetObjectItem(layout, EMOTE_OBJ_TYPE_LABEL);
    if (cJSON_IsObject(labelObj)) {
        cJSON *colorJson = cJSON_GetObjectItem(labelObj, "color");
        if (cJSON_IsNumber(colorJson)) {
            color = colorJson->valueint;
        }

        cJSON *textAlignJson = cJSON_GetObjectItem(labelObj, "text_align");
        if (cJSON_IsString(textAlignJson)) {
            textAlign = textAlignJson->valuestring;
        }

        cJSON *longMode = cJSON_GetObjectItem(labelObj, "long_mode");
        if (cJSON_IsObject(longMode)) {
            cJSON *longType = cJSON_GetObjectItem(longMode, "type");
            if (cJSON_IsString(longType)) {
                longModeType = longType->valuestring;
            }

            cJSON *loop = cJSON_GetObjectItem(longMode, "loop");
            if (cJSON_IsBool(loop)) {
                longModeLoop = cJSON_IsTrue(loop);
            }

            cJSON *speed = cJSON_GetObjectItem(longMode, "speed");
            if (cJSON_IsNumber(speed)) {
                longModeSpeed = speed->valueint;
            }

            cJSON *snap_interval = cJSON_GetObjectItem(longMode, "snap_interval");
            if (cJSON_IsNumber(snap_interval)) {
                longModeSnapInterval = snap_interval->valueint;
            }
        }
    }

    obj = emote_create_obj_by_name(handle, name);
    ESP_GOTO_ON_FALSE(obj, ESP_ERR_INVALID_STATE, error, TAG, "Failed to create label: %s", name);

    gfx_emote_lock(handle->gfx_handle);
    gfx_obj_align(obj, emote_convert_align_str(align->valuestring), x->valueint, y->valueint);

    if (w > 0 && h > 0) {
        gfx_obj_set_size(obj, w, h);
    }

    gfx_label_set_color(obj, GFX_COLOR_HEX(color));
    gfx_label_set_text_align(obj, emote_convert_text_align_str(textAlign));
    gfx_label_set_long_mode(obj, emote_convert_long_mode_str(longModeType));

    if (strcmp(longModeType, "GFX_LABEL_LONG_SCROLL") == 0) {
        gfx_label_set_scroll_speed(obj, longModeSpeed);
        gfx_label_set_scroll_loop(obj, longModeLoop);
    } else if (strcmp(longModeType, "GFX_LABEL_LONG_SNAP") == 0) {
        gfx_label_set_snap_loop(obj, longModeLoop);
        gfx_label_set_snap_interval(obj, longModeSnapInterval);
    }

    gfx_obj_set_visible(obj, false);
    gfx_emote_unlock(handle->gfx_handle);

    return ESP_OK;

error:
    return ret;
}

esp_err_t emote_apply_timer_layout(emote_handle_t handle, const char *name, cJSON *layout)
{
    esp_err_t ret = ESP_OK;
    gfx_obj_t *obj = NULL;

    ESP_GOTO_ON_FALSE(handle && name && layout, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    uint32_t period = 1000;
    int32_t repeat_count = -1;

    cJSON *timerObj = cJSON_GetObjectItem(layout, EMOTE_OBJ_TYPE_TIMER);
    ESP_GOTO_ON_FALSE(cJSON_IsObject(timerObj), ESP_ERR_INVALID_ARG, error, TAG, "Timer object not found for %s", name);

    cJSON *periodJson = cJSON_GetObjectItem(timerObj, "period");
    if (cJSON_IsNumber(periodJson)) {
        period = periodJson->valueint;
    }

    cJSON *repeatCountJson = cJSON_GetObjectItem(timerObj, "repeat_count");
    if (cJSON_IsNumber(repeatCountJson)) {
        repeat_count = repeatCountJson->valueint;
    }

    obj = emote_create_obj_by_name(handle, name);
    ESP_GOTO_ON_FALSE(obj, ESP_ERR_INVALID_STATE, error, TAG, "Failed to create timer: %s", name);

    gfx_emote_lock(handle->gfx_handle);
    gfx_timer_set_repeat_count(obj, repeat_count);
    gfx_timer_set_period(obj, period);
    gfx_timer_pause((gfx_timer_handle_t)obj);
    gfx_emote_unlock(handle->gfx_handle);

    return ESP_OK;

error:
    return ret;
}

esp_err_t emote_apply_qrcode_layout(emote_handle_t handle, const char *name, cJSON *layout)
{
    esp_err_t ret = ESP_OK;
    gfx_obj_t *obj = NULL;

    ESP_GOTO_ON_FALSE(handle && name && layout, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    cJSON *align = cJSON_GetObjectItem(layout, "align");
    cJSON *x = cJSON_GetObjectItem(layout, "x");
    cJSON *y = cJSON_GetObjectItem(layout, "y");

    ESP_GOTO_ON_FALSE(cJSON_IsString(align) && cJSON_IsNumber(x) && cJSON_IsNumber(y),
                      ESP_ERR_INVALID_ARG, error, TAG, "QRCode %s: missing align/x/y fields", name);

    int size = 150;  // Default QRCode size
    cJSON *qrcodeObj = cJSON_GetObjectItem(layout, EMOTE_OBJ_TYPE_QRCODE);
    if (cJSON_IsObject(qrcodeObj)) {
        cJSON *sizeJson = cJSON_GetObjectItem(qrcodeObj, "size");
        if (cJSON_IsNumber(sizeJson)) {
            size = sizeJson->valueint;
        }
    }

    obj = emote_create_obj_by_name(handle, name);
    ESP_GOTO_ON_FALSE(obj, ESP_ERR_INVALID_STATE, error, TAG, "Failed to create qrcode: %s", name);

    gfx_emote_lock(handle->gfx_handle);
    gfx_obj_align(obj, emote_convert_align_str(align->valuestring), x->valueint, y->valueint);
    if (size > 0) {
        gfx_obj_set_size(obj, size, size);
    }
    gfx_obj_set_visible(obj, false);
    gfx_emote_unlock(handle->gfx_handle);

    return ESP_OK;

error:
    return ret;
}

esp_err_t emote_apply_fonts(emote_handle_t handle, const uint8_t *fontData)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(handle && fontData, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    handle->gfx_font = gfx_font_lv_load_from_binary((uint8_t *)fontData);
    ESP_GOTO_ON_FALSE(handle->gfx_font, ESP_ERR_INVALID_STATE, error, TAG, "Failed to create font");

    gfx_obj_t *obj = handle->def_objects[EMOTE_DEF_OBJ_LABEL_TOAST].obj;
    if (obj) {
        gfx_emote_lock(handle->gfx_handle);
        gfx_label_set_font(obj, handle->gfx_font);
        gfx_emote_unlock(handle->gfx_handle);
    }

    return ESP_OK;

error:
    return ret;
}


gfx_obj_t *emote_get_obj_by_name(emote_handle_t handle, const char *name)
{
    if (!handle || !name) {
        return NULL;
    }

    // First check predefined types
    emote_obj_type_t type = emote_get_element_type(name);
    if (type != EMOTE_DEF_OBJ_MAX) {
        return handle->def_objects[type].obj;
    }

    // Check custom objects
    emote_custom_obj_entry_t *entry = emote_find_custom_obj(handle, name);
    if (entry) {
        return entry->obj;
    }

    return NULL;
}

gfx_obj_t *emote_create_obj_by_type(emote_handle_t handle, const char *type_str, const char *name)
{
    esp_err_t ret = ESP_OK;
    gfx_obj_t *obj = NULL;
    gfx_handle_t gfx_handle = NULL;
    const obj_type_str_entry_t *entry = NULL;

    ESP_GOTO_ON_FALSE(handle && name && type_str, ESP_ERR_INVALID_ARG, error, TAG, "Invalid parameters");

    // Check if object already exists
    gfx_obj_t *existing = emote_get_obj_by_name(handle, name);
    if (existing) {
        ESP_LOGW(TAG, "Object '%s' already exists", name);
        return existing;
    }

    // Find type string in mapping table
    for (size_t i = 0; i < OBJ_TYPE_STR_TABLE_SIZE; i++) {
        if (strcmp(obj_type_str_table[i].type_str, type_str) == 0) {
            entry = &obj_type_str_table[i];
            break;
        }
    }

    ESP_GOTO_ON_FALSE(entry && entry->creator, ESP_ERR_INVALID_ARG, error, TAG, "Unknown object type: %s", type_str);

    gfx_handle = handle->gfx_handle;
    ESP_GOTO_ON_FALSE(gfx_handle, ESP_ERR_INVALID_STATE, error, TAG, "GFX handle not initialized");

    gfx_emote_lock(gfx_handle);

    // Create object
    obj = entry->creator(handle);
    if (obj && entry->configurator) {
        entry->configurator(obj);
    }

    gfx_emote_unlock(gfx_handle);

    if (obj) {
        // Register as custom object
        ret = emote_register_custom_obj(handle, name, obj);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register custom object: %s", name);
            gfx_emote_lock(gfx_handle);
            gfx_obj_delete(obj);
            gfx_emote_unlock(gfx_handle);
            obj = NULL;
            goto error;
        }
        ESP_LOGI(TAG, "Created custom object '%s' of type '%s'", name, type_str);
    }

    return obj;

error:
    return NULL;
}
