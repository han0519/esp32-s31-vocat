/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_activity — demand-driven camera lifecycle (see app_activity.h).
 *
 * Consumers only ever manipulate a small need-mask (mutex-protected). The real
 * app_camera_start()/stop() calls happen in a single low-priority reconcile
 * task, which serialises camera power-up/down and gives us a natural "linger":
 * the camera stays warm for a few seconds after the last need disappears, so
 * rapid page toggling or a slightly-delayed web poll does not cause churn.
 */
#include "app_activity.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "app_camera.h"

static const char *TAG = "ACT_MGR";

#define ACT_RECONCILE_MS    400     /* how often the reconcile task wakes */
#define ACT_WEB_LEASE_MS    2500    /* web lease: camera kept for 2.5s after a poll */
#define ACT_LINGER_MS       3000    /* keep camera warm this long after last need */
#define ACT_FAIL_COOLDOWN_MS 5000   /* don't spin-retry a broken camera */
/* Minimum time the camera must stay OFF after a stop before we may start it
 * again. The DVP driver needs a moment to fully release its DMA state after
 * STREAMOFF; starting too early made REQBUFS fail and put the driver in a
 * broken state (the start/stop loop that crashed the camera page). */
#define ACT_STOP_COOLDOWN_MS 2000

static SemaphoreHandle_t s_mtx = NULL;
static uint32_t s_mask = 0;             /* static need bits (page/guard/rec) */
static uint64_t s_web_ts = 0;           /* last web touch (ms) */
static uint64_t s_last_need = 0;        /* last ms the effective need was non-zero */
static uint64_t s_last_fail = 0;        /* last failed app_camera_start (ms) */
static uint64_t s_last_stop = 0;        /* last app_camera_stop (ms), for cooldown */
static TaskHandle_t s_task = NULL;

static void act_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(ACT_RECONCILE_MS));
        uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);

        xSemaphoreTake(s_mtx, portMAX_DELAY);
        uint32_t eff = s_mask;
        if (now - s_web_ts < ACT_WEB_LEASE_MS) {
            eff |= APP_ACT_CAMERA_WEB;
        }
        if (eff != 0) {
            s_last_need = now;
        }
        bool running = app_camera_is_running();
        xSemaphoreGive(s_mtx);

        if (eff != 0) {
            if (!running && (now - s_last_fail) >= ACT_FAIL_COOLDOWN_MS &&
                (now - s_last_stop) >= ACT_STOP_COOLDOWN_MS) {
                esp_err_t r = app_camera_start();
                if (r != ESP_OK) {
                    s_last_fail = now;
                    ESP_LOGW(TAG, "camera start failed: %s", esp_err_to_name(r));
                } else {
                    ESP_LOGI(TAG, "camera started (need=0x%x)", (unsigned)eff);
                }
            }
        } else if (running && (now - s_last_need) >= ACT_LINGER_MS) {
            app_camera_stop();
            s_last_stop = now;
            ESP_LOGI(TAG, "camera stopped (idle, all apps silent)");
        }
    }
}

esp_err_t app_activity_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateMutex();
        if (s_mtx == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
    /* Deliberately do NOT arm the web lease at boot: a stale lease would start
     * the camera right after boot for no reason. It only becomes active after
     * the first real web request (app_activity_web_touch). */
    s_web_ts = 0;
    s_last_need = 0;
    BaseType_t t = xTaskCreatePinnedToCoreWithCaps(act_task, "act_mgr",
                                                   4096, NULL, 2, &s_task, 1,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (t != pdPASS) {
        s_task = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "activity manager started (camera runs only on demand)");
    return ESP_OK;
}

void app_activity_camera_acquire(app_activity_cam_reason_t r)
{
    if (s_mtx == NULL) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_mask |= (uint32_t)r;
    xSemaphoreGive(s_mtx);
}

void app_activity_camera_release(app_activity_cam_reason_t r)
{
    if (s_mtx == NULL) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_mask &= ~(uint32_t)r;
    xSemaphoreGive(s_mtx);
}

void app_activity_web_touch(void)
{
    if (s_mtx == NULL) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_web_ts = (uint64_t)(esp_timer_get_time() / 1000);
    xSemaphoreGive(s_mtx);
}
