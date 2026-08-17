/*
 * app_camera.c — SC101IOT DVP capture + S31 hardware JPEG encode.
 *
 * Pipeline (single reference-counted instance, shared by MCP take_photo + MJPEG
 * preview + camera page):
 *
 *   /dev/video2  (DVP, YUV422 UYVY 1280x720)  --capture stream-->  YUV frame
 *        |
 *        v  (copy into our own PSRAM MMAP buffer)
 *   /dev/video10 (HW JPEG M2M)  --capture stream-->  JPEG frame
 *
 * A background task streams the DVP, copies each YUV frame into the JPEG
 * encoder's OWN MMAP input buffer (the M2M framework only accepts an MMAP input
 * for the encoder; feeding the DVP buffer directly as USERPTR fails the
 * alignment/PSRAM check and silently drops every frame), invokes the registered
 * frame callback (preview) and keeps the latest JPEG as a snapshot for one-shot
 * capture. Only the task touches the V4L2 queues, so callers are race-free.
 */
#include "app_camera.h"

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "esp_video_device.h"

static const char *TAG = "app_camera";

#define CAM_WIDTH   1280
#define CAM_HEIGHT  720
#define CAM_FORMAT  V4L2_PIX_FMT_UYVY
#define CAP_BUFS    2
#define JPEG_CAP_BUFS 1   /* JPEG encoder output (compressed) MMAP buffers (1 is enough for still/preview) */
#define JPEG_IN_BUFS  1   /* JPEG encoder input (YUV) MMAP buffer we own      */

/* ---- shared instance state (guarded by s_mutex) ---- */
static void camera_hw_deinit(void);   /* forward decl (camera_hw_init fail path) */
static SemaphoreHandle_t s_mutex = NULL;
static int               s_refcount = 0;
static bool              s_running = false;
static TaskHandle_t      s_task = NULL;

static int  s_fd_dvp = -1;
static int  s_fd_jpeg = -1;

/* DVP capture MMAP buffers */
static uint8_t *s_dvp_buf[CAP_BUFS];
static uint32_t s_dvp_len[CAP_BUFS];

/* JPEG encoder capture (output JPEG) MMAP buffers */
static uint8_t *s_jpeg_buf[JPEG_CAP_BUFS];
static uint32_t s_jpeg_len[JPEG_CAP_BUFS];

/* JPEG encoder input: our own PSRAM MMAP buffer (YUV source for the HW encoder) */
static uint8_t *s_jpeg_in_buf = NULL;
static uint32_t s_jpeg_in_len = 0;

/* Latest encoded JPEG snapshot (PSRAM, owned by us) */
static uint8_t *s_latest = NULL;
static size_t   s_latest_len = 0;
static uint32_t s_frame_count = 0;

/* Per-frame JPEG callbacks. Multiple consumers register here: the MJPEG HTTP
 * preview (/stream) and the on-screen camera page each have their own callback.
 * A single slot used to make the last register win, silently killing the other
 * consumer (the /stream served stale frames while the camera page was open). */
#define APP_CAMERA_MAX_FRAME_CB 4
typedef struct {
    app_camera_frame_cb_t cb;
    void *ctx;
} cam_cb_slot_t;
static cam_cb_slot_t s_cbs[APP_CAMERA_MAX_FRAME_CB];

/* Raw UYVY callback (on-screen PPA preview). Single slot; page registers it on
 * enter and clears it on exit. Runs in the streaming task context. */
static app_camera_yuv_cb_t s_yuv_cb = NULL;
static void *s_yuv_ctx = NULL;

/* ---------- low level V4L2 helpers ---------- */

