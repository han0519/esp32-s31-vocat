/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Camera page for the ESP-VoCat-S31 "喵伴".
 *
 * Shows a LIVE preview ON the round screen using the S31 PPA hardware
 * accelerator (like esp-vision's camera_lcd_preview): each raw DVP UYVY frame
 * is hardware-converted to RGB565 + scaled to 240x240, then blitted to an LVGL
 * canvas. The MJPEG HTTP preview (http://<ip>/stream) keeps working for
 * browsers, and take_photo still uploads the hardware-JPEG to the vision
 * service.
 *
 * This deliberately does NOT use esp_new_jpeg software decode (jpeg_dec_process
 * crashes on this S31 with large streams); PPA runs in hardware.
 */
#include "page_camera.h"

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "app_sfx.h"
#include "app_camera.h"
#include "app_camera_preview.h"
#include "app_vision_lite.h"
#include "app_activity.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_xiaozhi_chat_display.h"
#include "lvgl.h"

#define TAG "PAGE_CAMERA"

/* DVP capture resolution (matches app_camera). */
#define CAM_WIDTH  1280
#define CAM_HEIGHT 720

static lv_obj_t *s_canvas = NULL;
static lv_color16_t *s_canvas_buf = NULL;   /* PSRAM, aligned for PPA output */
static lv_obj_t *s_status = NULL;
static lv_obj_t *s_ip_label = NULL;         /* web access IP (updated on tick) */

/* --- PPA preview task ---------------------------------------------------
 * The PPA transform + canvas blit runs on a small PSRAM-stack task so the
 * streaming camera task never blocks (it must keep the V4L2 queues moving).
 * The LVGL lock is only taken for the cheap blit. */
static SemaphoreHandle_t s_frame_mutex = NULL;
static uint8_t *s_frame = NULL;             /* PSRAM, latest UYVY copy */
static size_t   s_frame_len = 0;
static uint32_t s_frame_seq = 0;
static TaskHandle_t s_prev_task = NULL;
static volatile bool s_prev_running = false;
static volatile bool s_prev_exited = false;

#define PREV_TASK_STACK 12 * 1024
/* ~22 fps ceiling; the DVP runs faster but the LVGL canvas redraw + SPI flush
 * of 240x240 is the bottleneck — pushing more frames just queues work and
 * makes the UI feel laggy. */
#define PREV_POLL_MS    45

static void prev_task(void *arg)
{
    (void)arg;
    /* Output buffer: 16-byte aligned, PSRAM (PPA requires alignment). */
    size_t out_size = CAM_PREVIEW_W * CAM_PREVIEW_H * 2;
    /* PPA requires the PSRAM output buffer to be aligned to L1+L2 cache line
     * (64 bytes); 16-byte alignment is NOT enough (ppa_core rejects it). */
    uint8_t *out = (uint8_t *)heap_caps_aligned_alloc(64, out_size,
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out == NULL) {
        ESP_LOGE(TAG, "preview out buf alloc failed");
        s_prev_exited = true;
        vTaskDelete(NULL);
        return;
    }
    /* PPA client is registered once by app_camera_start() (stays for app
     * lifetime); here we only use it. */

    uint32_t last_seq = 0;
    uint32_t frame_no = 0;
    while (s_prev_running) {
        xSemaphoreTake(s_frame_mutex, portMAX_DELAY);
        bool has = (s_frame != NULL && s_frame_len >= 8);
        uint32_t seq = s_frame_seq;
        xSemaphoreGive(s_frame_mutex);
        if (!has || seq == last_seq) {
            vTaskDelay(pdMS_TO_TICKS(PREV_POLL_MS));
            continue;
        }

        xSemaphoreTake(s_frame_mutex, portMAX_DELAY);
        uint8_t *yuv = s_frame;   /* owned by us; not modified outside */
        size_t len = s_frame_len;
        xSemaphoreGive(s_frame_mutex);

        /* Hardware UYVY -> RGB565 + scale (no software decode). */
        if (len >= (size_t)(CAM_WIDTH * CAM_HEIGHT * 2) &&
            app_camera_preview_process(yuv, CAM_WIDTH, CAM_HEIGHT, out) == ESP_OK) {
            /* Vision analysis only every 3rd frame: motion/activity doesn't need
             * full rate and it keeps CPU free for LVGL + audio (less lag). */
            frame_no++;
            if ((frame_no % 3) == 1) {
                app_vision_lite_feed_rgb565(out, CAM_PREVIEW_W, CAM_PREVIEW_H,
                                            (uint32_t)(esp_timer_get_time() / 1000));
            }
            if (s_prev_running && lvgl_port_lock(1000)) {
                if (s_canvas && s_canvas_buf) {
                    memcpy(s_canvas_buf, out, out_size);
                    lv_obj_invalidate(s_canvas);
                }
                lvgl_port_unlock();
            }
        }
        last_seq = seq;
        vTaskDelay(pdMS_TO_TICKS(PREV_POLL_MS));
    }

    heap_caps_free(out);
    /* PPA client stays registered for app lifetime (app_camera_start created
     * it); do NOT unregister here to avoid churning scarce internal RAM. */
    if (xSemaphoreTake(s_frame_mutex, 0) == pdTRUE) {
        if (s_frame) {
            heap_caps_free(s_frame);
            s_frame = NULL;
        }
        s_frame_len = 0;
        xSemaphoreGive(s_frame_mutex);
    }
    s_prev_exited = true;
    vTaskDelete(NULL);
}

static void on_yuv(const uint8_t *uyvy, int w, int h, void *ctx)
{
    (void)w;
    (void)h;
    (void)ctx;
    if (uyvy == NULL || s_frame_mutex == NULL || !s_prev_running) {
        return;
    }
    if (xSemaphoreTake(s_frame_mutex, 0) != pdTRUE) {
        return;   /* preview busy, drop this frame */
    }
    size_t len = (size_t)CAM_WIDTH * CAM_HEIGHT * 2;
    if (s_frame && s_frame_len >= len) {
        memcpy(s_frame, uyvy, len);
        s_frame_len = len;
    } else {
        uint8_t *nb = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (nb) {
            if (s_frame) heap_caps_free(s_frame);
            s_frame = nb;
            memcpy(s_frame, uyvy, len);
            s_frame_len = len;
        }
    }
    s_frame_seq++;
    xSemaphoreGive(s_frame_mutex);
}

static void build_preview(void)
{
    lv_obj_t *root = esp_xiaozhi_chat_display_page_root();
    if (root == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);

    extern lv_obj_t *ui_page_make_back_button(lv_obj_t *);
    ui_page_make_back_button(root);
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "摄像头");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    if (s_canvas_buf == NULL) {
        s_canvas_buf = (lv_color16_t *)heap_caps_aligned_alloc(
            64, CAM_PREVIEW_W * CAM_PREVIEW_H * 2,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_canvas_buf == NULL) {
            return;
        }
        memset(s_canvas_buf, 0, CAM_PREVIEW_W * CAM_PREVIEW_H * 2);
    }
    s_canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, CAM_PREVIEW_W, CAM_PREVIEW_H,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 0);

    s_status = lv_label_create(root);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_status, LV_HOR_RES - 16);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_label_set_text(s_status, "预览启动中...");

    /* Web access IP, shown separately from the vision status line so the
     * on_tick motion refresh never overwrites it. Re-fetched each tick to
     * follow DHCP lease changes. */
    s_ip_label = lv_label_create(root);
    lv_obj_set_style_text_color(s_ip_label, lv_color_hex(0x3D8BFD), 0);
    lv_obj_set_style_text_align(s_ip_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_ip_label, LV_HOR_RES - 16);
    lv_obj_align(s_ip_label, LV_ALIGN_TOP_MID, 0, 34);
    lv_label_set_text(s_ip_label, "IP: --");
}

