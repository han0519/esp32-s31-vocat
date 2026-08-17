/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SD-card video recorder. Consumes the per-frame JPEG already produced by the
 * camera pipeline (app_camera frame callback) and appends each frame to a single
 * MJPEG file on the SD card. Zero extra encode cost — we just persist the frames
 * the live preview already generates.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Start recording. fps is the target save rate (the source is ~15fps,
 *         so frames are decimated to the nearest achievable rate).
 */
esp_err_t app_recorder_start(int fps);

/**
 * @brief  Stop recording and finalize the manifest file.
 */
esp_err_t app_recorder_stop(void);

/**
 * @brief  True while recording.
 */
bool app_recorder_is_recording(void);

/**
 * @brief  Number of frames written so far.
 */
int app_recorder_frame_count(void);

#ifdef __cplusplus
}
#endif
