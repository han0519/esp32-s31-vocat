/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"

#include "audio_processor.h"
#include "esp_gmf_afe.h"
#include "esp_gmf_oal_sys.h"
#include "esp_gmf_oal_thread.h"
#include "esp_gmf_oal_mem.h"
#include "esp_codec_dev.h"
#include "driver/gpio.h"
#include "esp_board_manager_adapter.h"
#include "esp_board_manager_includes.h"

#include "esp_mcp_engine.h"
#include "esp_mcp_tool.h"
#include "esp_mcp_property.h"
#include "esp_mcp_data.h"
#include "esp_xiaozhi_camera.h"
#include "esp_xiaozhi_info.h"
#include "esp_xiaozhi_chat_app.h"
#include "esp_xiaozhi_chat_display.h"
#include "wifi_provisioning.h"
#include "app_audio_dsp.h"

#include "app_chat_history.h"
#include "app_sfx.h"
#include "app_music_player.h"
#include "app_activity.h"
#include "app_sdcard.h"
#include "ui_touch.h"
#include "ui_page_manager.h"
#include "ui_control_center.h"
#include "easter_egg.h"
#include "page_face.h"
#include "page_launcher.h"
#include "page_dialogue.h"
#include "page_camera.h"
#include "page_music.h"
#include "page_game.h"
#include "page_timer.h"

#if CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT
#include "app_camera.h"
#include "http_preview.h"
#endif

/* The camera take_photo tool authenticates with the xiaozhi vision service using
 * the device's own auth token (stored in the NVS keystore namespace "websocket",
 * key "token"). The keystore API is private to the esp_xiaozhi component, so its
 * header is exposed to main via main/CMakeLists.txt INCLUDE_DIRS. */
#include "esp_xiaozhi_keystore.h"

/* Page table consumed by the page manager (face + read-only dialogue + stubs).
 * Stored as pointers because the page structs live in other translation units. */
static const ui_page_t *s_pages[PAGE_MAX] = {
    [PAGE_FACE]     = &page_face,
    [PAGE_LAUNCHER] = &page_launcher,
    [PAGE_DIALOGUE] = &page_dialogue,
    [PAGE_CAMERA]   = &page_camera,
    [PAGE_MUSIC]    = &page_music,
    [PAGE_GAME]     = &page_game,
    [PAGE_TIMER]    = &page_timer,
};

static char *TAG = "ESP_XIAOZHI_CHAT_APP";

#define ESP_XIAOZHI_CHAT_APP_AUDIO_READ_ERROR_BACKOFF_MS 20
#define ESP_XIAOZHI_CHAT_APP_AUDIO_READ_ERROR_LOG_INTERVAL 50
#define ESP_XIAOZHI_CHAT_APP_AUDIO_READ_MAX_ERRORS 200
#define ESP_XIAOZHI_CHAT_APP_AUDIO_SEND_MAX_TRANSIENT_ERRORS 8

/* AFE input channel layout.
 *
 * The board adapter derives "MR" (Mic + AEC Reference) from adc_channel_mask
 * 0b0011, which is correct for Korvo-style boards that loop the speaker signal
 * back into ADC channel 1. The VoCat-S31 does NOT: the ES8389 records two real
 * differential microphones (MIC1P/N and MIC2P/N, adc_channel_mask 0x3, labelled
 * "FL,FR"), and there is NO hardware AEC reference loop.
 *
 * "MM" = two real microphones, no reference. esp-sr then enables dual-mic
 * beamforming (BF) instead of AEC, which is exactly what this hardware can use
 * to suppress ambient noise and reject the on-board speaker during barge-in.
 * We fall back to "MN" (single mic, channel 1 unused) only if the board reports
 * fewer than 2 capture channels, so a misconfigured board still boots. */
#define ESP_XIAOZHI_CHAT_APP_MIC_LAYOUT_DUAL "MM"
#define ESP_XIAOZHI_CHAT_APP_MIC_LAYOUT_SINGLE "MN"

/* Capture a short block straight from the codec at start-up and log its level.
 * This is the only reliable way to tell "the mic hardware is silent" apart from
 * "the wake word engine is misconfigured". */
#define ESP_XIAOZHI_CHAT_APP_MIC_SELFTEST_BLOCKS 3

static int current_volume = 80;   /* default out volume: 0-100 -> -50..0 dB (80 ~ -10 dB, balanced) */
static int current_brightness = 80;
static char current_theme[32] = "light";
static int current_hue = 0;          // 0..360
static int current_saturation = 0;   // 0..100
static int current_value = 0;        // 0..100
static int current_red = 0;          // 0..255
static int current_green = 0;        // 0..255
static int current_blue = 0;         // 0..255
static esp_xiaozhi_camera_handle_t *s_camera_explain = NULL;
static esp_codec_dev_handle_t s_play_dev = NULL;

/* The MCP tool callback runs in the small (4 KB) websocket_task stack. The
 * camera capture + TLS-explain path overflows that stack (Guru Meditation /
 * Stack protection fault). Offload the heavy work to a dedicated worker task
 * with a large stack; the callback just posts the job and blocks on a
 * semaphore, so the websocket_task never grows its stack. */
#define CAM_WORKER_STACK (24 * 1024)

typedef struct {
    const esp_mcp_property_list_t *properties;
    esp_mcp_value_t result;
} cam_job_t;

static TaskHandle_t s_cam_worker = NULL;
static SemaphoreHandle_t s_cam_done_sem = NULL;   /* worker -> caller */
static SemaphoreHandle_t s_cam_call_mutex = NULL; /* serialize calls */
static cam_job_t s_cam_job;

static esp_mcp_value_t camera_take_photo_do(const esp_mcp_property_list_t *properties);

static void camera_worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* wait for a job */
        if (s_cam_worker == NULL) {
            continue;
        }
        s_cam_job.result = camera_take_photo_do(s_cam_job.properties);
        xSemaphoreGive(s_cam_done_sem);
    }
}

static void camera_worker_init(void)
{
    if (s_cam_worker != NULL) {
        return;
    }
    s_cam_done_sem = xSemaphoreCreateBinary();
    s_cam_call_mutex = xSemaphoreCreateMutex();
    if (s_cam_done_sem == NULL || s_cam_call_mutex == NULL) {
        ESP_LOGE(TAG, "camera worker sem init failed");
        return;
    }
    /* The 24 KB worker stack must come from PSRAM, NOT internal RAM: with the
     * camera's ~5.5 MB of PSRAM buffers live the board has only ~32 KB of
     * internal RAM free, and xTaskCreate (internal RAM) used to exhaust it,
     * making subsequent allocations fail (page switches / MCP spawn crashes). */
    BaseType_t r = xTaskCreatePinnedToCoreWithCaps(camera_worker_task, "cam_worker",
                                                   CAM_WORKER_STACK, NULL, 4,
                                                   &s_cam_worker, 0,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "camera worker task create failed");
        s_cam_worker = NULL;
        return;
    }
    ESP_LOGI(TAG, "camera worker task started (stack=%u B, PSRAM) — take_photo offloaded off websocket_task",
             (unsigned)CAM_WORKER_STACK);
}

static esp_err_t init_camera_explain_if_needed(void)
{
    if (s_camera_explain != NULL) {
        return ESP_OK;
    }

    esp_xiaozhi_camera_config_t camera_config = {
        .explain_url = NULL,
        .explain_token = NULL,
    };
    return esp_xiaozhi_camera_create(&camera_config, &s_camera_explain);
}

static esp_mcp_value_t camera_take_photo_callback(const esp_mcp_property_list_t *properties);

/* Thin MCP entry point: runs in the small websocket_task stack. Offloads the
 * stack-heavy capture + TLS-explain work to cam_worker and blocks for it. */
static esp_mcp_value_t camera_take_photo_callback(const esp_mcp_property_list_t *properties)
{
    if (s_cam_worker == NULL || s_cam_done_sem == NULL || s_cam_call_mutex == NULL) {
        ESP_LOGE(TAG, "camera worker not ready");
        return esp_mcp_value_create_bool(false);
    }
    if (xSemaphoreTake(s_cam_call_mutex, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "camera worker busy (timeout)");
        return esp_mcp_value_create_bool(false);
    }
    s_cam_job.properties = properties;
    s_cam_job.result = esp_mcp_value_create_bool(false);
    xTaskNotifyGive(s_cam_worker);
    if (xSemaphoreTake(s_cam_done_sem, pdMS_TO_TICKS(60000)) != pdTRUE) {
        ESP_LOGE(TAG, "camera worker timeout");
        xSemaphoreGive(s_cam_call_mutex);
        return esp_mcp_value_create_bool(false);
    }
    esp_mcp_value_t r = s_cam_job.result;
    xSemaphoreGive(s_cam_call_mutex);
    return r;
}