esp_err_t page_camera_on_enter(void)
{
    app_sfx_play(APP_SFX_PAGE);
    build_preview();

    if (s_frame_mutex == NULL) {
        s_frame_mutex = xSemaphoreCreateMutex();
    }
    /* Wait for a previous preview task to wind down (without LVGL lock). */
    if (s_prev_task != NULL) {
        lvgl_port_unlock();
        int guard = 0;
        while (!s_prev_exited && guard++ < 100) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        lvgl_port_lock(1000);
        s_prev_task = NULL;
    }
    if (!s_prev_running && s_frame_mutex) {
        s_prev_exited = false;
        s_prev_running = true;
        BaseType_t t = xTaskCreatePinnedToCoreWithCaps(prev_task, "cam_prev",
                                                       PREV_TASK_STACK, NULL, 4,
                                                       &s_prev_task, 0,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (t != pdPASS) {
            s_prev_running = false;
            s_prev_exited = false;
            ESP_LOGE(TAG, "failed to create preview task");
        }
    }

    /* This page now holds the "camera page visible" token; the activity manager
     * starts the DVP pipeline on demand. The web server itself stays up (its
     * camera need is leased per web request), so on an idle face page the
     * camera is fully off — no streaming bandwidth, no DVP/JPEG CPU. */
    app_activity_camera_acquire(APP_ACT_CAMERA_PAGE);

    /* Subscribe to raw UYVY frames for the on-screen PPA preview. */
    app_camera_register_yuv_cb(on_yuv, NULL);

    /* Enable the offline vision analyzer (motion / wave / activity). It reads
     * the same RGB565 preview the PPA produces, so no extra capture cost. */
    app_vision_lite_init();
    app_vision_lite_set_enabled(true);

    char ip_str[16] = "0.0.0.0";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
            esp_ip4addr_ntoa(&ip.ip, ip_str, sizeof(ip_str));
        }
    }
    /* Show the web access address on its own line (vision status goes to
     * s_status and is refreshed by on_tick). The address is re-fetched on
     * every tick so a DHCP lease change is reflected automatically. */
    if (s_ip_label) {
        char buf[96];
        snprintf(buf, sizeof(buf), "Web: http://%s/", ip_str[0] ? ip_str : "<无网络>");
        lv_label_set_text(s_ip_label, buf);
    }
    if (s_status) {
        lv_label_set_text(s_status, "预览中 · 浏览器打开上方地址");
    }
    return ESP_OK;
}