static esp_err_t set_fmt(int fd, int type, uint32_t pix_fmt, uint32_t w, uint32_t h)
{
    struct v4l2_format fmt = {
        .type = type,
        .fmt.pix = {
            .width = w,
            .height = h,
            .pixelformat = pix_fmt,
        },
    };
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT failed (type=%d fmt=0x%x)", type, pix_fmt);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t set_jpeg_quality(int fd, int quality)
{
    struct v4l2_ext_control ctrl = {
        .id = V4L2_CID_JPEG_COMPRESSION_QUALITY,
        .value = quality,
    };
    struct v4l2_ext_controls ctrls = {
        .ctrl_class = V4L2_CTRL_CLASS_JPEG,
        .count = 1,
        .controls = &ctrl,
    };
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) != 0) {
        ESP_LOGW(TAG, "set JPEG quality failed (non-fatal)");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---------- streaming task ---------- */

static void camera_task(void *arg)
{
    ESP_LOGI(TAG, "streaming task started");
    uint32_t frames = 0;

    while (s_running) {
        /* 1. dequeue a YUV frame from the DVP capture */
        struct v4l2_buffer dvp_buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (ioctl(s_fd_dvp, VIDIOC_DQBUF, &dvp_buf) != 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!(dvp_buf.flags & V4L2_BUF_FLAG_DONE) || dvp_buf.bytesused == 0) {
            ioctl(s_fd_dvp, VIDIOC_QBUF, &dvp_buf);
            continue;
        }

        /* 1b. Hand the raw UYVY frame to the on-screen PPA preview callback
         *     (data valid only now). This runs entirely in hardware and never
         *     touches the software JPEG decoder. */
        {
            app_camera_yuv_cb_t yuv_cb;
            void *yuv_ctx;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            yuv_cb = s_yuv_cb;
            yuv_ctx = s_yuv_ctx;
            xSemaphoreGive(s_mutex);
            if (yuv_cb) {
                yuv_cb(s_dvp_buf[dvp_buf.index], CAM_WIDTH, CAM_HEIGHT, yuv_ctx);
            }
        }

        /* 2. copy the YUV frame into the encoder's own PSRAM MMAP input buffer.
         *    (USERPTR-fed DVP buffers are rejected by the M2M framework's
         *    PSRAM/alignment check, so we always copy.) */
        if (dvp_buf.bytesused > s_jpeg_in_len) {
            ESP_LOGW(TAG, "dvp frame %u B > encoder in buf %u B, dropped",
                     dvp_buf.bytesused, s_jpeg_in_len);
            ioctl(s_fd_dvp, VIDIOC_QBUF, &dvp_buf);
            continue;
        }
        memcpy(s_jpeg_in_buf, s_dvp_buf[dvp_buf.index], dvp_buf.bytesused);

        /* 3. queue the encoder input (MMAP) */
        struct v4l2_buffer in_buf = {
            .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
            .memory = V4L2_MEMORY_MMAP,
            .index = 0,
        };
        if (ioctl(s_fd_jpeg, VIDIOC_QBUF, &in_buf) != 0) {
            ESP_LOGE(TAG, "QBUF jpeg input failed");
            ioctl(s_fd_dvp, VIDIOC_QBUF, &dvp_buf);
            continue;
        }

        /* 4. dequeue the encoded JPEG (capture) and recycle the input */
        struct v4l2_buffer jpeg_buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        struct v4l2_buffer in_done = {
            .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (ioctl(s_fd_jpeg, VIDIOC_DQBUF, &jpeg_buf) != 0 ||
            ioctl(s_fd_jpeg, VIDIOC_DQBUF, &in_done) != 0) {
            ESP_LOGE(TAG, "DQBUF jpeg failed");
            ioctl(s_fd_dvp, VIDIOC_QBUF, &dvp_buf);
            continue;
        }

        if (jpeg_buf.flags & V4L2_BUF_FLAG_DONE && jpeg_buf.bytesused > 0) {
            uint8_t *jpeg = s_jpeg_buf[jpeg_buf.index];
            size_t jlen = jpeg_buf.bytesused;

            /* Invoke all preview callbacks (data valid only now). Snapshot the
             * slot array under the mutex so a concurrent register/unregister
             * cannot tear the array mid-iteration, then call with the mutex
             * released (callbacks do their own locking). */
            cam_cb_slot_t slots[APP_CAMERA_MAX_FRAME_CB];
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            memcpy(slots, s_cbs, sizeof(s_cbs));
            xSemaphoreGive(s_mutex);
            for (int i = 0; i < APP_CAMERA_MAX_FRAME_CB; i++) {
                if (slots[i].cb) {
                    slots[i].cb(jpeg, jlen, slots[i].ctx);
                }
            }

            /* keep a snapshot copy for one-shot capture */
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (s_latest && s_latest_len >= jlen) {
                memcpy(s_latest, jpeg, jlen);
                s_latest_len = jlen;
            } else {
                uint8_t *nb = heap_caps_malloc(jlen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (nb) {
                    if (s_latest) heap_caps_free(s_latest);
                    s_latest = nb;
                    memcpy(s_latest, jpeg, jlen);
                    s_latest_len = jlen;
                }
            }
            s_frame_count++;
            xSemaphoreGive(s_mutex);

            if (frames == 0) {
                ESP_LOGI(TAG, "first JPEG frame encoded: %u bytes", (unsigned)jlen);
            }
            frames++;
        }

        /* 5. recycle both buffers */
        ioctl(s_fd_jpeg, VIDIOC_QBUF, &jpeg_buf);
        ioctl(s_fd_dvp, VIDIOC_QBUF, &dvp_buf);

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ESP_LOGI(TAG, "streaming task stopped");
    vTaskDelete(NULL);
}

/* ---------- init / deinit ---------- */

static esp_err_t camera_hw_init(void)
{
    /* DVP capture device */
    s_fd_dvp = open(ESP_VIDEO_DVP_DEVICE_NAME, O_RDWR);
    if (s_fd_dvp < 0) {
        ESP_LOGE(TAG, "open %s failed", ESP_VIDEO_DVP_DEVICE_NAME);
        return ESP_ERR_NOT_FOUND;
    }
    if (set_fmt(s_fd_dvp, V4L2_BUF_TYPE_VIDEO_CAPTURE, CAM_FORMAT, CAM_WIDTH, CAM_HEIGHT) != ESP_OK) {
        goto fail;
    }

    struct v4l2_requestbuffers req = {
        .count = CAP_BUFS,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_fd_dvp, VIDIOC_REQBUFS, &req) != 0 || req.count == 0) {
        ESP_LOGE(TAG, "REQBUFS dvp failed");
        goto fail;
    }
    for (int i = 0; i < (int)req.count; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = (uint32_t)i,
        };
        if (ioctl(s_fd_dvp, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QUERYBUF dvp %d failed", i);
            goto fail;
        }
        s_dvp_len[i] = buf.length;
        s_dvp_buf[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_fd_dvp, buf.m.offset);
        if (s_dvp_buf[i] == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap dvp %d failed", i);
            goto fail;
        }
        if (ioctl(s_fd_dvp, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QBUF dvp %d failed", i);
            goto fail;
        }
    }
    int cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_fd_dvp, VIDIOC_STREAMON, &cap_type) != 0) {
        ESP_LOGE(TAG, "STREAMON dvp failed");
        goto fail;
    }
    ESP_LOGI(TAG, "dvp ready: %u bufs, %u bytes each", req.count, s_dvp_len[0]);

    /* JPEG encoder device */
    s_fd_jpeg = open(ESP_VIDEO_JPEG_DEVICE_NAME, O_RDWR);
    if (s_fd_jpeg < 0) {
        ESP_LOGE(TAG, "open %s failed", ESP_VIDEO_JPEG_DEVICE_NAME);
        goto fail;
    }
    if (set_fmt(s_fd_jpeg, V4L2_BUF_TYPE_VIDEO_OUTPUT, CAM_FORMAT, CAM_WIDTH, CAM_HEIGHT) != ESP_OK) {
        goto fail;
    }
    if (set_fmt(s_fd_jpeg, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_PIX_FMT_JPEG, CAM_WIDTH, CAM_HEIGHT) != ESP_OK) {
        goto fail;
    }
    set_jpeg_quality(s_fd_jpeg, APP_CAMERA_JPEG_QUALITY);

    /* encoder INPUT: our own MMAP (PSRAM) buffer we copy the YUV frame into */
    struct v4l2_requestbuffers oreq = {
        .count = JPEG_IN_BUFS,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_fd_jpeg, VIDIOC_REQBUFS, &oreq) != 0 || oreq.count == 0) {
        ESP_LOGE(TAG, "REQBUFS jpeg output failed");
        goto fail;
    }
    struct v4l2_buffer obuf = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_MMAP,
        .index = 0,
    };
    if (ioctl(s_fd_jpeg, VIDIOC_QUERYBUF, &obuf) != 0) {
        ESP_LOGE(TAG, "QUERYBUF jpeg output failed");
        goto fail;
    }
    s_jpeg_in_len = obuf.length;
    s_jpeg_in_buf = mmap(NULL, obuf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_fd_jpeg, obuf.m.offset);
    if (s_jpeg_in_buf == MAP_FAILED) {
        ESP_LOGE(TAG, "mmap jpeg output failed");
        goto fail;
    }
    ESP_LOGI(TAG, "jpeg input buf %u bytes @ %p", s_jpeg_in_len, s_jpeg_in_buf);

    /* encoder OUTPUT (JPEG): MMAP capture buffers, queued now */
    struct v4l2_requestbuffers creq = {
        .count = JPEG_CAP_BUFS,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_fd_jpeg, VIDIOC_REQBUFS, &creq) != 0 || creq.count == 0) {
        ESP_LOGE(TAG, "REQBUFS jpeg capture failed");
        goto fail;
    }
    for (int i = 0; i < (int)creq.count; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = (uint32_t)i,
        };
        if (ioctl(s_fd_jpeg, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QUERYBUF jpeg %d failed", i);
            goto fail;
        }
        s_jpeg_len[i] = buf.length;
        s_jpeg_buf[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_fd_jpeg, buf.m.offset);
        if (s_jpeg_buf[i] == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap jpeg %d failed", i);
            goto fail;
        }
        if (ioctl(s_fd_jpeg, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QBUF jpeg %d failed", i);
            goto fail;
        }
    }

    int out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    int capj_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_fd_jpeg, VIDIOC_STREAMON, &out_type) != 0 ||
        ioctl(s_fd_jpeg, VIDIOC_STREAMON, &capj_type) != 0) {
        ESP_LOGE(TAG, "STREAMON jpeg failed");
        goto fail;
    }

    ESP_LOGI(TAG, "camera hw init ok (%dx%d YUV422 -> JPEG)", CAM_WIDTH, CAM_HEIGHT);
    return ESP_OK;