static esp_mcp_value_t camera_take_photo_do(const esp_mcp_property_list_t *properties)
{
    /* The MCP engine rejects any call that omits a declared tool property, and
     * the xiaozhi server / external MCP clients normally invoke this tool with
     * empty arguments ("application-managed camera"). So question/url/token are
     * no longer declared as required properties; we supply them here instead:
     * a default question, the default vision endpoint, and the device's own
     * auth token from the NVS keystore. Callers that DO pass them still win via
     * the same fallback logic below. */
    const char *question = esp_mcp_property_list_get_property_string(properties, "question");
    const char *url = esp_mcp_property_list_get_property_string(properties, "url");
    const char *token = esp_mcp_property_list_get_property_string(properties, "token");

    static const char *kDefaultQuestion = "请描述你看到的画面内容。";
    static const char *kDefaultUrl = "https://api.xiaozhi.me/vision/explain";

    if (question == NULL || question[0] == '\0') {
        question = kDefaultQuestion;
    }
    if (url == NULL || url[0] == '\0') {
        url = kDefaultUrl;
    }
    char dev_token[160];
    if (token == NULL || token[0] == '\0') {
        /* fall back to the device's own xiaozhi auth token (same source the
         * websocket uses to authenticate with the server). It lives in the NVS
         * keystore namespace "websocket", key "token". */
        esp_xiaozhi_chat_keystore_t ks;
        if (esp_xiaozhi_chat_keystore_init(&ks, "websocket", false) == ESP_OK) {
            esp_xiaozhi_chat_keystore_get_string(&ks, "token", "", dev_token, sizeof(dev_token));
            esp_xiaozhi_chat_keystore_deinit(&ks);
        }
        if (dev_token[0] != '\0') {
            token = dev_token;
        }
    }

    esp_err_t ret = init_camera_explain_if_needed();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create camera explain client: %s", esp_err_to_name(ret));
        return esp_mcp_value_create_bool(false);
    }

    ret = esp_xiaozhi_camera_set_explain_url(s_camera_explain, url, token);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure explain url: %s", esp_err_to_name(ret));
        return esp_mcp_value_create_bool(false);
    }
#if CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT
    /* Capture a JPEG still via the V4L2 DVP + hardware JPEG encoder pipeline. */
    uint8_t *jpeg = NULL;
    size_t jlen = 0;
    esp_err_t cap = app_camera_capture_jpeg(&jpeg, &jlen);
    if (cap != ESP_OK || jpeg == NULL) {
        ESP_LOGE(TAG, "Failed to capture photo");
        return esp_mcp_value_create_bool(false);
    }

    esp_xiaozhi_camera_frame_t frame = {
        .data = jpeg,
        .len = jlen,
    };

    char *response = heap_caps_calloc(1, 4096, MALLOC_CAP_8BIT);
    if (response == NULL) {
        app_camera_release_jpeg(jpeg);
        ESP_LOGE(TAG, "Failed to allocate explain response buffer");
        return esp_mcp_value_create_bool(false);
    }

    size_t response_len = 0;
    cap = esp_xiaozhi_camera_explain(s_camera_explain, &frame, question, response, 4096, &response_len);
    app_camera_release_jpeg(jpeg);

    if (cap != ESP_OK) {
        ESP_LOGE(TAG, "Failed to explain photo: %s", esp_err_to_name(cap));
        free(response);
        return esp_mcp_value_create_bool(false);
    }

    esp_mcp_value_t result = esp_mcp_value_create_string(response);
    free(response);
    if (result.type == ESP_MCP_VALUE_TYPE_INVALID) {
        ESP_LOGE(TAG, "Failed to create explain result value");
        return esp_mcp_value_create_bool(false);
    }

    return result;
#else
    return esp_mcp_value_create_bool(false);
#endif
}

static esp_mcp_value_t get_device_status_callback(const esp_mcp_property_list_t* properties)
{
    ESP_LOGI(TAG, "get_device_status_callback called");

    cJSON *status = cJSON_CreateObject();
    if (!status) {
        ESP_LOGE(TAG, "Failed to create status object");
        return esp_mcp_value_create_bool(false);
    }

    cJSON *audio = cJSON_CreateObject();
    if (!audio) {
        ESP_LOGE(TAG, "Failed to create audio object");
        cJSON_Delete(status);
        return esp_mcp_value_create_bool(false);
    }

    cJSON *screen = cJSON_CreateObject();
    if (!screen) {
        ESP_LOGE(TAG, "Failed to create screen object");
        cJSON_Delete(audio);
        cJSON_Delete(status);
        return esp_mcp_value_create_bool(false);
    }

    cJSON *volume_item = cJSON_CreateNumber(current_volume);
    if (!volume_item) {
        ESP_LOGE(TAG, "Failed to create volume item");
        cJSON_Delete(screen);
        cJSON_Delete(audio);
        cJSON_Delete(status);
        return esp_mcp_value_create_bool(false);
    }
    cJSON_AddItemToObject(audio, "volume", volume_item);
    cJSON_AddItemToObject(status, "audio", audio);

    cJSON *brightness_item = cJSON_CreateNumber(current_brightness);
    if (!brightness_item) {
        ESP_LOGE(TAG, "Failed to create brightness item");
        cJSON_Delete(screen);
        cJSON_Delete(status);
        return esp_mcp_value_create_bool(false);
    }
    cJSON_AddItemToObject(screen, "brightness", brightness_item);

    cJSON *theme_item = cJSON_CreateString(current_theme);
    if (!theme_item) {
        ESP_LOGE(TAG, "Failed to create theme item");
        cJSON_Delete(screen);
        cJSON_Delete(status);
        return esp_mcp_value_create_bool(false);
    }
    cJSON_AddItemToObject(screen, "theme", theme_item);
    cJSON_AddItemToObject(status, "screen", screen);

    char *json_string = cJSON_Print(status);
    cJSON_Delete(status);

    if (!json_string) {
        ESP_LOGE(TAG, "Failed to print JSON string");
        return esp_mcp_value_create_bool(false);
    }

    esp_mcp_value_t result = esp_mcp_value_create_string(json_string);
    free(json_string);

    if (result.type == ESP_MCP_VALUE_TYPE_INVALID) {
        ESP_LOGE(TAG, "Failed to create string value for device status");
        return esp_mcp_value_create_bool(false);
    }

    return result;
}

/* Public volume / brightness controls (also used by the control center).
 * These can be called from the LVGL task (control-center slider), the MCP
 * task (set_volume / set_brightness tools) and the app task, so guard the
 * hardware access with a mutex to avoid concurrent LEDC / codec access
 * (which caused a misaligned-load crash when the slider was dragged while a
 * MCP brightness call was in flight). */
static SemaphoreHandle_t s_audio_ctrl_mutex = NULL;

/* Created once at init (single-threaded) to avoid a race between multiple
 * tasks lazily creating it. */
void esp_xiaozhi_chat_app_audio_ctrl_lock_init(void)
{
    if (s_audio_ctrl_mutex == NULL) {
        s_audio_ctrl_mutex = xSemaphoreCreateMutex();
    }
}

