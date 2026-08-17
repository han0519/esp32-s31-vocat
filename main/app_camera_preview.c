/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * PPA-based on-screen camera preview (see app_camera_preview.h).
 *
 * Uses the ESP32-S31 Pixel Processing Accelerator (PPA) SRM engine to convert
 * the DVP's native 1280x720 UYVY stream into a small RGB565 preview with
 * bilinear scaling — all in hardware, no CPU, no software JPEG decode.
 *
 * Reference: esp-vision's camera_lcd_preview example follows the same
 * "native YUV -> PPA -> RGB565 -> LCD" pipeline.
 */
#include "app_camera_preview.h"

#include <string.h>
#include "esp_log.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"

static const char *TAG = "CAM_PREVIEW";

static ppa_client_handle_t s_srm = NULL;
static bool s_inited = false;

esp_err_t app_camera_preview_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    ppa_client_config_t cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    esp_err_t ret = ppa_register_client(&cfg, &s_srm);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ppa_register_client failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_inited = true;
    ESP_LOGI(TAG, "PPA SRM client ready (UYVY->RGB565 %dx%d)",
             CAM_PREVIEW_W, CAM_PREVIEW_H);
    return ESP_OK;
}

esp_err_t app_camera_preview_process(const uint8_t *yuv, int w, int h, uint8_t *out)
{
    if (!s_inited || yuv == NULL || out == NULL || w <= 0 || h <= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Keep the 16:9 (1280x720) source from being stretched into the square
     * 240x240 preview: centre-crop a square region of the source and scale
     * THAT, so people/objects keep correct proportions (no egg-head effect). */
    uint32_t src_w = (uint32_t)w;
    uint32_t src_h = (uint32_t)h;
    uint32_t crop = (src_w < src_h) ? src_w : src_h;          /* square crop side */
    uint32_t crop_x = (src_w - crop) / 2;                     /* centre horizontally */
    uint32_t crop_y = (src_h - crop) / 2;                     /* centre vertically */
    /* PPA source block must be even (UYVY 2 bytes/px); 720x720 crop is fine. */

    ppa_srm_oper_config_t srm = {
        .in = {
            .buffer = yuv,
            .pic_w = src_w,
            .pic_h = src_h,
            .block_w = crop,
            .block_h = crop,
            .block_offset_x = crop_x,
            .block_offset_y = crop_y,
            .srm_cm = PPA_SRM_COLOR_MODE_YUV422_UYVY,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = out,
            .buffer_size = (uint32_t)(CAM_PREVIEW_W * CAM_PREVIEW_H * 2),
            .pic_w = CAM_PREVIEW_W,
            .pic_h = CAM_PREVIEW_H,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = (float)CAM_PREVIEW_W / (float)crop,
        .scale_y = (float)CAM_PREVIEW_H / (float)crop,
        .mirror_x = false,
        .mirror_y = false,
        .rgb_swap = false,
        .byte_swap = false,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode = PPA_TRANS_MODE_BLOCKING,
        .user_data = NULL,
    };

    esp_err_t ret = ppa_do_scale_rotate_mirror(s_srm, &srm);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ppa_do_srm failed: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

void app_camera_preview_deinit(void)
{
    /* Keep the PPA client registered for the lifetime of the app: registering /
     * unregistering on every camera-page enter/exit repeatedly allocated and
     * freed internal RAM (the S31 has only ~62KB internal RAM free at runtime;
     * churn made allocation fail under memory pressure -> crashes over time).
     * The client is tiny and reused across page visits. */
    (void)s_srm;
    (void)s_inited;
}
