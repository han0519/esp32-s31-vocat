/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_activity — activity / resource scheduler for the 喵伴 board.
 *
 * Only the currently-active app should run its heavy task; everything else stays
 * silent. In particular the camera + DVP stream + JPEG encoder consume both WiFi
 * bandwidth (when pushed to the web viewer) and a big slice of the scarce
 * internal SRAM. This module makes the camera lifecycle demand-driven:
 *
 *   camera runs  <=>  (camera page visible) | (guard mode on) | (recording) |
 *                     (a web viewer is actively polling /stream or /snapshot)
 *
 * Consumers declare their need with app_activity_camera_acquire()/release()
 * (cheap, non-blocking on the camera). A low-priority task reconciles the
 * effective need against app_camera_is_running() and starts/stops the pipeline.
 * The web need is a short lease refreshed by app_activity_web_touch(), so a
 * stateless polling browser keeps the camera alive while open, and the camera
 * automatically stops a few seconds after the tab is closed or the page exits.
 *
 * Returning to the face (emote) page with no web viewer therefore turns the
 * camera fully off: zero streaming bandwidth, zero DVP/JPEG CPU, and the PSRAM
 * capture buffers are released — which also keeps the tight internal-SRAM head
 * room healthy and avoids the OOM-class crashes seen while the camera churned
 * in the background.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Reason a consumer needs the camera. Bitmask; a consumer may hold several. */
typedef enum {
    APP_ACT_CAMERA_PAGE  = (1 << 0),  /*!< camera page is on screen */
    APP_ACT_CAMERA_GUARD = (1 << 1),  /*!< timer "guard mode" is active */
    APP_ACT_CAMERA_WEB   = (1 << 2),  /*!< a web client is viewing (lease-based) */
    APP_ACT_CAMERA_REC   = (1 << 3),  /*!< SD recorder is running */
} app_activity_cam_reason_t;

/**
 * @brief  Start the activity manager (mutex + reconcile task).
 *         Safe to call multiple times; no-op when already running.
 */
esp_err_t app_activity_init(void);

/** @brief  Declare that the given consumer needs the camera. */
void app_activity_camera_acquire(app_activity_cam_reason_t r);

/** @brief  Declare that the given consumer no longer needs the camera. */
void app_activity_camera_release(app_activity_cam_reason_t r);

/** @brief  Refresh the web-viewer lease (call from http handlers). */
void app_activity_web_touch(void);

#ifdef __cplusplus
}
#endif