esp_err_t esp_xiaozhi_chat_app_set_volume(int volume)
{
    if (volume < 0 || volume > 100) {
        ESP_LOGE(TAG, "Invalid volume value: %d", volume);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_audio_ctrl_mutex && xSemaphoreTake(s_audio_ctrl_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    current_volume = volume;
    ESP_LOGI(TAG, "Volume set to: %d", current_volume);
    /* Volume 0 must actually be SILENT. The ES8389's volume register maps 0 to a
     * very low (not mute) gain, so a slider dragged to zero would still produce
     * audible output ("拖动滑动条清零没效果"). Explicitly mute the DAC at 0 and
     * unmute for anything above it. */
    if (s_play_dev != NULL) {
        if (volume == 0) {
            esp_codec_dev_set_out_mute(s_play_dev, true);
        } else {
            esp_codec_dev_set_out_mute(s_play_dev, false);
            if (esp_codec_dev_set_out_vol(s_play_dev, volume) != ESP_CODEC_DEV_OK) {
                ESP_LOGW(TAG, "Failed to apply output volume to codec device");
                if (s_audio_ctrl_mutex) {
                    xSemaphoreGive(s_audio_ctrl_mutex);
                }
                return ESP_FAIL;
            }
        }
    }
    if (s_audio_ctrl_mutex) {
        xSemaphoreGive(s_audio_ctrl_mutex);
    }
    return ESP_OK;
}

/* Minimum backlight to keep the screen visible. A slider dragged to 0 would
 * otherwise set LEDC duty to 0 -> screen goes fully black and the user cannot
 * see the slider to drag it back ("拉不回来"). A low floor (5%) keeps the
 * panel dim but usable. The slider callback snaps the knob to this value so
 * dragging toward 0 visibly dims the screen instead of doing nothing. */
#define BRIGHTNESS_MIN_PERCENT 5

esp_err_t esp_xiaozhi_chat_app_set_brightness(int brightness)
{
    if (brightness < 0 || brightness > 100) {
        ESP_LOGE(TAG, "Invalid brightness value: %d", brightness);
        return ESP_ERR_INVALID_ARG;
    }
    /* Clamp to a visible floor so the control center can always be recovered. */
    if (brightness < BRIGHTNESS_MIN_PERCENT) {
        brightness = BRIGHTNESS_MIN_PERCENT;
    }
    if (s_audio_ctrl_mutex && xSemaphoreTake(s_audio_ctrl_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    current_brightness = brightness;
    ESP_LOGI(TAG, "Brightness set to: %d", current_brightness);
    esp_xiaozhi_chat_display_set_brightness(current_brightness);
    if (s_audio_ctrl_mutex) {
        xSemaphoreGive(s_audio_ctrl_mutex);
    }
    return ESP_OK;
}

int esp_xiaozhi_chat_app_get_volume(void)     { return current_volume; }
int esp_xiaozhi_chat_app_get_brightness(void) { return current_brightness; }

static esp_mcp_value_t set_volume_callback(const esp_mcp_property_list_t* properties)
{
    int volume = esp_mcp_property_list_get_property_int(properties, "volume");
    bool ok = (esp_xiaozhi_chat_app_set_volume(volume) == ESP_OK);
    return esp_mcp_value_create_bool(ok);
}

static esp_mcp_value_t set_brightness_callback(const esp_mcp_property_list_t* properties)
{
    int brightness = esp_mcp_property_list_get_property_int(properties, "brightness");
    bool ok = (esp_xiaozhi_chat_app_set_brightness(brightness) == ESP_OK);
    return esp_mcp_value_create_bool(ok);
}

/* --- music MCP tools ------------------------------------------------------ */

static esp_mcp_value_t music_play_callback(const esp_mcp_property_list_t* properties)
{
    const char *url = esp_mcp_property_list_get_property_string(properties, "url");
    if (url == NULL || *url == '\0') {
        return esp_mcp_value_create_bool(false);
    }
    bool ok = (app_music_player_play(url) == ESP_OK);
    return esp_mcp_value_create_bool(ok);
}

static esp_mcp_value_t music_pause_callback(const esp_mcp_property_list_t* properties)
{
    (void)properties;
    bool ok = (app_music_player_pause() == ESP_OK);
    return esp_mcp_value_create_bool(ok);
}

static esp_mcp_value_t music_resume_callback(const esp_mcp_property_list_t* properties)
{
    (void)properties;
    bool ok = (app_music_player_resume() == ESP_OK);
    return esp_mcp_value_create_bool(ok);
}

static esp_mcp_value_t music_stop_callback(const esp_mcp_property_list_t* properties)
{
    (void)properties;
    bool ok = (app_music_player_stop() == ESP_OK);
    return esp_mcp_value_create_bool(ok);
}

static esp_mcp_value_t music_status_callback(const esp_mcp_property_list_t* properties)
{
    (void)properties;
    return esp_mcp_value_create_string(app_music_player_state_str());
}

static esp_mcp_value_t set_theme_callback(const esp_mcp_property_list_t* properties)
{
    const char *theme = esp_mcp_property_list_get_property_string(properties, "theme");
    if (!theme) {
        ESP_LOGE(TAG, "Failed to get theme");
        return esp_mcp_value_create_bool(false);
    }

    ESP_LOGI(TAG, "Theme set to: %s", theme);
    strncpy(current_theme, theme, sizeof(current_theme) - 1);
    current_theme[sizeof(current_theme) - 1] = '\0';

    esp_xiaozhi_chat_display_set_theme(theme);

    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t set_hsv_callback(const esp_mcp_property_list_t* properties)
{
    const char *hsv = esp_mcp_property_list_get_property_array(properties, "HSV");
    if (!hsv) {
        ESP_LOGE(TAG, "Failed to get HSV");
        return esp_mcp_value_create_bool(false);
    }

    ESP_LOGI(TAG, "HSV set to: %s", hsv);
    cJSON *hsv_json = cJSON_Parse(hsv);
    if (!hsv_json) {
        ESP_LOGE(TAG, "Invalid HSV value: %s", hsv);
        return esp_mcp_value_create_bool(false);
    }

    if (!cJSON_IsArray(hsv_json)) {
        ESP_LOGE(TAG, "Invalid HSV value (expect JSON array): %s", hsv);
        cJSON_Delete(hsv_json);
        return esp_mcp_value_create_bool(false);
    }

    int size = cJSON_GetArraySize(hsv_json);
    if (size <= 0 || size > 3) {
        ESP_LOGE(TAG, "Invalid HSV array size=%d (expect 1..3): %s", size, hsv);
        cJSON_Delete(hsv_json);
        return esp_mcp_value_create_bool(false);
    }

    int hue = current_hue;
    int saturation = current_saturation;
    int value = current_value;

    cJSON *h_item = cJSON_GetArrayItem(hsv_json, 0);
    if (h_item && !cJSON_IsNull(h_item)) {
        if (!cJSON_IsNumber(h_item)) {
            ESP_LOGE(TAG, "Invalid HSV[0] type (expect number or null): %s", hsv);
            cJSON_Delete(hsv_json);
            return esp_mcp_value_create_bool(false);
        }
        hue = h_item->valueint;
    }

    cJSON *s_item = (size >= 2) ? cJSON_GetArrayItem(hsv_json, 1) : NULL;
    if (s_item && !cJSON_IsNull(s_item)) {
        if (!cJSON_IsNumber(s_item)) {
            ESP_LOGE(TAG, "Invalid HSV[1] type (expect number or null): %s", hsv);
            cJSON_Delete(hsv_json);
            return esp_mcp_value_create_bool(false);
        }
        saturation = s_item->valueint;
    }

    cJSON *v_item = (size >= 3) ? cJSON_GetArrayItem(hsv_json, 2) : NULL;
    if (v_item && !cJSON_IsNull(v_item)) {
        if (!cJSON_IsNumber(v_item)) {
            ESP_LOGE(TAG, "Invalid HSV[2] type (expect number or null): %s", hsv);
            cJSON_Delete(hsv_json);
            return esp_mcp_value_create_bool(false);
        }
        value = v_item->valueint;
    }
    cJSON_Delete(hsv_json);

    if (hue < 0 || hue > 360 || saturation < 0 || saturation > 100 || value < 0 || value > 100) {
        ESP_LOGE(TAG, "HSV out of range: hue=%d saturation=%d value=%d (expect hue 0-360, sat/val 0-100)", hue, saturation, value);
        return esp_mcp_value_create_bool(false);
    }

    current_hue = hue;
    current_saturation = saturation;
    current_value = value;

    ESP_LOGI(TAG, "HSV set to: hue: %d, saturation: %d, value: %d", hue, saturation, value);
    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t set_rgb_callback(const esp_mcp_property_list_t* properties)
{
    const char *rgb = esp_mcp_property_list_get_property_object(properties, "RGB");
    if (!rgb) {
        ESP_LOGE(TAG, "Failed to get RGB");
        return esp_mcp_value_create_bool(false);
    }

    ESP_LOGI(TAG, "RGB set to: %s", rgb);
    cJSON *rgb_json = cJSON_Parse(rgb);
    if (!rgb_json) {
        ESP_LOGE(TAG, "Invalid RGB value: %s", rgb);
        return esp_mcp_value_create_bool(false);
    }

    if (!cJSON_IsObject(rgb_json)) {
        ESP_LOGE(TAG, "Invalid RGB value (expect JSON object): %s", rgb);
        cJSON_Delete(rgb_json);
        return esp_mcp_value_create_bool(false);
    }

    int red = current_red;
    int green = current_green;
    int blue = current_blue;
    bool updated = false;

    cJSON *r_item = cJSON_GetObjectItem(rgb_json, "red");
    if (r_item && !cJSON_IsNull(r_item)) {
        if (!cJSON_IsNumber(r_item)) {
            ESP_LOGE(TAG, "Invalid RGB.red type (expect number or null): %s", rgb);
            cJSON_Delete(rgb_json);
            return esp_mcp_value_create_bool(false);
        }
        red = r_item->valueint;
        updated = true;
    }

    cJSON *g_item = cJSON_GetObjectItem(rgb_json, "green");
    if (g_item && !cJSON_IsNull(g_item)) {
        if (!cJSON_IsNumber(g_item)) {
            ESP_LOGE(TAG, "Invalid RGB.green type (expect number or null): %s", rgb);
            cJSON_Delete(rgb_json);
            return esp_mcp_value_create_bool(false);
        }
        green = g_item->valueint;
        updated = true;
    }

    cJSON *b_item = cJSON_GetObjectItem(rgb_json, "blue");
    if (b_item && !cJSON_IsNull(b_item)) {
        if (!cJSON_IsNumber(b_item)) {
            ESP_LOGE(TAG, "Invalid RGB.blue type (expect number or null): %s", rgb);
            cJSON_Delete(rgb_json);
            return esp_mcp_value_create_bool(false);
        }
        blue = b_item->valueint;
        updated = true;
    }
    cJSON_Delete(rgb_json);

    if (!updated) {
        ESP_LOGE(TAG, "No RGB fields provided (expect at least one of red/green/blue): %s", rgb);
        return esp_mcp_value_create_bool(false);
    }

    if (red < 0 || red > 255 || green < 0 || green > 255 || blue < 0 || blue > 255) {
        ESP_LOGE(TAG, "RGB out of range: red=%d green=%d blue=%d (expect 0-255)", red, green, blue);
        return esp_mcp_value_create_bool(false);
    }

    current_red = red;
    current_green = green;
    current_blue = blue;

    ESP_LOGI(TAG, "RGB set to: red: %d, green: %d, blue: %d", red, green, blue);
    return esp_mcp_value_create_bool(true);
}

static void esp_xiaozhi_chat_app_audio_error(esp_err_t error)
{
    if (error == ESP_OK) {
        return;
    }
    /* Transient failures (mutex timeout, channel already closed, UDP backpressure): log only, no UI error */
    if (error == ESP_ERR_TIMEOUT || error == ESP_FAIL) {
        ESP_LOGW(TAG, "Send audio transient: %s", esp_err_to_name(error));
        return;
    }
    ESP_LOGE(TAG, "Failed to send audio data: %s", esp_err_to_name(error));
    esp_xiaozhi_chat_display_set_status("Error");
    esp_xiaozhi_chat_display_set_notification("Error", 2000);
    esp_xiaozhi_chat_display_set_emotion("sad");
}

static void esp_xiaozhi_chat_app_audio_event(esp_xiaozhi_chat_event_t event, void *event_data, void *ctx)
{
    switch (event) {
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STARTED:
        ESP_LOGI(TAG, "chat start");
        esp_xiaozhi_chat_display_set_status("Speaking...");
        esp_xiaozhi_chat_display_set_emotion("thinking");
        app_audio_bargein_set_playing(true);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STOPPED:
        ESP_LOGI(TAG, "chat stop");
        esp_xiaozhi_chat_display_set_status("Ready");
        esp_xiaozhi_chat_display_set_emotion("neutral");
        app_audio_bargein_set_playing(false);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE: {
        esp_xiaozhi_chat_tts_state_t *tts = (esp_xiaozhi_chat_tts_state_t *)event_data;
        if (tts) {
            if (tts->state == ESP_XIAOZHI_CHAT_TTS_STATE_START) {
                esp_xiaozhi_chat_display_set_status("Speaking...");
                esp_xiaozhi_chat_display_set_emotion("thinking");
                app_audio_bargein_set_playing(true);
            } else if (tts->state == ESP_XIAOZHI_CHAT_TTS_STATE_STOP) {
                esp_xiaozhi_chat_display_set_status("Ready");
                esp_xiaozhi_chat_display_set_emotion("neutral");
                app_audio_bargein_set_playing(false);
            }
            /* SENTENCE_START: CHAT_TEXT is also emitted, UI updated there */
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SYSTEM_CMD:
        if (event_data && strcmp((const char *)event_data, "reboot") == 0) {
            ESP_LOGI(TAG, "System command reboot, restarting");
            esp_restart();
        }
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
        esp_xiaozhi_chat_text_data_t *text_data = (esp_xiaozhi_chat_text_data_t *)event_data;
        if (text_data && text_data->text) {
            const char *role_str = (text_data->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_USER) ? "user" : "assistant";
            chat_role_t role = (text_data->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_USER)
                                 ? CHAT_ROLE_USER : CHAT_ROLE_ASSISTANT;
            /* Persist locally and forward to the active page (the face page
             * shows it as an emote toast; the dialogue page appends a bubble). */
            app_chat_history_append(role, text_data->text);
            ui_page_manager_notify_chat(role, text_data->text);
            /* AI-chat music control: "播放/搜/找 XXX" spoken to 小智 starts
             * playback on the 喵伴 (SD match, else online search). */
            if (role == CHAT_ROLE_USER) {
                char song[64];
                if (app_music_intent(text_data->text, song, sizeof(song))) {
                    app_music_play_by_name(song);
                }
            }
            (void)role_str;
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_EMOJI:
        esp_xiaozhi_chat_display_set_emotion((char *)event_data);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR: {
        esp_xiaozhi_chat_error_info_t *info = (esp_xiaozhi_chat_error_info_t *)event_data;
        if (info) {
            ESP_LOGE(TAG, "chat error: %s (source: %s)", esp_err_to_name(info->code), info->source ? info->source : "");
        } else {
            ESP_LOGE(TAG, "chat error: unknown");
        }
        esp_xiaozhi_chat_display_set_status("Error");
        esp_xiaozhi_chat_display_set_notification("Error", 2000);
        esp_xiaozhi_chat_display_set_emotion("sad");
        break;
    }
    default:
        break;
    }
}

static void esp_xiaozhi_chat_app_audio_data(const uint8_t *data, int len, void *ctx)
{
    static int s_audio_cb_count = 0;
    static int s_audio_cb_bytes = 0;
    s_audio_cb_count++;
    s_audio_cb_bytes += len;
    if (s_audio_cb_count <= 3 || (s_audio_cb_count % 50) == 0) {
        ESP_LOGI(TAG, "[TTS_IN] audio_data cb#%d len=%d total=%d", s_audio_cb_count, len, s_audio_cb_bytes);
    }
    audio_feeder_feed_data((uint8_t *)data, len);
}

static void esp_xiaozhi_chat_app_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app = (esp_xiaozhi_chat_app_t *)arg;
    (void)event_base;
    switch (event_id) {
    case ESP_XIAOZHI_CHAT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        esp_xiaozhi_chat_display_set_status("Connected");
        esp_xiaozhi_chat_display_set_notification("Connected", 2000);
        esp_xiaozhi_chat_display_set_emotion("happy");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "disconnected");
        esp_xiaozhi_chat_display_set_status("Disconnected");
        esp_xiaozhi_chat_display_set_notification("Disconnected", 2000);
        esp_xiaozhi_chat_display_set_emotion("sad");
        if (xiaozhi_chat_app->data_evt_group) {
            xEventGroupSetBits(xiaozhi_chat_app->data_evt_group, ESP_XIAOZHI_CHAT_APP_OFFLINE);
        }
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_OPENED:
        ESP_LOGI(TAG, "audio channel opened");
        xiaozhi_chat_app->wakeuped = true;
        xiaozhi_chat_app->audio_send_errors = 0;
        esp_xiaozhi_chat_display_set_status("Listening...");
        esp_xiaozhi_chat_display_set_emotion("thinking");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_CLOSED:
        ESP_LOGI(TAG, "audio channel closed");
        xiaozhi_chat_app->wakeuped = false;
        xiaozhi_chat_app->audio_send_errors = 0;
        esp_xiaozhi_chat_display_set_status("Ready");
        esp_xiaozhi_chat_display_set_emotion("neutral");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE:
        ESP_LOGI(TAG, "server goodbye");
        esp_xiaozhi_chat_display_set_status("Goodbye");
        esp_xiaozhi_chat_display_set_notification("Goodbye", 2000);
        esp_xiaozhi_chat_display_set_emotion("neutral");
        if (xiaozhi_chat_app->data_evt_group) {
            xEventGroupSetBits(xiaozhi_chat_app->data_evt_group, ESP_XIAOZHI_CHAT_APP_OFFLINE);
        }
        break;
    default:
        break;
    }
}

static esp_err_t esp_xiaozhi_chat_app_init(esp_xiaozhi_chat_app_t *xiaozhi_chat_app)
{
    ESP_RETURN_ON_FALSE(xiaozhi_chat_app != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid app");

    esp_err_t ret = ESP_OK;
    esp_xiaozhi_chat_info_t info = {0};
    esp_mcp_t *mcp = NULL;
    esp_mcp_property_t *property = NULL;
    esp_mcp_tool_t *tool = NULL;
    xiaozhi_chat_app->chat = 0;

    do {
        ret = esp_xiaozhi_chat_get_info(&info);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get chat info: %s", esp_err_to_name(ret));
            break;
        }

        ret = esp_mcp_create(&mcp);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create MCP engine: %s", esp_err_to_name(ret));
            break;
        }

        tool = esp_mcp_tool_create("self.get_device_status", "Get device status including audio, screen, battery, and network information", get_device_status_callback);
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.audio_speaker.set_volume", "Set audio speaker volume (0-100)", set_volume_callback);
        property = esp_mcp_property_create_with_range("volume", 0, 100);
        esp_mcp_tool_add_property(tool, property);
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.screen.set_brightness", "Set screen brightness (0-100)", set_brightness_callback);
        property = esp_mcp_property_create_with_range("brightness", 0, 100);
        esp_mcp_tool_add_property(tool, property);
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.screen.set_theme", "Set screen theme (light/dark)", set_theme_callback);
        property = esp_mcp_property_create_with_string("theme", "light");
        esp_mcp_tool_add_property(tool, property);
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.screen.set_hsv", "Set screen HSV, first value is hue which range is (0, 360), \
                                    second value is saturation which range is (0, 100), \
                                    third value is value which range is (0, 100)", set_hsv_callback);
        property = esp_mcp_property_create_with_array("HSV", "[120, 80, 50]");
        esp_mcp_tool_add_property(tool, property);
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.screen.set_rgb", "Set screen RGB, red value range is (0, 255), \
                                    green value range is (0, 255), \
                                    blue value range is (0, 255)", set_rgb_callback);
        property = esp_mcp_property_create_with_object("RGB", "{\"red\": 0, \"green\": 120, \"blue\": 240}");
        esp_mcp_tool_add_property(tool, property);
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.camera.take_photo",
                                   "Take a photo with an application-managed camera and send it to the vision explain service.",
                                   camera_take_photo_callback);
        /* NOTE: question/url/token are intentionally NOT declared as required
         * properties. The MCP engine rejects any tools/call that omits a declared
         * property, but the xiaozhi server / external MCP clients invoke this
         * tool with empty arguments, so the callback supplies them internally
         * (device token + default vision endpoint + default question). */
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.music.play",
                                   "Play music / audio from a stream URL (mp3/aac/wav). Pauses the TTS channel if needed.",
                                   music_play_callback);
        property = esp_mcp_property_create_with_string("url", "https://example.com/song.mp3");
        esp_mcp_tool_add_property(tool, property);
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.music.pause", "Pause the currently playing music.", music_pause_callback);
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.music.resume", "Resume paused music.", music_resume_callback);
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.music.stop", "Stop playing music.", music_stop_callback);
        esp_mcp_add_tool(mcp, tool);

        tool = esp_mcp_tool_create("self.music.get_status", "Get current music playback state (playing/paused/stopped/error).", music_status_callback);
        esp_mcp_add_tool(mcp, tool);

        esp_xiaozhi_chat_config_t chat_config = {0};
        chat_config.audio_type = ESP_XIAOZHI_CHAT_AUDIO_TYPE_OPUS;
        chat_config.audio_callback = esp_xiaozhi_chat_app_audio_data;
        chat_config.event_callback = esp_xiaozhi_chat_app_audio_event;
        chat_config.mcp_engine = mcp;
        chat_config.owns_mcp_engine = true;
        chat_config.has_mqtt_config = info.has_mqtt_config;
        chat_config.has_websocket_config = info.has_websocket_config;
#if defined(CONFIG_XIAOZHI_CHAT_APP_TRANSPORT_WEBSOCKET)
        chat_config.has_mqtt_config = false;
#endif
        ret = esp_xiaozhi_chat_init(&chat_config, &xiaozhi_chat_app->chat);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init chat: %s", esp_err_to_name(ret));
            break;
        }
        mcp = NULL;

        ret = esp_xiaozhi_chat_start(xiaozhi_chat_app->chat);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start chat: %s", esp_err_to_name(ret));
            break;
        }
    } while (0);

    if (ret != ESP_OK && xiaozhi_chat_app->chat != 0) {
        esp_xiaozhi_chat_deinit(xiaozhi_chat_app->chat);
        xiaozhi_chat_app->chat = 0;
    }
    if (mcp != NULL) {
        esp_mcp_destroy(mcp);
    }
    esp_xiaozhi_chat_free_info(&info);

    return ret;
}

static void esp_xiaozhi_chat_app_audio_recorder(void *event, void *ctx)
{
    esp_gmf_afe_evt_t *afe_evt = (esp_gmf_afe_evt_t *)event;
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app = (esp_xiaozhi_chat_app_t *)ctx;

    switch (afe_evt->type) {
    case ESP_GMF_AFE_EVT_WAKEUP_START:
        ESP_LOGI(TAG, "wakeup start");
        if (xiaozhi_chat_app->chat != 0 && xiaozhi_chat_app->data_evt_group) {
            xEventGroupSetBits(xiaozhi_chat_app->data_evt_group, ESP_XIAOZHI_CHAT_APP_ONLINE);
        }
        break;
    case ESP_GMF_AFE_EVT_WAKEUP_END:
        ESP_LOGI(TAG, "wakeup end");
        break;
    case ESP_GMF_AFE_EVT_VAD_START:
        ESP_LOGI(TAG, "vad start");
        app_audio_bargein_on_vad(true);
        break;
    case ESP_GMF_AFE_EVT_VAD_END:
        ESP_LOGI(TAG, "vad end");
        app_audio_bargein_on_vad(false);
        break;
    case ESP_GMF_AFE_EVT_VCMD_DECT_TIMEOUT:
        ESP_LOGI(TAG, "vcmd detect timeout");
        break;
    default:
        break;
    }
}

/* Called by the barge-in controller when the user's speech is detected over
 * the speaker playback. Stop the current TTS (the FIFO is discarded by
 * audio_feeder_stop, so the interrupted sentence is not replayed) and resume
 * the feeder so the next server response plays normally. */
static void esp_xiaozhi_chat_app_bargein_interrupt(void)
{
    ESP_LOGI(TAG, "Barge-in: halting current TTS playback");
    audio_feeder_stop();
    audio_feeder_run();
    app_audio_bargein_set_playing(false);
}

static void esp_xiaozhi_chat_app_audio_channel(void *pv)
{
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app = (esp_xiaozhi_chat_app_t *)pv;
    const EventBits_t wait_bits = ESP_XIAOZHI_CHAT_APP_ONLINE | ESP_XIAOZHI_CHAT_APP_OFFLINE;
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(xiaozhi_chat_app->data_evt_group, wait_bits, pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & ESP_XIAOZHI_CHAT_APP_ONLINE) {
            if (xiaozhi_chat_app->chat == 0) {
                ESP_LOGW(TAG, "Ignore online event before chat is initialized");
                continue;
            }
            bool audio_opened = false;
            esp_err_t ret = esp_xiaozhi_chat_open_audio_channel(xiaozhi_chat_app->chat, &xiaozhi_chat_app->audio, NULL, 0);
            esp_xiaozhi_chat_app_audio_error(ret);
            audio_opened = (ret == ESP_OK);
            if (ret == ESP_OK) {
                ret = esp_xiaozhi_chat_send_wake_word(xiaozhi_chat_app->chat, "你好小智");
                esp_xiaozhi_chat_app_audio_error(ret);
            }
            if (ret == ESP_OK) {
                ret = esp_xiaozhi_chat_send_start_listening(xiaozhi_chat_app->chat, 0);
                esp_xiaozhi_chat_app_audio_error(ret);
            }
            if (ret != ESP_OK && audio_opened) {
                esp_xiaozhi_chat_app_audio_error(esp_xiaozhi_chat_close_audio_channel(xiaozhi_chat_app->chat));
            }
        }

        if (bits & ESP_XIAOZHI_CHAT_APP_OFFLINE) {
            if (xiaozhi_chat_app->chat == 0) {
                ESP_LOGW(TAG, "Ignore offline event before chat is initialized");
                continue;
            }
            xiaozhi_chat_app->wakeuped = false;
            xiaozhi_chat_app->audio_send_errors = 0;
            esp_xiaozhi_chat_app_audio_error(esp_xiaozhi_chat_close_audio_channel(xiaozhi_chat_app->chat));
        }

        xEventGroupClearBits(xiaozhi_chat_app->data_evt_group, wait_bits);
    }
}

static void esp_xiaozhi_chat_app_audio_read(void *pv)
{
    int ret = 0;
    int consecutive_errors = 0;
    esp_xiaozhi_chat_app_t *xiaozhi_chat_app = (esp_xiaozhi_chat_app_t *)pv;
    uint8_t *data = esp_gmf_oal_calloc(1, ESP_XIAOZHI_CHAT_REC_READ_SIZE);
    if (data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for audio data");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        ret = audio_recorder_read_data(data, ESP_XIAOZHI_CHAT_REC_READ_SIZE);
        if (ret > 0) {
            consecutive_errors = 0;
        }

        if (ret > 0 && xiaozhi_chat_app->wakeuped && xiaozhi_chat_app->chat != 0) {
            esp_err_t send_ret = esp_xiaozhi_chat_send_audio_data(xiaozhi_chat_app->chat, (char *)data, ret);
            if (send_ret == ESP_OK) {
                xiaozhi_chat_app->audio_send_errors = 0;
            } else {
                esp_xiaozhi_chat_app_audio_error(send_ret);
                if (send_ret == ESP_FAIL || send_ret == ESP_ERR_TIMEOUT) {
                    xiaozhi_chat_app->audio_send_errors++;
                    if (xiaozhi_chat_app->audio_send_errors >= ESP_XIAOZHI_CHAT_APP_AUDIO_SEND_MAX_TRANSIENT_ERRORS) {
                        ESP_LOGW(TAG, "Too many transient audio send failures, closing audio channel");
                        xiaozhi_chat_app->wakeuped = false;
                        xiaozhi_chat_app->audio_send_errors = 0;
                        if (xiaozhi_chat_app->data_evt_group != NULL) {
                            xEventGroupSetBits(xiaozhi_chat_app->data_evt_group, ESP_XIAOZHI_CHAT_APP_OFFLINE);
                        }
                    }
                }
            }
            continue;
        }

        if (ret <= 0) {
            consecutive_errors++;
            if (consecutive_errors == 1 ||
                    (consecutive_errors % ESP_XIAOZHI_CHAT_APP_AUDIO_READ_ERROR_LOG_INTERVAL) == 0) {
                ESP_LOGW(TAG, "audio_recorder_read_data failed: %d (consecutive=%d)", ret, consecutive_errors);
            }
            if (consecutive_errors >= ESP_XIAOZHI_CHAT_APP_AUDIO_READ_MAX_ERRORS) {
                ESP_LOGE(TAG, "audio recorder read failed too many times, stopping audio read thread");
                xiaozhi_chat_app->wakeuped = false;
                if (xiaozhi_chat_app->data_evt_group != NULL) {
                    xEventGroupSetBits(xiaozhi_chat_app->data_evt_group, ESP_XIAOZHI_CHAT_APP_OFFLINE);
                }
                esp_xiaozhi_chat_display_set_status("Recorder Error");
                esp_xiaozhi_chat_display_set_notification("Recorder Error", 2000);
                esp_xiaozhi_chat_display_set_emotion("sad");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(ESP_XIAOZHI_CHAT_APP_AUDIO_READ_ERROR_BACKOFF_MS));
        }
    }

    esp_gmf_oal_free(data);
    xiaozhi_chat_app->read_thread = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Read a few raw blocks from the record codec and log their level.
 *
 * Runs once, right after the record device is opened and before the recorder
 * pipeline claims it. A peak of 0 on every channel means the ES8389 ADC is not
 * delivering samples at all (I2S RX / PGA / mic wiring); a healthy level here
 * means the problem is further up in AFE/WakeNet.
 */
static void esp_xiaozhi_chat_app_mic_selftest(esp_codec_dev_handle_t rec_dev, int sample_rate, int channels)
{
    if (rec_dev == NULL || channels <= 0 || channels > 2 || sample_rate <= 0) {
        return;
    }

    const int frames = sample_rate / 10;  /* 100 ms per block */
    const size_t bytes = (size_t)frames * (size_t)channels * sizeof(int16_t);
    int16_t *buf = (int16_t *)esp_gmf_oal_calloc(1, bytes);
    if (buf == NULL) {
        ESP_LOGW(TAG, "Mic self-test: out of memory");
        return;
    }

    /* First block after open is usually garbage while the ADC settles. */
    esp_codec_dev_read(rec_dev, buf, bytes);

    /* Diagnostic: read back the ES8389 ADC / PGA / analog registers so we can
     * tell "wired wrong" from "bias off" from "muted". peak==0 with a healthy
     * register map means the I2S RX clock is missing; peak==0 with bias bit
     * clear means the electret mics are unpowered. */
    static const int dbg_regs[] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                                   0x61, 0x62, 0x63, 0x64, 0x71, 0x72, 0x73};
    char regdump[160];
    int off = 0;
    for (size_t i = 0; i < sizeof(dbg_regs) / sizeof(dbg_regs[0]); i++) {
        int v = -1;
        esp_codec_dev_read_reg(rec_dev, dbg_regs[i], &v);
        off += snprintf(regdump + off, sizeof(regdump) - off, "0x%02x=%02d ", dbg_regs[i], v < 0 ? -1 : v);
    }
    ESP_LOGI(TAG, "Mic self-test ES8389 regs: %s", regdump);

    /* The ES8389 open sequence now powers MICBIAS (0x62 bit7) for this board, so
     * a healthy register map plus the dump above should show 0x62=0x80 and the
     * three blocks below should report non-zero peaks on both channels. */
    for (int blk = 0; blk < ESP_XIAOZHI_CHAT_APP_MIC_SELFTEST_BLOCKS; blk++) {
        int r = esp_codec_dev_read(rec_dev, buf, bytes);
        if (r != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "Mic self-test: read failed (%d)", r);
            break;
        }
        int32_t peak[2] = {0, 0};
        int64_t energy[2] = {0, 0};
        for (int i = 0; i < frames; i++) {
            for (int c = 0; c < channels; c++) {
                int32_t v = buf[i * channels + c];
                int32_t a = v < 0 ? -v : v;
                if (a > peak[c]) {
                    peak[c] = a;
                }
                energy[c] += (int64_t)v * v;
            }
        }
        if (channels == 2) {
            ESP_LOGI(TAG, "Mic self-test blk%d: ch0 peak=%d rms=%d | ch1 peak=%d rms=%d",
                     blk, (int)peak[0], (int)(energy[0] / frames > 0 ? (int)sqrt((double)(energy[0] / frames)) : 0),
                     (int)peak[1], (int)(energy[1] / frames > 0 ? (int)sqrt((double)(energy[1] / frames)) : 0));
        } else {
            ESP_LOGI(TAG, "Mic self-test blk%d: ch0 peak=%d rms=%d",
                     blk, (int)peak[0], (int)(energy[0] / frames > 0 ? (int)sqrt((double)(energy[0] / frames)) : 0));
        }
    }

    /* Mic-path fix is applied permanently in the ES8389 driver (es8389.c /
     * es8389_reg.h): 0x62 bias voltage = 0x87 (non-zero; the 0x80 default left
     * v=0 and starved the electret) and 0x72 input mux = single-ended MIC1P
     * (mux 5). This board wires the mic single-ended, not as the differential
     * MIC1P-MIC1N pair the upstream default selected, so the differential mux
     * common-mode-rejected the signal and WakeNet never saw it. */

    esp_gmf_oal_free(buf);
}

