/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_xiaozhi_chat_app.h"

static const char *TAG = "app_main";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* WiFi is owned by the chat app now: it connects to stored credentials
     * and, if none are configured, enters SoftAP provisioning (the board
     * broadcasts its own AP + a captive-portal config page). We must NOT call
     * example_connect() here too, or wifi would be initialized twice. */

    /* The chat app brings up the board (LCD/emote), connects to the xiaozhi AI
     * service, and (if the audio codec is reachable) voice. With the board
     * adapter now tolerating a missing codec and the chat app retrying on
     * network failure, the device stays alive (screen lit) even when audio
     * hardware or WiFi are temporarily unavailable. */
    esp_xiaozhi_chat_app();
}
