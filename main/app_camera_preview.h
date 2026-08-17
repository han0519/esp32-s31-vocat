/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-screen camera preview for the ESP-VoCat-S31 "喵伴" using the S31 PPA
 * (Pixel Processing Accelerator).
 *
 * The DVP outputs 1280x720 UYVY (2 bytes/pixel). esp_new_jpeg software
 * decode of that stream crashes on this S31 (jpeg_dec_process), so instead we
 * hand each UYVY frame to the PPA SRM engine, which does hardware color
 * conversion (UYVY -> RGB565) + bilinear scaling to a small round-screen
 * preview (e.g. 240x240) with almost zero CPU cost.
 *
 * API: app_camera_preview_init() opens a PPA SRM client and allocates the
 * output buffer. app_camera_preview_process(yuv, w, h) runs the PPA transform
 * and returns a pointer to the RGB565 output (valid until the next call).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAM_PREVIEW_W 240   /* output preview width (multiple of 8) */
#define CAM_PREVIEW_H 240   /* output preview height */

/**
 * @brief Initialize the PPA preview channel.
 *
 * @return ESP_OK on success
 */
esp_err_t app_camera_preview_init(void);

/**
 * @brief Convert one UYVY frame to RGB565 + scale via the PPA hardware.
 *
 * @param[in]  yuv     1280x720 UYVY (2 bytes/pixel)
 * @param[in]  w,h     source dimensions (1280x720)
 * @param[out] out     caller-provided buffer (CAM_PREVIEW_W*CAM_PREVIEW_H*2 bytes),
 *                     must be 16-byte aligned (use heap_caps_aligned_alloc)
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t app_camera_preview_process(const uint8_t *yuv, int w, int h, uint8_t *out);

/**
 * @brief Free the PPA client (call on page exit).
 */
void app_camera_preview_deinit(void);

#ifdef __cplusplus
}
#endif
