/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_guard_mode.h — 番茄时钟"守护模式" (desk-watch).
 *
 * While a pomodoro is running the user can start guard mode: the camera runs in
 * the background (reusing the PPA preview path), frames are analyzed by
 * app_vision_lite (motion / activity), and the module reports whether the child
 * is still at the desk. If nobody is detected for too long, the caller is
 * notified via the returned status so it can show an alert.
 *
 * Implementation notes:
 *   - Camera is reference-counted (app_camera_start), so it can coexist with the
 *     camera page / MJPEG /take_photo.
 *   - A small task copies the latest UYVY frame, runs the PPA UYVY->RGB565
 *     transform and feeds app_vision_lite at a reduced rate (GUARD_ANALYZE_MS)
 *     to keep CPU free for the LVGL + audio.
 *   - "no body" state: after GUARD_NOBODY_ALERT_MS without any activity the
 *     status flips to GUARD_STATUS_ABSENT.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GUARD_STATUS_IDLE = 0,   /* guard not running */
    GUARD_STATUS_STARTING,
    GUARD_STATUS_ACTIVE,     /* child is moving (studying) */
    GUARD_STATUS_QUIET,      /* child still present, low activity */
    GUARD_STATUS_ABSENT,     /* nobody detected for the alert window */
} guard_status_t;

typedef struct {
    guard_status_t status;
    int            motion_ratio;   /* 0..100 latest */
    uint32_t       absent_ms;      /* monotonic time when ABSENT started (0 if not) */
} guard_info_t;

/**
 * @brief Start guard mode (camera + analyzer). Returns ESP_OK if the camera
 *        pipeline came up. Safe to call when already running (no-op).
 */
esp_err_t app_guard_mode_start(void);

/**
 * @brief Stop guard mode and release the camera.
 */
esp_err_t app_guard_mode_stop(void);

/** @brief True when guard mode is running. */
bool app_guard_mode_is_running(void);

/** @brief Snapshot of the current guard status. */
guard_info_t app_guard_mode_get_info(void);

#ifdef __cplusplus
}
#endif
