/*
 * http_preview.c — MJPEG preview server (see http_preview.h).
 */
#include "http_preview.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_http_server.h"

#include "app_camera.h"
#include "app_activity.h"
#include "app_recorder.h"
#include "app_sdcard.h"
#include "app_music_player.h"
#include "page_music.h"
#include <dirent.h>

static const char *TAG = "http_preview";

/* Single httpd server on port 80. A long-lived MJPEG /stream handler would
 * block the one httpd task and starve / and /snapshot, so the viewer page does
 * NOT use /stream: it polls /snapshot with a short request every ~180 ms
 * (~5-6 fps), which is snappy and lets the same httpd task serve everything.
 * /stream is still available for clients that want it (one viewer at a time). */
static httpd_handle_t s_server = NULL;          /* port 80: / + /snapshot + /stream */
static SemaphoreHandle_t s_mutex = NULL;
static uint8_t *s_latest = NULL;
static size_t s_latest_len = 0;
static bool s_running = false;

/* Latest frame fed by app_camera's per-frame callback */
static void on_frame(const uint8_t *jpeg, size_t len, void *ctx)
{
    if (jpeg == NULL || len == 0) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_latest && s_latest_len >= len) {
        memcpy(s_latest, jpeg, len);
        s_latest_len = len;
    } else {
        uint8_t *nb = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (nb) {
            if (s_latest) heap_caps_free(s_latest);
            s_latest = nb;
            memcpy(s_latest, jpeg, len);
            s_latest_len = len;
        }
    }
    xSemaphoreGive(s_mutex);
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    char header[160];

    /* CRITICAL: httpd_resp_set_type() stores the POINTER verbatim (no copy).
     * Passing a stack buffer corrupts ra->content_type the moment this function
     * returns. Use a static const so it lives for the whole stream. */
    static const char *ct_multipart =
        "multipart/x-mixed-replace;boundary=vocat-mjpeg-boundary";

    if (httpd_resp_set_type(req, ct_multipart) != ESP_OK) {
        return ESP_FAIL;
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    uint8_t *frame = NULL;
    size_t cap = 0;
    while (s_running) {
        /* Keep the web-viewer lease alive while this persistent stream is open
         * so the activity manager keeps the camera running for us. */
        app_activity_web_touch();
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        size_t len = s_latest_len;
        bool has = (s_latest != NULL && len > 0);
        xSemaphoreGive(s_mutex);
        if (!has) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Reuse one PSRAM buffer instead of allocating+freeing ~150 KB every
         * frame (malloc churn + fragmentation made the preview stutter). */
        if (len > cap) {
            if (frame) heap_caps_free(frame);
            frame = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            cap = frame ? len : 0;
        }
        if (frame == NULL) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        memcpy(frame, s_latest, s_latest_len);
        size_t flen = s_latest_len;
        xSemaphoreGive(s_mutex);

        int n = snprintf(header, sizeof(header),
                         "\r\n--vocat-mjpeg-boundary\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                         (unsigned)flen);
        if (httpd_resp_send_chunk(req, header, n) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)frame, flen) != ESP_OK) {
            break;   /* client closed or socket error */
        }
        /* ~18fps; the browser's <img> repaints each arrived frame. Keeping the
         * yield prevents the task from starving other work while still being
         * fluid enough for a live preview. */
        vTaskDelay(pdMS_TO_TICKS(55));
    }
    if (frame) {
        heap_caps_free(frame);
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t snapshot_handler(httpd_req_t *req)
{
    app_activity_web_touch();   /* keep camera alive for polling viewers */
    uint8_t *buf = NULL;
    size_t len = 0;
    esp_err_t r = app_camera_capture_jpeg(&buf, &len);
    if (r != ESP_OK || buf == NULL) {
        httpd_resp_set_status(req, "503");
        httpd_resp_send(req, "camera not ready", 17);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Length", NULL);
    char clen[16];
    snprintf(clen, sizeof(clen), "%u", (unsigned)len);
    httpd_resp_set_hdr(req, "Content-Length", clen);
    httpd_resp_send(req, (const char *)buf, len);
    app_camera_release_jpeg(buf);
    return ESP_OK;
}

/* Root page: a polished MJPEG viewer with a one-tap photo button. The <img>
 * points at the dedicated stream server (:81/stream) so the page stays
 * responsive. Photo is taken client-side with fetch() to /snapshot and shown as
 * an overlay thumbnail with a download link. */
static esp_err_t root_handler(httpd_req_t *req)
{
    app_activity_web_touch();   /* opening the viewer page starts the camera */
    /* static: the page is large (UTF-8 + CSS/JS) and must NOT live on the httpd
     * task's stack — a stack array overflows the ~4 KB httpd task stack and the
     * handler dies silently (blank page) while /snapshot and /stream keep working. */
    static const char html[] =
        "<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,user-scalable=no\">"
        "<title>喵伴 · 摄像头</title>"
        "<style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{background:radial-gradient(circle at 50% 0%,#1c2340,#0b0e1a 70%);"
        "min-height:100vh;display:flex;flex-direction:column;align-items:center;"
        "color:#fff;font-family:'Segoe UI',system-ui,sans-serif;padding:16px}"
        ".app{width:100%;max-width:560px;display:flex;flex-direction:column;gap:14px;align-items:center}"
        ".title{font-size:20px;font-weight:600;letter-spacing:1px;margin-top:4px}"
        ".title .dot{color:#2ecc71}"
        ".stage{position:relative;width:100%;aspect-ratio:1;max-width:420px;"
        "border-radius:24px;overflow:hidden;background:#000;"
        "box-shadow:0 12px 40px rgba(0,0,0,.5);border:1px solid #2a3355}"
        "#stream{width:100%;height:100%;object-fit:cover;display:block}"
        ".badge{position:absolute;top:12px;left:12px;background:rgba(46,204,113,.92);"
        "color:#04120a;font-size:12px;font-weight:600;padding:4px 10px;border-radius:999px}"
        ".controls{display:flex;gap:12px;align-items:center;flex-wrap:wrap;justify-content:center}"
        "button{background:linear-gradient(135deg,#4c8cff,#3a6fd0);border:0;color:#fff;"
        "font-size:15px;font-weight:600;padding:12px 20px;border-radius:14px;cursor:pointer;"
        "box-shadow:0 6px 18px rgba(76,140,255,.35)}"
        "button:active{transform:scale(.95)}button.rec{background:linear-gradient(135deg,#ff5e5e,#d03a3a)}"
        "a{color:#8fa8ff;font-size:13px;text-decoration:none;margin-top:2px}"
        "select{background:#1c2340;color:#fff;border:1px solid #2a3355;border-radius:10px;padding:8px 12px;font-size:14px}"
        ".filebtn{background:linear-gradient(135deg,#4c8cff,#3a6fd0);border:0;color:#fff;"
        "font-size:15px;font-weight:600;padding:12px 20px;border-radius:14px;cursor:pointer;"
        "box-shadow:0 6px 18px rgba(76,140,255,.35);display:inline-flex;align-items:center}"
        "#recStat{font-size:13px;color:#ffb86c}"
        ".snap{position:absolute;inset:0;display:none;background:rgba(0,0,0,.6);"
        "align-items:center;justify-content:center;flex-direction:column;gap:12px}"
        ".snap.show{display:flex}.snap img{width:60%;border-radius:12px;box-shadow:0 8px 30px #000}"
        ".snap .close{position:absolute;top:14px;right:18px;font-size:26px;cursor:pointer;color:#fff}"
        ".snap a{background:#2ecc71;color:#04120a;font-weight:700;padding:10px 20px;border-radius:10px}"
        "</style></head><body><div class=\"app\">"
        "<div class=\"title\">喵伴 <span class=\"dot\">●</span> 摄像头</div>"
        "<div class=\"stage\">"
        "<img id=\"stream\" alt=\"实时画面\">"
        "<span class=\"badge\">● 直播中</span>"
        "<div id=\"snap\" class=\"snap\"><span class=\"close\" onclick=\"closeSnap()\">&times;</span>"
        "<img id=\"snapImg\" alt=\"照片\"><a id=\"snapDl\" download=\"maoban.jpg\" href=\"#\">下载照片</a></div>"
        "</div>"
        "<div class=\"controls\"><button onclick=\"takePhoto()\">📷 拍照</button>"
        "<a href=\"/snapshot\" target=\"_blank\">原图快照</a></div>"
        "<div class=\"controls\"><button class=\"rec\" onclick=\"startRec()\">⏺ 录制到SD</button>"
        "<button onclick=\"stopRec()\">⏹ 停止</button><span id=\"recStat\">未录制</span></div>"
        "<div class=\"controls\"><select id=\"recList\"></select>"
        "<button onclick=\"playRec()\">▶ 回放</button><a href=\"/recordings\" target=\"_blank\">列表</a></div>"
        "<div class=\"controls\"><label class=\"filebtn\">📤 上传到音箱"
        "<input id=\"file\" type=\"file\" accept=\"audio/*\" onchange=\"onFile(this)\" hidden></label>"
        "<button onclick=\"sendPlay()\">▶ 播放</button>"
        "<button class=\"rec\" onclick=\"stopPlay()\">⏹ 停止</button></div>"
        "<div class=\"controls\"><span id=\"upStat\" style=\"font-size:13px;color:#ffb86c\">选文件后自动上传并播放</span>"
        "<a href=\"/settings\" style=\"margin-left:auto;background:#222a44;padding:10px 16px;border-radius:12px\">⚙ 音乐设置</a></div>"
        "<script>"
        "const img=document.getElementById('stream');let inflight=false,pre=null;"
        "function loop(){if(inflight)return;inflight=true;"
        "pre=new Image();pre.onload=function(){img.src=pre.src;inflight=false;setTimeout(loop,40)};"
        "pre.onerror=function(){inflight=false;setTimeout(loop,200)};"
        "pre.src='/snapshot?v='+new Date().getTime();}setTimeout(loop,100);"
        "async function takePhoto(){const b=document.querySelector('.controls button');b.disabled=true;"
        "try{const r=await fetch('/snapshot');const blob=await r.blob();"
        "const u=URL.createObjectURL(blob);document.getElementById('snapImg').src=u;"
        "document.getElementById('snapDl').href=u;"
        "document.getElementById('snap').classList.add('show')}catch(e){alert('拍照失败')}"
        "finally{b.disabled=false}}"
        "function closeSnap(){document.getElementById('snap').classList.remove('show')}"
        "async function refreshRecList(){try{const r=await fetch('/recordings');const j=await r.json();"
        "const sel=document.getElementById('recList');sel.innerHTML='';"
        "j.forEach(x=>{const o=document.createElement('option');o.value=x.name;"
        "o.textContent=x.name+' ('+x.frames+'帧)';sel.appendChild(o)})}catch(e){}}"
        "async function startRec(){try{const r=await fetch('/rec/start?fps=5');const j=await r.json();"
        "document.getElementById('recStat').textContent=j.ok?'录制中…':'启动失败';refreshRecList()}"
        "catch(e){document.getElementById('recStat').textContent='启动失败'}}"
        "async function stopRec(){try{await fetch('/rec/stop');"
        "document.getElementById('recStat').textContent='已停止';refreshRecList()}catch(e){}}"
        "function playRec(){const n=document.getElementById('recList').value;"
        "if(n)window.open('/play/'+n,'_blank');}"
        "let curUrl='';"
        "function onFile(input){const f=input.files[0];if(!f)return;const st=document.getElementById('upStat');"
        "st.textContent='上传中… '+f.name;const fd=f;"
        "fetch('/upload?name='+encodeURIComponent(f.name),{method:'POST',body:fd})"
        ".then(r=>r.json()).then(j=>{if(j.ok){curUrl=j.url;st.textContent='已上传: '+f.name;"
        "return fetch('/api/play?url='+encodeURIComponent(j.url));}else{st.textContent='上传失败';}})"
        ".then(r=>{if(r)return r.json();}).then(j=>{if(j)st.textContent=j.ok?'🎵 播放中: '+f.name:'播放失败';})"
        ".catch(e=>{st.textContent='上传失败';});}"
        "async function sendPlay(){if(curUrl){await fetch('/api/play?url='+encodeURIComponent(curUrl));}}"
        "async function stopPlay(){await fetch('/api/stop');}"
        "setTimeout(refreshRecList,600);"
        "</script></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

/* ----- SD recording control + playback ----- */

static esp_err_t rec_start_handler(httpd_req_t *req)
{
    int fps = 5;
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0) {
        char *q = malloc(qlen + 1);
        if (q) {
            if (httpd_req_get_url_query_str(req, q, qlen + 1) == ESP_OK) {
                char val[8] = {0};
                if (httpd_query_key_value(q, "fps", val, sizeof(val)) == ESP_OK) {
                    int v = atoi(val);
                    if (v > 0) fps = v;
                }
            }
            free(q);
        }
    }
    esp_err_t r = app_recorder_start(fps);
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "{\"ok\":%s}", (r == ESP_OK) ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static esp_err_t rec_stop_handler(httpd_req_t *req)
{
    esp_err_t r = app_recorder_stop();
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "{\"ok\":%s}", (r == ESP_OK) ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static esp_err_t rec_status_handler(httpd_req_t *req)
{
    char buf[96];
    int n = snprintf(buf, sizeof(buf), "{\"recording\":%s,\"frames\":%d}",
                     app_recorder_is_recording() ? "true" : "false",
                     app_recorder_frame_count());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static esp_err_t recordings_list_handler(httpd_req_t *req)
{
    char out[2048];
    int off = 0;
    off += snprintf(out + off, sizeof(out) - off, "[");
    if (app_sdcard_mounted()) {
        DIR *d = opendir(APP_SDCARD_RECORD_DIR);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL && off < (int)sizeof(out) - 256) {
                size_t l = strlen(ent->d_name);
                if (l < 5 || strcmp(ent->d_name + l - 5, ".json") != 0) {
                    continue;
                }
                char jp[320];
                snprintf(jp, sizeof(jp), "%s/%s", APP_SDCARD_RECORD_DIR, ent->d_name);
                FILE *jf = fopen(jp, "r");
                if (jf) {
                    char tmp[512];
                    int rd = fread(tmp, 1, sizeof(tmp) - 1, jf);
                    fclose(jf);
                    if (rd > 0) {
                        tmp[rd] = '\0';
                        off += snprintf(out + off, sizeof(out) - off, "%s,", tmp);
                    }
                }
            }
            closedir(d);
        }
    }
    if (off > 1) {
        out[off - 1] = ']';
    } else {
        out[1] = '\0';  /* overwrite the ',' */
        out[0] = '['; out[1] = ']'; off = 2;
    }
    out[off] = '\0';
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, off);
    return ESP_OK;
}

/* Parse the recording name out of "/play/<name>" (query stripped). */
static esp_err_t playback_handler(httpd_req_t *req)
{
    char name[64] = {0};
    const char *p = req->uri + strlen("/play/");
    const char *q = strchr(p, '?');
    size_t n = q ? (size_t)(q - p) : strlen(p);
    if (n == 0 || n >= sizeof(name)) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    memcpy(name, p, n);

    char path[200];
    snprintf(path, sizeof(path), "%s/%s.mjpg", APP_SDCARD_RECORD_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    size_t rd = fread(buf, 1, sz, f);
    fclose(f);

    /* Collect JPEG frame boundaries (FFD8 ... FFD9). */
    int max_frames = 2048;
    uint32_t *starts = heap_caps_malloc(sizeof(uint32_t) * max_frames,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t *ends = heap_caps_malloc(sizeof(uint32_t) * max_frames,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int nframes = 0;
    for (long i = 0; i + 1 < (long)rd && nframes < max_frames; i++) {
        if (buf[i] == 0xFF && buf[i + 1] == 0xD8) {
            starts[nframes] = (uint32_t)i;
        } else if (buf[i] == 0xFF && buf[i + 1] == 0xD9) {
            if (nframes < max_frames && starts[nframes] > 0) {
                ends[nframes] = (uint32_t)i + 2;
                nframes++;
            }
        }
    }

    static const char *ct =
        "multipart/x-mixed-replace;boundary=rec-mjpeg-boundary";
    httpd_resp_set_type(req, ct);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    char header[160];
    while (s_running) {
        for (int i = 0; i < nframes; i++) {
            uint32_t len = ends[i] - starts[i];
            int h = snprintf(header, sizeof(header),
                "\r\n--rec-mjpeg-boundary\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                (unsigned)len);
            if (httpd_resp_send_chunk(req, header, h) != ESP_OK ||
                httpd_resp_send_chunk(req, (const char *)(buf + starts[i]), len) != ESP_OK) {
                goto done;
            }
            vTaskDelay(pdMS_TO_TICKS(200));   /* ~5 fps playback */
        }
    }
done:
    free(starts);
    free(ends);
    heap_caps_free(buf);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ----- WiFi "streaming speaker": browser uploads a local audio file, we persist
 * it to the SD card and let the on-board codec play it. No Bluetooth needed. ----- */

static esp_err_t upload_handler(httpd_req_t *req)
{
    if (!app_sdcard_mounted()) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    /* File name comes from ?name= (URL-encoded). Fall back to a default. */
    char name[160] = {0};
    char *q = strchr(req->uri, '?');
    if (q && httpd_query_key_value(q + 1, "name", name, sizeof(name)) != ESP_OK) {
        name[0] = '\0';
    }
    if (name[0] == '\0') {
        strcpy(name, "upload.bin");
    }
    /* strip any path separators for safety */
    for (char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\') *p = '_';
    }

    char path[200];
    snprintf(path, sizeof(path), "%s/%s", APP_SDCARD_MOUNT_POINT, name);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "upload: cannot open %s", path);
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    uint8_t *buf = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    int total = req->content_len;
    int left = total;
    int received = 0;
    /* A single recv when content_len is unknown (chunked) just drains the socket
     * until the client closes / times out. */
    while (left != 0) {
        int want = (left > 2048) ? 2048 : (left > 0 ? left : 2048);
        int r = httpd_req_recv(req, (char *)buf, want);
        if (r <= 0) {
            break;
        }
        fwrite(buf, 1, r, f);
        received += r;
        if (left > 0) {
            left -= r;
        }
    }
    fclose(f);
    heap_caps_free(buf);

    char resp[256];
    int n = snprintf(resp, sizeof(resp),
                     "{\"ok\":true,\"bytes\":%d,\"url\":\"file://sdcard/%s\"}",
                     received, name);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, n);
    ESP_LOGI(TAG, "upload: saved %s (%d bytes)", path, received);
    return ESP_OK;
}

/* Play a URL on the on-board speaker (already-initialized codec player). */
static esp_err_t play_url_handler(httpd_req_t *req)
{
    char url[320] = {0};
    char *q = strchr(req->uri, '?');
    bool ok = false;
    if (q && httpd_query_key_value(q + 1, "url", url, sizeof(url)) == ESP_OK && url[0]) {
        esp_err_t r = app_music_player_play(url);
        ok = (r == ESP_OK);
    }
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":%s}", ok ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t stop_handler(httpd_req_t *req)
{
    app_music_player_stop();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", 11);
    return ESP_OK;
}

static const httpd_uri_t s_uri_stream = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = stream_handler,
};

static const httpd_uri_t s_uri_snapshot = {
    .uri = "/snapshot",
    .method = HTTP_GET,
    .handler = snapshot_handler,
};

static const httpd_uri_t s_uri_root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_handler,
};

static const httpd_uri_t s_uri_recordings = {
    .uri = "/recordings",
    .method = HTTP_GET,
    .handler = recordings_list_handler,
};

static const httpd_uri_t s_uri_rec_start = {
    .uri = "/rec/start",
    .method = HTTP_GET,
    .handler = rec_start_handler,
};

static const httpd_uri_t s_uri_rec_stop = {
    .uri = "/rec/stop",
    .method = HTTP_GET,
    .handler = rec_stop_handler,
};

static const httpd_uri_t s_uri_rec_status = {
    .uri = "/rec/status",
    .method = HTTP_GET,
    .handler = rec_status_handler,
};

static const httpd_uri_t s_uri_play = {
    .uri = "/play/*",
    .method = HTTP_GET,
    .handler = playback_handler,
};

static const httpd_uri_t s_uri_upload = {
    .uri = "/upload",
    .method = HTTP_POST,
    .handler = upload_handler,
};

static const httpd_uri_t s_uri_play_url = {
    .uri = "/api/play",
    .method = HTTP_POST,
    .handler = play_url_handler,
};

static const httpd_uri_t s_uri_stop = {
    .uri = "/api/stop",
    .method = HTTP_POST,
    .handler = stop_handler,
};

/* ----- Online-music API settings (KEY + custom API), persisted in NVS ----- */

static esp_err_t settings_handler(httpd_req_t *req)
{
    char key[160] = {0}, api[300] = {0};
    app_music_settings_get(key, sizeof(key), api, sizeof(api));

    static const char html[] =
        "<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,user-scalable=no\">"
        "<title>喵伴 · 音乐设置</title>"
        "<style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{background:radial-gradient(circle at 50%% 0%%,#1c2340,#0b0e1a 70%%);"
        "min-height:100vh;display:flex;flex-direction:column;align-items:center;"
        "color:#fff;font-family:'Segoe UI',system-ui,sans-serif;padding:18px}"
        ".app{width:100%%;max-width:520px;display:flex;flex-direction:column;gap:14px}"
        ".title{font-size:20px;font-weight:600;letter-spacing:1px;margin-top:4px}"
        ".title .dot{color:#2ecc71}"
        ".card{background:#141a30;border:1px solid #2a3355;border-radius:16px;padding:16px;display:flex;flex-direction:column;gap:10px}"
        "label{font-size:13px;color:#9fb0e0}"
        "input{background:#0d1226;color:#fff;border:1px solid #2a3355;border-radius:10px;padding:11px 12px;font-size:14px;width:100%%}"
        "input:focus{outline:none;border-color:#4c8cff}"
        ".hint{font-size:12px;color:#6b78a0;line-height:1.5}"
        "button{background:linear-gradient(135deg,#4c8cff,#3a6fd0);border:0;color:#fff;"
        "font-size:15px;font-weight:600;padding:12px 20px;border-radius:14px;cursor:pointer;"
        "box-shadow:0 6px 18px rgba(76,140,255,.35)}"
        "button:active{transform:scale(.96)}"
        "#msg{font-size:13px;color:#2ecc71;min-height:18px}"
        "a.back{color:#8fa8ff;font-size:13px;text-decoration:none;margin-top:4px}"
        "</style></head><body><div class=\"app\">"
        "<div class=\"title\">喵伴 <span class=\"dot\">●</span> 音乐设置</div>"
        "<div class=\"card\">"
        "<label>yaohud 密钥 (KEY)</label>"
        "<input id=\"key\" type=\"text\" placeholder=\"从 api.yaohud.cn 获取\" value=\"%s\">"
        "<div class=\"hint\">默认使用 yaohud 网易云 VIP 代理：<br>https://api.yaohud.cn/api/music/wyvip?key=KEY&amp;msg=歌名&amp;n=1</div>"
        "<label>自定义接口 (可选, 留空用默认)</label>"
        "<input id=\"api\" type=\"text\" placeholder=\"含 {q} 的 URL 模板\" value=\"%s\">"
        "<div class=\"hint\">如需换成其它音乐源，填一个含 <b>{q}</b> 的 GET 模板（歌名会替换 {q}）。留空则使用默认 yaohud。<br>若设备连不上 yaohud（日志 ESP_ERR_HTTP_CONNECT），多半是热点屏蔽了该域名，可部署 tools/music-proxy-worker.js 的 Cloudflare Worker，这里填 <b>https://你的子域.workers.dev/music?msg={q}</b> 即可绕过。</div>"
        "<button onclick=\"save()\">保存</button>"
        "<div id=\"msg\"></div>"
        "</div>"
        "<a class=\"back\" href=\"/\">← 返回摄像头</a>"
        "<script>"
        "async function save(){const k=document.getElementById('key').value;"
        "const a=document.getElementById('api').value;"
        "const fd=new URLSearchParams();fd.append('key',k);if(a)fd.append('api',a);"
        "try{const r=await fetch('/api/settings',{method:'POST',body:fd});"
        "const j=await r.json();"
        "document.getElementById('msg').textContent=j.ok?'✅ 已保存，播放音乐即可生效':'❌ 保存失败';}"
        "catch(e){document.getElementById('msg').textContent='❌ 网络错误'}}"
        "</script></body></html>";
    /* Inject current values into the static template (HTML-attribute safe: the
     * server-side values are alphanumeric/NVS strings, not user HTML). */
    char page[strlen(html) + sizeof(key) + sizeof(api) + 16];
    int n = snprintf(page, sizeof(page), html,
                     key[0] ? key : "", api[0] ? api : "");
    (void)n;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t settings_api_handler(httpd_req_t *req)
{
    char key[160] = {0}, api[300] = {0};
    bool got_key = false, got_api = false;

    /* POST body is urlencoded form (key=...&api=...). */
    if (req->content_len > 0 && req->content_len < (int)(sizeof(key) + sizeof(api) + 32)) {
        char *body = malloc(req->content_len + 1);
        if (body) {
            int r = httpd_req_recv(req, body, req->content_len);
            if (r > 0) {
                body[r] = '\0';
                char v[160];
                if (httpd_query_key_value(body, "key", v, sizeof(v)) == ESP_OK) {
                    strncpy(key, v, sizeof(key) - 1);
                    got_key = true;
                }
                if (httpd_query_key_value(body, "api", v, sizeof(api)) == ESP_OK) {
                    strncpy(api, v, sizeof(api) - 1);
                    got_api = true;
                }
            }
            free(body);
        }
    }

    if (!got_key) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"err\":\"missing key\"}", 28);
        return ESP_OK;
    }
    esp_err_t res = app_music_settings_save(key, got_api ? api : NULL);
    char resp[48];
    snprintf(resp, sizeof(resp), "{\"ok\":%s}", (res == ESP_OK) ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static const httpd_uri_t s_uri_settings = {
    .uri = "/settings",
    .method = HTTP_GET,
    .handler = settings_handler,
};

static const httpd_uri_t s_uri_settings_api = {
    .uri = "/api/settings",
    .method = HTTP_POST,
    .handler = settings_api_handler,
};

esp_err_t http_preview_start(void)
{
    if (s_running) {
        return ESP_OK;
    }
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
    /* The frame callback only copies frames; it is invoked whenever the camera
     * pipeline is running (started on demand by the activity manager). */
    app_camera_register_frame_cb(on_frame, NULL);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.task_priority = 4;
    config.max_uri_handlers = 16;
    /* Purge least-recently-used sockets so a stale/abandoned browser tab does
     * not leak a connection and clog the single httpd task (which made the
     * snapshot polling appear frozen). */
    config.lru_purge_enable = true;
    config.max_open_sockets = 4;
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start (80) failed");
        app_camera_register_frame_cb(NULL, NULL);
        return ESP_FAIL;
    }
    httpd_register_uri_handler(s_server, &s_uri_root);
    httpd_register_uri_handler(s_server, &s_uri_snapshot);
    httpd_register_uri_handler(s_server, &s_uri_stream);
    httpd_register_uri_handler(s_server, &s_uri_recordings);
    httpd_register_uri_handler(s_server, &s_uri_rec_start);
    httpd_register_uri_handler(s_server, &s_uri_rec_stop);
    httpd_register_uri_handler(s_server, &s_uri_rec_status);
    httpd_register_uri_handler(s_server, &s_uri_play);
    httpd_register_uri_handler(s_server, &s_uri_upload);
    httpd_register_uri_handler(s_server, &s_uri_play_url);
    httpd_register_uri_handler(s_server, &s_uri_stop);
    httpd_register_uri_handler(s_server, &s_uri_settings);
    httpd_register_uri_handler(s_server, &s_uri_settings_api);

    s_running = true;

    char ip_str[16] = {0};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
            esp_ip4addr_ntoa(&ip.ip, ip_str, sizeof(ip_str));
        }
    }
    ESP_LOGI(TAG, "camera web server: http://%s/  (camera starts on demand)",
             ip_str[0] ? ip_str : "<sta-ip>");
    return ESP_OK;
}

esp_err_t http_preview_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }
    s_running = false;
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    app_camera_register_frame_cb(NULL, NULL);
    if (s_latest) {
        heap_caps_free(s_latest);
        s_latest = NULL;
    }
    s_latest_len = 0;
    return ESP_OK;
}

bool http_preview_is_running(void)
{
    return s_running;
}