fail:
    /* CRITICAL: a partial init may have already opened the DVP, mmap'd its
     * buffers and started the stream. Merely closing the fd left the esp_video
     * driver with active DMA pointers/stale MMAPs; when the activity manager
     * retried camera start a few seconds later the driver state was corrupted
     * and the board crashed with a Load access fault. Always run the FULL
     * teardown (STREAMOFF + munmap + close) so the next start is clean. */
    camera_hw_deinit();
    return ESP_FAIL;
}

static void camera_hw_deinit(void)
{
    int t;
    if (s_fd_dvp >= 0) {
        t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s_fd_dvp, VIDIOC_STREAMOFF, &t);
        for (int i = 0; i < CAP_BUFS; i++) {
            if (s_dvp_buf[i] && s_dvp_buf[i] != MAP_FAILED) munmap(s_dvp_buf[i], s_dvp_len[i]);
            s_dvp_buf[i] = NULL;
        }
        close(s_fd_dvp); s_fd_dvp = -1;
    }
    if (s_fd_jpeg >= 0) {
        t = V4L2_BUF_TYPE_VIDEO_OUTPUT; ioctl(s_fd_jpeg, VIDIOC_STREAMOFF, &t);
        t = V4L2_BUF_TYPE_VIDEO_CAPTURE; ioctl(s_fd_jpeg, VIDIOC_STREAMOFF, &t);
        if (s_jpeg_in_buf && s_jpeg_in_buf != MAP_FAILED) { munmap(s_jpeg_in_buf, s_jpeg_in_len); s_jpeg_in_buf = NULL; }
        for (int i = 0; i < JPEG_CAP_BUFS; i++) {
            if (s_jpeg_buf[i] && s_jpeg_buf[i] != MAP_FAILED) munmap(s_jpeg_buf[i], s_jpeg_len[i]);
            s_jpeg_buf[i] = NULL;
        }
        close(s_fd_jpeg); s_fd_jpeg = -1;
    }
    if (s_latest) { heap_caps_free(s_latest); s_latest = NULL; }
    s_latest_len = 0;
    s_frame_count = 0;
}