/* Network-independent audio self-test. Opens the playback and record codec
 * devices, drives the NS4150B PA (GPIO2) HIGH, plays a short boot chime to
 * prove the speaker path, reads back the DAC volume/mute registers and the PA
 * level, then captures a few mic blocks to prove the ADC path. Both devices are
 * closed afterwards so the full audio pipeline (esp_xiaozhi_chat_app_audio)
 * can re-open them cleanly. Runs on every boot, before WiFi/provisioning, so
 * the "no sound / no mic" fixes are proven locally without any network. */
static void esp_xiaozhi_chat_app_audio_selftest(esp_board_manager_adapter_info_t *bsp_info)
{
    if (bsp_info == NULL || bsp_info->play_dev == NULL || bsp_info->rec_dev == NULL) {
        ESP_LOGW(TAG, "[AUDIO_SELFTEST] skipped (no codec devices)");
        return;
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = bsp_info->sample_rate,
        .channel = bsp_info->channels,
        .bits_per_sample = bsp_info->sample_bits,
    };
    esp_codec_dev_handle_t play_dev = (esp_codec_dev_handle_t)bsp_info->play_dev;
    esp_codec_dev_handle_t rec_dev = (esp_codec_dev_handle_t)bsp_info->rec_dev;

    ESP_LOGI(TAG, "[AUDIO_SELFTEST] Opening playback codec (rate=%d bits=%d ch=%d)",
             bsp_info->sample_rate, bsp_info->sample_bits, bsp_info->channels);
    if (esp_codec_dev_open(play_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "[AUDIO_SELFTEST] Failed to open playback codec");
        return;
    }
    /* NS4150B PA (CTRL = GPIO2): ACTIVE-HIGH. Drive it HIGH to unmute the amp. */
    gpio_set_direction(2, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_drive_capability(2, GPIO_DRIVE_CAP_3);
    gpio_set_level(2, 1);

    esp_codec_dev_set_out_vol(play_dev, 60);
    app_sfx_set_play_dev(play_dev, bsp_info->sample_rate);
    app_sfx_play(APP_SFX_BOOT);

    int rb_vol = -1, rb_mute = -1;
    esp_codec_dev_read_reg(play_dev, 0x46, &rb_vol);
    esp_codec_dev_read_reg(play_dev, 0x20, &rb_mute);
    ESP_LOGI(TAG, "[AUDIO_SELFTEST] PA_GPIO2=%d  DAC_VOL_REG0x46=%d  DAC_MUTE_REG0x20=0x%02x (bit0/1=mute)",
             gpio_get_level(2), rb_vol, rb_mute < 0 ? 0 : (uint8_t)rb_mute);

    ESP_LOGI(TAG, "[AUDIO_SELFTEST] Opening record codec");
    if (esp_codec_dev_open(rec_dev, &fs) == ESP_CODEC_DEV_OK) {
        esp_codec_dev_set_in_gain(rec_dev, 24.0);
        esp_xiaozhi_chat_app_mic_selftest(rec_dev, bsp_info->sample_rate, bsp_info->channels);
        esp_codec_dev_close(rec_dev);
    } else {
        ESP_LOGE(TAG, "[AUDIO_SELFTEST] Failed to open record codec");
    }
    esp_codec_dev_close(play_dev);
    /* Reset the app_sfx global so a tap before the full audio pipeline opens
     * the device again can't write to this now-closed handle. */
    app_sfx_set_play_dev(NULL, 0);
    ESP_LOGI(TAG, "[AUDIO_SELFTEST] done (devices closed)");
}

static esp_err_t esp_xiaozhi_chat_app_audio(esp_xiaozhi_chat_app_t *xiaozhi_chat_app, esp_board_manager_adapter_info_t bsp_info)
{
    ESP_RETURN_ON_FALSE(xiaozhi_chat_app != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid app");
    ESP_RETURN_ON_FALSE(bsp_info.play_dev != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid playback device");
    ESP_RETURN_ON_FALSE(bsp_info.rec_dev != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid record device");

    av_processor_afe_config_t afe_config = DEFAULT_AV_PROCESSOR_AFE_CONFIG();
    afe_config.ai_mode_wakeup = true;

    audio_manager_config_t config = DEFAULT_AUDIO_MANAGER_CONFIG();
    esp_err_t ret = ESP_OK;
    bool audio_manager_inited = false;
    bool play_dev_opened = false;
    bool rec_dev_opened = false;
    bool playback_opened = false;
    bool recorder_opened = false;
    bool feeder_opened = false;
    bool read_thread_created = false;
    bool audio_channel_created = false;

    config.play_dev = bsp_info.play_dev;
    config.rec_dev = bsp_info.rec_dev;
    s_play_dev = (esp_codec_dev_handle_t)bsp_info.play_dev;
    /* The ES8389 is opened with no_dac_ref == false, so the codec emits
     * "ADCL + DACR" ("Set internal reference signal" in the driver log):
     * channel 0 is the microphone, channel 1 is an INTERNAL loopback of the DAC
     * output, not a second microphone. That is precisely the "MR" layout the
     * board adapter derives from the YAML, and it is what lets the AFE run AEC
     * so the device does not hear (and wake on) its own TTS. Forcing "MM" here
     * both disabled AEC ("no reference channel in mic layout") and fed the
     * beamformer the speaker signal, so prefer the adapter's layout. */
    const char *mic_layout = (bsp_info.mic_layout[0] != '\0')
        ? (const char *)bsp_info.mic_layout
        : ((bsp_info.channels >= 2) ? ESP_XIAOZHI_CHAT_APP_MIC_LAYOUT_DUAL
                                    : ESP_XIAOZHI_CHAT_APP_MIC_LAYOUT_SINGLE);
    snprintf(config.mic_layout, sizeof(config.mic_layout), "%s", mic_layout);
    ESP_LOGI(TAG, "AFE mic layout: \"%s\" (%d capture ch, board adapter suggested \"%.*s\")",
             config.mic_layout, bsp_info.channels, 8, bsp_info.mic_layout);
    config.board_sample_rate = bsp_info.sample_rate;
    config.board_bits = bsp_info.sample_bits;
    config.board_channels = bsp_info.channels;
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = config.board_sample_rate,
        .channel = config.board_channels,
        .bits_per_sample = config.board_bits,
    };
    audio_playback_config_t playback_config = DEFAULT_AUDIO_PLAYBACK_CONFIG();
    av_processor_encoder_config_t recorder_cfg = {0};
    audio_recorder_config_t recorder_config = DEFAULT_AUDIO_RECORDER_CONFIG();
    av_processor_decoder_config_t feeder_cfg = {0};
    audio_feeder_config_t feeder_config = DEFAULT_AUDIO_FEEDER_CONFIG();

    do {
        ret = audio_manager_init(&config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init audio manager: %s", esp_err_to_name(ret));
            break;
        }
        audio_manager_inited = true;

        if (esp_codec_dev_open(config.play_dev, &fs) != ESP_CODEC_DEV_OK) {
            ret = ESP_FAIL;
            ESP_LOGE(TAG, "Failed to open playback codec device");
            break;
        }
        play_dev_opened = true;

        /* NS4150B PA (CTRL = GPIO2) is owned SOLELY by this app. The ES8389
         * codec no longer drives it (pa_cfg.port = -1 in board YAML), so there
         * is no double-drive conflict. Per the NS4150B datasheet, CTRL is
         * ACTIVE-HIGH: CTRL = LOW puts the amp in shutdown (muted), CTRL = HIGH
         * enables it. v014 (the known-good firmware) drives PA HIGH, so we do
         * the same. The CTRL pin has an internal pulldown, so gpio_get_level()
         * may read 0 even while we are driving it HIGH; that readback must NOT
         * be used to decide the final level or the PA gets muted. */
        /* Use INPUT_OUTPUT so the pad's real level can be read back. With a
         * plain OUTPUT direction ESP32 disables the input buffer and
         * gpio_get_level() returns 0 even while the pin is driven HIGH — a
         * false negative that previously made us (wrongly) drop PA to LOW and
         * mute the speaker. */
        esp_err_t pa_dir = gpio_set_direction(2, GPIO_MODE_INPUT_OUTPUT);
        gpio_set_drive_capability(2, GPIO_DRIVE_CAP_3);
        esp_err_t pa_set = gpio_set_level(2, 1);
        int pa_rd = gpio_get_level(2);
        ESP_LOGI(TAG, "[AUDIO] PA_GPIO2 dir_ret=%s set_high_ret=%s readback=%d (NS4150B active-HIGH: 1=ON, 0=muted)",
                 esp_err_to_name(pa_dir), esp_err_to_name(pa_set), pa_rd);

        if (esp_codec_dev_open(config.rec_dev, &fs) != ESP_CODEC_DEV_OK) {
            ret = ESP_FAIL;
            ESP_LOGE(TAG, "Failed to open record codec device");
            break;
        }
        rec_dev_opened = true;

        /* Gain MUST be applied AFTER the codec is open. es8389_apply_mic_pga_gain()
         * silently drops the call while is_open == false, so setting it before
         * esp_codec_dev_open() was a no-op (mic self-test read all zeros). */
        if (esp_codec_dev_set_out_vol(config.play_dev, 80) != ESP_CODEC_DEV_OK) {
            ret = ESP_FAIL;
            ESP_LOGE(TAG, "Failed to set output volume");
            break;
        }
        if (esp_codec_dev_set_in_gain(config.rec_dev, 24.0) != ESP_CODEC_DEV_OK) {
            ret = ESP_FAIL;
            ESP_LOGE(TAG, "Failed to set input gain");
            break;
        }

        /* Play a short self-test chime to prove the playback path works end to
         * end (codec + PA + speaker). Harmless if it glitches against the still
         * idle audio manager. */
        app_sfx_set_play_dev(s_play_dev, config.board_sample_rate);
        app_sfx_play(APP_SFX_BOOT);

        /* SELF-TEST READBACK: confirm the codec actually left the DAC unmuted
         * with non-zero volume and the PA GPIO driven. If the speaker is still
         * silent after this, the problem is PA polarity (GPIO2 active level)
         * or the analog output stage, not the TTS pipeline. */
        int rb_vol = -1, rb_mute = -1, rb_pa = -1;
        esp_codec_dev_read_reg(config.play_dev, 0x46, &rb_vol);
        esp_codec_dev_read_reg(config.play_dev, 0x20, &rb_mute);
        rb_pa = gpio_get_level(2);
        ESP_LOGI(TAG, "[AUDIO_SELFTEST] PA_GPIO2=%d  DAC_VOL_REG0x46=%d  DAC_MUTE_REG0x20=0x%02x (bit0/1=mute)  out_vol_req=60",
                 rb_pa, rb_vol, rb_mute < 0 ? 0 : (uint8_t)rb_mute);

        esp_xiaozhi_chat_app_mic_selftest(config.rec_dev, config.board_sample_rate, config.board_channels);

        /* Initialise the playback DSP chain (ESP-Audio-Effects). av_processor
         * calls app_audio_dsp_playback_run() right before writing each decoded
         * frame to the codec. */
        if (app_audio_dsp_init_playback(config.board_sample_rate,
                                         config.board_channels,
                                         config.board_bits) != ESP_OK) {
            ESP_LOGW(TAG, "Playback DSP init failed (continuing without DSP)");
        }
        /* Network music player: routes decoded PCM to the same codec. */
        if (app_music_player_init((esp_codec_dev_handle_t)config.play_dev,
                                  config.board_sample_rate) != ESP_OK) {
            ESP_LOGW(TAG, "Music player init failed (continuing)");
        }
        app_audio_bargein_register_interrupt_cb(esp_xiaozhi_chat_app_bargein_interrupt);
        app_audio_bargein_reset();

        ret = audio_playback_open(&playback_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open audio playback: %s", esp_err_to_name(ret));
            break;
        }
        playback_opened = true;

        recorder_cfg.format = AV_PROCESSOR_FORMAT_ID_OPUS;
        recorder_cfg.params.opus.audio_info.sample_rate = xiaozhi_chat_app->audio.sample_rate;
        recorder_cfg.params.opus.audio_info.sample_bits = 16;
        recorder_cfg.params.opus.audio_info.channels = xiaozhi_chat_app->audio.channels;
        recorder_cfg.params.opus.audio_info.frame_duration = xiaozhi_chat_app->audio.frame_duration;
        recorder_cfg.params.opus.enable_vbr = false;
        recorder_cfg.params.opus.bitrate = 24000;

        recorder_config.encoder_cfg = recorder_cfg;
        recorder_config.afe_config = afe_config;
        recorder_config.recorder_event_cb = esp_xiaozhi_chat_app_audio_recorder;
        recorder_config.recorder_ctx = (void *)xiaozhi_chat_app;
        /* Mic energy meter for the barge-in controller (runs before AFE). */
        recorder_config.input_cb = app_audio_dsp_mic_input_cb;
        recorder_config.input_ctx = NULL;
        recorder_config.recorder_task_config.task_core = 1;
        ret = audio_recorder_open(&recorder_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open audio recorder: %s", esp_err_to_name(ret));
            break;
        }
        recorder_opened = true;

        feeder_cfg.format = AV_PROCESSOR_FORMAT_ID_OPUS;
        feeder_cfg.params.opus.audio_info.sample_rate = xiaozhi_chat_app->audio.sample_rate;
        feeder_cfg.params.opus.audio_info.sample_bits = 16;
        feeder_cfg.params.opus.audio_info.channels = xiaozhi_chat_app->audio.channels;
        feeder_cfg.params.opus.audio_info.frame_duration = xiaozhi_chat_app->audio.frame_duration;

        feeder_config.feeder_task_config.task_core = 1;
        feeder_config.decoder_cfg = feeder_cfg;
        ret = audio_feeder_open(&feeder_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open audio feeder: %s", esp_err_to_name(ret));
            break;
        }
        feeder_opened = true;

        ret = audio_feeder_run();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to run audio feeder: %s", esp_err_to_name(ret));
            break;
        }

        if (esp_gmf_oal_thread_create(&xiaozhi_chat_app->read_thread, "audio_read",
                                      esp_xiaozhi_chat_app_audio_read, (void *)xiaozhi_chat_app,
                                      3096, 12, true, 1) != ESP_GMF_ERR_OK) {
            ret = ESP_FAIL;
            ESP_LOGE(TAG, "Failed to create audio read thread");
            break;
        }
        read_thread_created = true;

        if (esp_gmf_oal_thread_create(&xiaozhi_chat_app->audio_channel, "audio_channel",
                                      esp_xiaozhi_chat_app_audio_channel, (void *)xiaozhi_chat_app,
                                      3096, 12, true, 1) != ESP_GMF_ERR_OK) {
            ret = ESP_FAIL;
            ESP_LOGE(TAG, "Failed to create audio channel thread");
            break;
        }
        audio_channel_created = true;
    } while (0);

    if (ret != ESP_OK) {
        if (audio_channel_created) {
            esp_gmf_oal_thread_delete(xiaozhi_chat_app->audio_channel);
            xiaozhi_chat_app->audio_channel = NULL;
        }
        if (read_thread_created) {
            esp_gmf_oal_thread_delete(xiaozhi_chat_app->read_thread);
            xiaozhi_chat_app->read_thread = NULL;
        }
        if (feeder_opened) {
            audio_feeder_close();
        }
        if (recorder_opened) {
            audio_recorder_close();
        }
        if (playback_opened) {
            audio_playback_close();
        }
        if (rec_dev_opened) {
            esp_codec_dev_close(config.rec_dev);
        }
        if (play_dev_opened) {
            esp_codec_dev_close(config.play_dev);
        }
        s_play_dev = NULL;
        if (audio_manager_inited) {
            audio_manager_deinit();
        }
    }

    return ret;
}

esp_err_t esp_xiaozhi_chat_app(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);

    esp_xiaozhi_chat_app_t *xiaozhi_chat_app = (esp_xiaozhi_chat_app_t *)calloc(1, sizeof(esp_xiaozhi_chat_app_t));
    ESP_RETURN_ON_FALSE(xiaozhi_chat_app, ESP_ERR_NO_MEM, TAG, "Failed to allocate memory for Xiaozhi chat app");
    esp_err_t ret = ESP_OK;
    bool evt_group_created = false;
    bool event_registered = false;
    bool board_inited = false;
    esp_board_manager_adapter_info_t bsp_info = {0};

    do {
        xiaozhi_chat_app->data_evt_group = xEventGroupCreate();
        if (xiaozhi_chat_app->data_evt_group == NULL) {
            ret = ESP_ERR_NO_MEM;
            ESP_LOGE(TAG, "Failed to create event group");
            break;
        }
        evt_group_created = true;

        ret = esp_event_handler_register(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID, esp_xiaozhi_chat_app_event, xiaozhi_chat_app);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register chat app event handler: %s", esp_err_to_name(ret));
            break;
        }
        event_registered = true;

        xiaozhi_chat_app->audio = (esp_xiaozhi_chat_audio_t) {
            .format = "opus",
            .sample_rate = 16000,
            .channels = 1,
            .frame_duration = 60,
        };

        esp_board_manager_adapter_config_t bsp_config = ESP_BOARD_MANAGER_ADAPTER_CONFIG_DEFAULT();
        bsp_config.enable_lcd = true;
        bsp_config.enable_lcd_backlight = true;
        bsp_config.enable_lvgl = false;
        /* ES8389 audio codec: I2C pins fixed to SDA=GPIO1/SCL=GPIO2 (addr 0x20)
         * and I2S BCLK=GPIO16, matching the official s31_vocat_xiaozhi_v014
         * firmware (verified via serial log). Codec power pin GPIO17 is driven
         * by the board profile, so the codec now responds on the I2C bus. */
        bsp_config.enable_audio = true;
        ret = esp_board_manager_adapter_init(&bsp_config, &bsp_info);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init board manager adapter: %s", esp_err_to_name(ret));
            break;
        }
        board_inited = true;

        ret = esp_xiaozhi_chat_display_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init display: %s", esp_err_to_name(ret));
            break;
        }

        /* Audio self-test: prove the playback + capture paths work end to end
         * (codec + PA + speaker, and the mic ADC) on every boot, independent of
         * the network. This is the local proof that the "no sound / no mic"
         * problems are fixed; it does NOT depend on WiFi or the xiaozhi server. */
        esp_xiaozhi_chat_app_audio_selftest(&bsp_info);

        /* Network bring-up: connect to stored WiFi credentials.
         *
         * - No credentials at all (first boot): provisioning_enter() blocks and
         *   shows the SoftAP config page; it reboots on success.
         * - Credentials exist but the AP is unreachable: we wait up to
         *   WIFI_CONNECT_TIMEOUT_MS; if still not joined, we enter provisioning
         *   so the user can (re)configure WiFi. Without the network the AI chat
         *   (emote / conversation) cannot work, so re-pairing is the right move
         *   rather than leaving the board stuck offline.
         *
         * wifi_sta_connect_from_nvs() returns:
         *   ESP_OK               -> joined the stored AP
         *   ESP_ERR_NVS_NOT_FOUND / other NVS errors -> no stored creds
         *   ESP_FAIL             -> creds exist but could not connect within the
         *                            timeout window */