esp_err_t page_camera_on_exit(void)
{
    app_vision_lite_set_enabled(false);
    app_camera_register_yuv_cb(NULL, NULL);
    s_prev_running = false;
    /* NULL the canvas under the LVGL lock so a winding-down prev task can't
     * blit into a freed object. The buffer is kept for the next on_enter. */
    if (lvgl_port_lock(1000)) {
        s_canvas = NULL;
        lvgl_port_unlock();
    }
    /* Release our camera token. The activity manager keeps the pipeline warm
     * for a short linger, then turns the camera fully off if nothing else (a
     * web viewer, guard mode, recording) still needs it. */
    app_activity_camera_release(APP_ACT_CAMERA_PAGE);
    return ESP_OK;
}

void page_camera_on_tick(uint32_t ms)
{
    (void)ms;
    /* Refresh the vision status line (motion / wave / activity) a few times a
     * second. The analyzer runs on the preview task; we only read its snapshot
     * here under the LVGL lock (page_tick already runs under it). */
    static uint32_t s_last_refresh = 0;
    if (ms - s_last_refresh < 300) {
        return;
    }
    s_last_refresh = ms;

    /* Refresh the web IP (DHCP can change after reconnect). */
    if (s_ip_label) {
        char ip_str[16] = "0.0.0.0";
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t ip;
            if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
                esp_ip4addr_ntoa(&ip.ip, ip_str, sizeof(ip_str));
            }
        }
        static char s_last_ip[16] = {0};
        if (strcmp(s_last_ip, ip_str) != 0) {
            strncpy(s_last_ip, ip_str, sizeof(s_last_ip) - 1);
            char buf[96];
            snprintf(buf, sizeof(buf), "Web: http://%s/", ip_str[0] ? ip_str : "<无网络>");
            lv_label_set_text(s_ip_label, buf);
        }
    }

    if (s_status == NULL || !app_vision_lite_is_enabled()) {
        return;
    }
    app_vision_result_t v = app_vision_lite_get_result();
    const char *act = "—";
    if (v.activity == VISION_ACTIVITY_NONE) act = "无活动";
    else if (v.activity == VISION_ACTIVITY_QUIET) act = "轻微";
    else act = "活跃";
    char buf[96];
    snprintf(buf, sizeof(buf), "运动:%d%% %s %s", v.motion_ratio, act,
             v.wave ? "  👋挥手" : "");
    lv_label_set_text(s_status, buf);
}

const ui_page_t page_camera = {
    .id = PAGE_CAMERA,
    .name = "camera",
    .on_enter = page_camera_on_enter,
    .on_exit = page_camera_on_exit,
    .on_tick = page_camera_on_tick,
};
