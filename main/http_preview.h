/*
 * http_preview.h — local MJPEG preview server for the SC101IOT camera.
 *
 * Serves the live camera JPEG stream over HTTP so a phone/PC on the same Wi-Fi
 * can watch the picture by opening http://<board-ip>/stream (or grab a single
 * still with /snapshot). The stream is fed by app_camera's per-frame callback.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Start the preview server (also starts the camera pipeline). */
esp_err_t http_preview_start(void);

/** @brief Stop the preview server (releases the camera reference). */
esp_err_t http_preview_stop(void);

/** @brief True if the preview server is running. */
bool http_preview_is_running(void);

#ifdef __cplusplus
}
#endif