#define WIFI_CONNECT_TIMEOUT_MS 30000   /* 30s: no network -> re-provision */
        esp_err_t wr = wifi_sta_connect_from_nvs(WIFI_CONNECT_TIMEOUT_MS);
        if (wr == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected to stored AP");
        } else if (wr != ESP_FAIL) {
            /* No stored credentials (first boot / wiped NVS): must provision. */
            ESP_LOGW(TAG, "No stored WiFi credentials (%s); entering provisioning",
                     esp_err_to_name(wr));
            provisioning_enter();   /* blocks; reboots on success; never returns */
        } else {
            /* Credentials exist but the AP was unreachable within the timeout:
             * re-enter provisioning so the user can fix the WiFi. */
            ESP_LOGW(TAG, "WiFi unreachable for %d ms; entering provisioning",
                     WIFI_CONNECT_TIMEOUT_MS);
            provisioning_enter();   /* blocks; reboots on success; never returns */
        }

        /* Bring up the page UI: persist history, run the page state machine, and
         * start the touch gesture layer (swipes switch pages; taps interact). */
        esp_xiaozhi_chat_app_audio_ctrl_lock_init();

        /* Mount the SD card (1-bit SDIO, GPIO20/24/25) so music playback and
         * video recording are available. Failure is non-fatal (no card inserted). */
        if (app_sdcard_init() != ESP_OK) {
            ESP_LOGW(TAG, "SD card not available (insert card for music/recording)");
        }
        app_sdcard_ensure_record_dir();

        /* Activity manager + camera web server. The server listens on port 80
         * (/, /snapshot, /stream) at all times, but the camera pipeline itself
         * now starts ON DEMAND — only while the camera page is visible, guard
         * mode / recording is active, or a web client is actually viewing. An
         * idle face page therefore consumes no streaming bandwidth, no DVP/JPEG
         * CPU and no camera PSRAM. On failure the board still boots normally. */
        app_activity_init();
        esp_err_t pre = http_preview_start();
        if (pre != ESP_OK) {
            ESP_LOGW(TAG, "camera web preview start failed (continuing): %s",
                     esp_err_to_name(pre));
        }
        camera_worker_init();
        app_chat_history_init();
        ui_page_manager_init(s_pages, PAGE_MAX);
        ui_control_center_init();
        easter_egg_init();
        ui_touch_init();
        ui_touch_set_callback(ui_page_manager_dispatch_gesture);
        ui_page_manager_switch(PAGE_FACE);

        /* Bring up the network chat. If WiFi/network is unavailable the HTTP
         * fetch fails; instead of tearing the board down (which would blank
         * the screen), keep the display alive and retry periodically. The
         * device auto-connects to the AI as soon as connectivity is present. */
        int chat_attempt = 0;
        while (1) {
            ret = esp_xiaozhi_chat_app_init(xiaozhi_chat_app);
            if (ret == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG, "Chat app init failed (attempt %d): %s; display stays on, retrying in 5s",
                     ++chat_attempt, esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(5000));
        }

        /* Audio is optional; never let it blank the device. */
        ret = esp_xiaozhi_chat_app_audio(xiaozhi_chat_app, bsp_info);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Chat app audio init failed (continuing): %s", esp_err_to_name(ret));
        }

        /* Start the official emote face LAST: its engine + animation buffers
         * claim internal SRAM, and esp-sr's WakeNet (qacc-limited) needs that
         * internal SRAM during AFE creation. Starting emote after the audio /
         * AFE pipeline is up guarantees WakeNet gets the memory it needs, so
         * the wake words keep working. */
        esp_xiaozhi_chat_display_enable_emote();
        return ESP_OK;
    } while (0);

    if (ret == ESP_OK) {
        return ESP_OK;
    }

    if (xiaozhi_chat_app->chat != 0) {
        esp_xiaozhi_chat_deinit(xiaozhi_chat_app->chat);
        xiaozhi_chat_app->chat = 0;
    }
    if (event_registered) {
        esp_event_handler_unregister(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID, esp_xiaozhi_chat_app_event);
    }
    if (board_inited) {
        esp_board_manager_adapter_deinit();
    }
    if (evt_group_created) {
        vEventGroupDelete(xiaozhi_chat_app->data_evt_group);
        xiaozhi_chat_app->data_evt_group = NULL;
    }
    free(xiaozhi_chat_app);
    return ret;
}
