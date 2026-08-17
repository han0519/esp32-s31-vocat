/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read WiFi credentials from NVS ("wifi_creds") and connect as station.
 *
 * @param timeout_ms  Max time to wait for an IP address after starting STA.
 * @return ESP_OK on successful association + IP; ESP_ERR_NOT_FOUND if no creds
 *         stored; other esp_err_t on connect failure / timeout.
 */
esp_err_t wifi_sta_connect_from_nvs(int timeout_ms);

/**
 * @brief Enter SoftAP provisioning mode. NEVER returns.
 *
 * Starts an open SoftAP "Xiaozhi-XXXX" (XXXX from the STA MAC), a tiny HTTP
 * server with a captive-portal form at http://192.168.4.1, and a DNS redirect
 * so phones auto-open the config page. The on-board screen shows the AP name
 * and the URL. When the user submits valid credentials they are persisted to
 * NVS and the chip reboots into normal (station) mode.
 */
void provisioning_enter(void);

/**
 * @brief Clear the full-screen provisioning overlay drawn on lv_layer_top().
 *
 * Safe to call even if no overlay was ever drawn.
 */
void provisioning_hide_screen(void);

/**
 * @brief Erase the stored WiFi credentials and reboot the chip.
 *
 * Used by the UI "重置 WiFi" button: after a short delay (so the user sees the
 * confirmation), the chip reboots into SoftAP provisioning mode. Does not
 * return.
 */
void wifi_creds_erase_and_reboot(void);

#ifdef __cplusplus
}
#endif
