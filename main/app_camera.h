/*
 * app_camera.h — SC101IOT DVP camera capture + hardware JPEG encode wrapper.
 *
 * Uses the esp_video V4L2-style API:
 *   - DVP capture device : /dev/video2  (YUV422 UYVY 1280x720 @ 15fps)
 *   - JPEG encoder device: /dev/video10 (S31 native hardware JPEG M2M)
 *
 * The capture stream and the encoder are shared by a single reference-counted
 * instance. A background task streams frames; each frame is hardware-encoded to
 * JPEG and handed to a registered callback (used by the MJPEG preview) and kept
 * as the "latest" frame for one-shot capture (used by the MCP take_photo tool).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* JPEG quality 1..100 (higher = better). Lowered to 70: the 1280x720 stream is
 * served to the web viewer by polling /snapshot, and a quality-70 JPEG is ~40%
 * smaller than quality-80 (~90 KB vs ~150 KB), which materially improves the
 * refresh rate over Wi-Fi while staying sharp enough for a monitoring view. */
#define APP_CAMERA_JPEG_QUALITY 70

/* Callback invoked with each encoded JPEG frame. `buf`/`len` are valid only for
 * the duration of the call; copy if you need to keep them. Returns void. */
typedef void (*app_camera_frame_cb_t)(const uint8_t *jpeg, size_t len, void *ctx);

/* Callback invoked with each raw DVP UYVY frame (1280x720, 2 bytes/pixel).
 * Used by the on-screen PPA preview — this path never touches JPEG decode. */
typedef void (*app_camera_yuv_cb_t)(const uint8_t *uyvy, int w, int h, void *ctx);

/**
 * @brief Start the camera pipeline (reference counted). On first call it powers
 *        the camera, opens /dev/video2 + /dev/video10, starts the DVP stream and
 *        the encoder, and spawns the streaming task.
 */
esp_err_t app_camera_start(void);

/**
 * @brief Stop the camera pipeline (reference counted). When the count reaches 0
 *        the stream and devices are closed and the task deleted.
 */
esp_err_t app_camera_stop(void);

/**
 * @brief One-shot capture: grab the latest encoded JPEG frame (or capture a
 *        fresh one if none is buffered) and return a heap-allocated copy.
 *        Caller must free with app_camera_release_jpeg().
 *        The camera is auto-started if not already running.
 */
esp_err_t app_camera_capture_jpeg(uint8_t **out_buf, size_t *out_len);

/** @brief Free a buffer returned by app_camera_capture_jpeg(). */
void app_camera_release_jpeg(uint8_t *buf);

/**
 * @brief Register a per-frame JPEG callback (MJPEG preview, on-screen camera
 *        page, ...). Multiple callbacks may coexist (up to
 *        APP_CAMERA_MAX_FRAME_CB); registering the same callback updates its
 *        context in place. Pass NULL to unregister ALL consumers.
 *        Callbacks run from the streaming task; copy the buffer if you need to
 *        keep it (valid only for the duration of the call).
 */
esp_err_t app_camera_register_frame_cb(app_camera_frame_cb_t cb, void *ctx);

/**
 * @brief Unregister a single per-frame JPEG callback (matched by function
 *        pointer). Use this instead of passing NULL to register_frame_cb when
 *        you must NOT disturb other consumers (e.g. the live MJPEG preview).
 */
esp_err_t app_camera_unregister_frame_cb(app_camera_frame_cb_t cb);

/**
 * @brief Register the raw UYVY frame callback (on-screen PPA preview).
 *        Pass NULL to unregister ALL consumers. Runs from the streaming task;
 *        the buffer is valid only for the duration of the call.
 */
esp_err_t app_camera_register_yuv_cb(app_camera_yuv_cb_t cb, void *ctx);

/** @brief True if the camera pipeline is currently running. */
bool app_camera_is_running(void);

#ifdef __cplusplus
}
#endif