/* ---------- public API ---------- */

/* Lazily create the shared mutex so any public entry point can take it safely
 * even before app_camera_start() has run (e.g. app_camera_is_running() called
 * from app_camera_capture_jpeg before the streaming task exists). */
static void ensure_mutex(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
}

esp_err_t app_camera_start(void)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_running) {
        s_refcount++;
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(s_mutex);

    if (camera_hw_init() != ESP_OK) {
        return ESP_FAIL;
    }
    /* Prepare the on-screen PPA preview client once (it stays registered for
     * the app lifetime; repeated register/unregister churned scarce internal
     * RAM). Failure is non-fatal — the MJPEG /stream still works. */
    {
        extern esp_err_t app_camera_preview_init(void);
        esp_err_t pre = app_camera_preview_init();
        if (pre != ESP_OK) {
            ESP_LOGW(TAG, "PPA preview init failed (continuing): %s",
                     esp_err_to_name(pre));
        }
    }
    s_running = true;
    s_refcount = 1;
    /* The streaming task drives the DVP + HW-JPEG M2M driver ioctl loop; the
     * driver stacks are deep enough that 6 KB overflowed and smashed the return
     * address (MEPC=0x0). 16 KB is the safe depth. Internal RAM on this board is
     * too tight to allocate that stack once the camera's ~5.5 MB of PSRAM
     * buffers are live (xTaskCreatePinnedToCore from internal RAM failed with
     * pdFAIL at camera-page entry, starving even the LCD TX buffer), so the
     * stack is explicitly allocated from PSRAM via the WithCaps variant. */
    ESP_LOGI(TAG, "free int=%u spiram=%u before task create",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    BaseType_t r = xTaskCreatePinnedToCoreWithCaps(camera_task, "cam_task", 16 * 1024, NULL,
                                                   5, &s_task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "create task failed (stack from SPIRAM)");
        s_running = false;
        camera_hw_deinit();
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t app_camera_stop(void)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_running) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    if (--s_refcount > 0) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    s_running = false;
    xSemaphoreGive(s_mutex);

    /* task deletes itself on next loop iteration */
    int guard = 0;
    while (s_task != NULL && guard++ < 100) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_task = NULL;
    camera_hw_deinit();
    return ESP_OK;
}

