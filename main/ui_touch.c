/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ui_touch.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_board_manager.h"
#include "dev_lcd_touch.h"
#include "esp_lcd_touch.h"

static const char *TAG = "UI_TOUCH";

#define TOUCH_POLL_MS   12
#define TOUCH_STACK     2048
#define TOUCH_PRIO      6

static esp_lcd_touch_handle_t s_touch = NULL;
static ui_touch_cb_t s_cb = NULL;
static TaskHandle_t s_task = NULL;
static TaskHandle_t s_disp_task = NULL;
static QueueHandle_t s_gesture_queue = NULL;
static bool s_running = false;

/* Gestures are classified in the polling touch task but DISPATCHED in a separate
 * task. This keeps the poller free to keep feeding LVGL pointer input even while
 * a page switch (which can take a while under the LVGL lock) is in progress,
 * so the UI never feels frozen / unresponsive. */
typedef struct {
    ui_gesture_t g;
    int x;
    int y;
} gesture_msg_t;

/* Per-contact tracking state. */
static bool     s_down = false;
static int      s_down_x = 0;
static int      s_down_y = 0;
static int64_t  s_down_ms = 0;
static int      s_last_x = 0;
static int      s_last_y = 0;

/* Latest raw touch position + press state, shared with the LVGL pointer
 * input device (see ui_touch_get_pos). Only this task writes it. */
static volatile int  s_cur_x = -1;
static volatile int  s_cur_y = -1;
static volatile bool s_cur_pressed = false;

static int64_t now_ms(void)
{
    return (int64_t)(esp_timer_get_time() / 1000);
}

static void emit(ui_gesture_t g, int x, int y)
{
    if (s_gesture_queue == NULL) {
        /* Queue not ready yet (early boot) — dispatch directly as a fallback. */
        if (s_cb) {
            s_cb(g, x, y);
        }
        return;
    }
    gesture_msg_t msg = { g, x, y };
    /* Best-effort enqueue; drop if the consumer is somehow behind. */
    xQueueSend(s_gesture_queue, &msg, 0);
}

static void ui_touch_dispatch_task(void *arg)
{
    (void)arg;
    gesture_msg_t msg;
    while (1) {
        if (xQueueReceive(s_gesture_queue, &msg, portMAX_DELAY) == pdTRUE) {
            if (s_cb) {
                s_cb(msg.g, msg.x, msg.y);
            }
        }
    }
}

static void classify_release(void)
{
    int64_t dt = now_ms() - s_down_ms;
    int dx = s_last_x - s_down_x;
    int dy = s_last_y - s_down_y;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;

    if (dt >= UI_LONG_PRESS_MS && adx < UI_SWIPE_MIN_DIST_PX && ady < UI_SWIPE_MIN_DIST_PX) {
        emit(UI_GESTURE_LONG_PRESS, s_last_x, s_last_y);
    } else if (adx >= UI_SWIPE_MIN_DIST_PX || ady >= UI_SWIPE_MIN_DIST_PX) {
        if (adx > ady) {
            emit(dx > 0 ? UI_GESTURE_SWIPE_RIGHT : UI_GESTURE_SWIPE_LEFT, s_last_x, s_last_y);
        } else {
            emit(dy > 0 ? UI_GESTURE_SWIPE_DOWN : UI_GESTURE_SWIPE_UP, s_last_x, s_last_y);
        }
    } else if (dt <= UI_TAP_MAX_MS) {
        emit(UI_GESTURE_TAP, s_last_x, s_last_y);
    }
}

static void touch_task(void *arg)
{
    (void)arg;
    esp_lcd_touch_point_data_t pts[1];
    uint8_t point_cnt = 0;

    while (s_running) {
        if (s_touch == NULL) {
            vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
            continue;
        }
        esp_lcd_touch_read_data(s_touch);
        esp_lcd_touch_get_data(s_touch, pts, &point_cnt, 1);

        if (point_cnt > 0) {
            s_last_x = pts[0].x;
            s_last_y = pts[0].y;
            s_cur_x = pts[0].x;
            s_cur_y = pts[0].y;
            s_cur_pressed = true;
            if (!s_down) {
                s_down = true;
                s_down_x = pts[0].x;
                s_down_y = pts[0].y;
                s_down_ms = now_ms();
            }
        } else if (s_down) {
            /* Transition from pressed to released. */
            s_down = false;
            s_cur_pressed = false;
            classify_release();
        }
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
    vTaskDelete(NULL);
}

esp_err_t ui_touch_init(void)
{
    if (s_running) {
        return ESP_OK;
    }
    void *handle = NULL;
    esp_err_t ret = esp_board_manager_get_device_handle("lcd_touch", &handle);
    if (ret != ESP_OK || handle == NULL) {
        ESP_LOGW(TAG, "lcd_touch handle unavailable (%s); touch disabled",
                 esp_err_to_name(ret));
        return ret;
    }
    dev_lcd_touch_handles_t *h = (dev_lcd_touch_handles_t *)handle;
    if (h->touch_handle == NULL) {
        ESP_LOGW(TAG, "lcd_touch handle has NULL touch_handle; touch disabled");
        return ESP_ERR_INVALID_STATE;
    }
    s_touch = h->touch_handle;

    if (s_gesture_queue == NULL) {
        s_gesture_queue = xQueueCreate(8, sizeof(gesture_msg_t));
        if (s_gesture_queue == NULL) {
            ESP_LOGE(TAG, "failed to create gesture queue");
            return ESP_FAIL;
        }
    }

    s_running = true;
    BaseType_t t = xTaskCreate(touch_task, "ui_touch", TOUCH_STACK, NULL, TOUCH_PRIO, &s_task);
    if (t != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "failed to create touch task");
        return ESP_FAIL;
    }
    BaseType_t dt = xTaskCreate(ui_touch_dispatch_task, "ui_touch_disp", 3072, NULL, 5, &s_disp_task);
    if (dt != pdPASS) {
        ESP_LOGE(TAG, "failed to create gesture dispatch task");
        /* Non-fatal: gestures would just be dropped; keep the touch poller. */
    }
    ESP_LOGI(TAG, "touch layer ready");
    return ESP_OK;
}

void ui_touch_set_callback(ui_touch_cb_t cb)
{
    s_cb = cb;
}

void ui_touch_get_pos(int *x, int *y, bool *pressed)
{
    if (x) {
        *x = s_cur_pressed ? (int)s_cur_x : -1;
    }
    if (y) {
        *y = s_cur_pressed ? (int)s_cur_y : -1;
    }
    if (pressed) {
        *pressed = s_cur_pressed;
    }
}

esp_err_t ui_touch_deinit(void)
{
    if (s_task) {
        s_running = false;
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS * 2));
        s_task = NULL;
    }
    if (s_disp_task) {
        vTaskDelete(s_disp_task);
        s_disp_task = NULL;
    }
    if (s_gesture_queue) {
        vQueueDelete(s_gesture_queue);
        s_gesture_queue = NULL;
    }
    s_touch = NULL;
    s_cb = NULL;
    return ESP_OK;
}