esp_err_t app_camera_capture_jpeg(uint8_t **out_buf, size_t *out_len)
{
    if (out_buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_mutex();

    /* IMPORTANT: do NOT auto-start/stop the camera here. Auto-starting from a
     * one-shot capture raced with the activity manager's reconcile task and
     * caused a start/stop loop (streaming task stopped -> REQBUFS dvp failed
     * -> retry -> ...), which left the DVP driver in a broken state and the
     * camera page crashed. The camera is now started/stopped ONLY by the
     * activity manager (app_activity). If the pipeline isn't running, the
     * capture simply fails fast — the caller (MCP take_photo / web snapshot)
     * shows "camera not ready". */
    if (!app_camera_is_running()) {
        ESP_LOGW(TAG, "capture: camera not running (managed by activity manager)");
        return ESP_ERR_INVALID_STATE;
    }

    /* wait up to ~3s for at least one encoded frame */
    uint32_t start = s_frame_count;
    int waited = 0;
    while (s_frame_count == start && waited < 300) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited++;
    }
    if (s_frame_count == start) {
        ESP_LOGE(TAG, "capture: no frame produced in %d ms (dvp streaming?)", waited * 10);
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t ret = ESP_FAIL;
    if (s_latest && s_latest_len > 0) {
        uint8_t *copy = heap_caps_malloc(s_latest_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (copy) {
            memcpy(copy, s_latest, s_latest_len);
            *out_buf = copy;
            *out_len = s_latest_len;
            ret = ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ret;
}

void app_camera_release_jpeg(uint8_t *buf)
{
    if (buf) {
        heap_caps_free(buf);
    }
}

esp_err_t app_camera_register_frame_cb(app_camera_frame_cb_t cb, void *ctx)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (cb == NULL) {
        /* NULL clears all consumers (stop path). */
        memset(s_cbs, 0, sizeof(s_cbs));
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    /* Re-registering the same callback updates its context in place. */
    for (int i = 0; i < APP_CAMERA_MAX_FRAME_CB; i++) {
        if (s_cbs[i].cb == cb) {
            s_cbs[i].ctx = ctx;
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    for (int i = 0; i < APP_CAMERA_MAX_FRAME_CB; i++) {
        if (s_cbs[i].cb == NULL) {
            s_cbs[i].cb = cb;
            s_cbs[i].ctx = ctx;
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    ESP_LOGW(TAG, "frame callback slot full (%d)", APP_CAMERA_MAX_FRAME_CB);
    return ESP_FAIL;
}

esp_err_t app_camera_unregister_frame_cb(app_camera_frame_cb_t cb)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool found = false;
    for (int i = 0; i < APP_CAMERA_MAX_FRAME_CB; i++) {
        if (s_cbs[i].cb == cb) {
            s_cbs[i].cb = NULL;
            s_cbs[i].ctx = NULL;
            found = true;
        }
    }
    xSemaphoreGive(s_mutex);
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t app_camera_register_yuv_cb(app_camera_yuv_cb_t cb, void *ctx)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_yuv_cb = cb;
    s_yuv_ctx = ctx;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

bool app_camera_is_running(void)
{
    bool r;
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    r = s_running;
    xSemaphoreGive(s_mutex);
    return r;
}
