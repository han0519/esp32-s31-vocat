/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Music player page for the ESP-VoCat-S31 "喵伴".
 *
 * Full player UI (360x360 round screen):
 *   top bar : "♪ 音乐播放器" + Bluetooth icon
 *   cover   : rounded gradient "album art" card
 *   title   : scrolling song name
 *   lyrics  : scrolling lyric line
 *   progress: seek bar + current/total time (simulated, the simple player
 *             has no position query API)
 *   controls: prev / play-pause / next (+ stop on long press of play)
 *
 * Actual playback is handled by app_music_player (esp_audio_simple_player),
 * which decodes MP3/AAC streams and writes PCM to the ES8389 speaker.
 */
#include "page_music.h"

#include <string.h>
#include "esp_timer.h"
#include "app_sfx.h"
#include "app_music_player.h"
#include "app_sdcard.h"
#include "esp_xiaozhi_chat_display.h"
#include "lvgl.h"
#include "font_awesome_symbols.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include <cJSON.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui_page_manager.h"

#define BG      lv_color_black()
#define FG      lv_color_white()
#define ACCENT  lv_color_hex(0x3D8BFD)
#define DIM     lv_color_hex(0x888888)
#define CARD    lv_color_hex(0x1E1E24)

/* Network demo tracks removed per user request — only SD-card music is shown. */
/* Simulated track length (seconds) used for the progress bar. */
#define TRACK_LEN_S 210   /* 3:30 */

static lv_obj_t *s_cover = NULL;        /* cover card */
static lv_obj_t *s_title = NULL;        /* scrolling song name */
static lv_obj_t *s_lyric = NULL;        /* scrolling lyric line */
static lv_obj_t *s_bar = NULL;          /* progress bar */
static lv_obj_t *s_time = NULL;         /* mm:ss / mm:ss */
static lv_obj_t *s_bt_icon = NULL;      /* bluetooth status icon */
static lv_obj_t *s_btn_prev = NULL;
static lv_obj_t *s_btn_play = NULL;     /* toggles play/pause icon */
static lv_obj_t *s_btn_next = NULL;
static lv_obj_t *s_list = NULL;         /* playlist container */
static int s_cur_track = -1;            /* index into the SD-card list */

/* SD card music list (only source of tracks — network demo tracks removed). */
#define SD_MAX_ITEMS 8
static app_sdcard_music_item_t s_sd_items[SD_MAX_ITEMS];
static int s_sd_count = 0;

/* Simulated progress */
static int64_t s_pos_ms = 0;            /* current position in stream */
static int64_t s_last_tick_ms = 0;      /* last refresh */
static int64_t s_play_epoch_ms = 0;     /* when playing began (for bar animation) */

static void fmt_time(int64_t ms, char *buf, size_t n)
{
    int s = (int)(ms / 1000);
    snprintf(buf, n, "%02d:%02d", s / 60, s % 60);
}

static void refresh_state(void)
{
    app_music_state_t st = app_music_player_get_state();
    bool playing = (st == APP_MUSIC_PLAYING);
    static app_music_state_t s_last_st = APP_MUSIC_STOPPED;
    static int s_last_preset = -1;
    static int s_last_sec = -1;

    /* Only touch labels when the value actually changes: lv_label_set_text on
     * an unchanged string still invalidates/redraws the widget every tick,
     * which wastes CPU on the 360x360 panel. */
    if (s_title && s_cur_track != s_last_preset) {
        if (s_cur_track >= 0 && s_cur_track < s_sd_count) {
            lv_label_set_text(s_title, s_sd_items[s_cur_track].title);
        } else {
            lv_label_set_text(s_title, "未选择曲目");
        }
    }
    if (s_lyric && (st != s_last_st || s_cur_track != s_last_preset)) {
        if (s_cur_track >= 0) {
            lv_label_set_text(s_lyric, playing ? "正在播放中…" : "已暂停");
        } else {
            lv_label_set_text(s_lyric, "点击下方曲目开始播放");
        }
    }
    if (s_btn_play && st != s_last_st) {
        lv_label_set_text(lv_obj_get_child(s_btn_play, 0),
                          playing ? FONT_AWESOME_PAUSE : FONT_AWESOME_PLAY);
    }
    int sec = (int)(s_pos_ms / 1000);
    if (sec != s_last_sec) {
        if (s_bar) {
            lv_bar_set_value(s_bar, sec * 100 / TRACK_LEN_S, LV_ANIM_OFF);
        }
        if (s_time) {
            char buf[32];
            char cur[8], tot[8];
            fmt_time(s_pos_ms, cur, sizeof(cur));
            fmt_time((int64_t)TRACK_LEN_S * 1000, tot, sizeof(tot));
            snprintf(buf, sizeof(buf), "%s / %s", cur, tot);
            lv_label_set_text(s_time, buf);
        }
    }
    s_last_st = st;
    s_last_preset = s_cur_track;
    s_last_sec = sec;
    /* Bluetooth status icon color is static (grey) for now; no per-tick work. */
    (void)s_bt_icon;
}

static int cur_list_count(void)
{
    return s_sd_count;
}

static const char *cur_track_url(int idx)
{
    return (idx >= 0 && idx < s_sd_count) ? s_sd_items[idx].path : NULL;
}

static void set_track(int idx)
{
    int n = cur_list_count();
    if (idx < 0 || idx >= n) {
        return;
    }
    const char *url = cur_track_url(idx);
    if (url == NULL) {
        return;
    }
    s_cur_track = idx;
    s_pos_ms = 0;
    if (app_music_player_play(url) != ESP_OK) {
        if (s_lyric) {
            lv_label_set_text(s_lyric, "播放失败");
        }
    } else {
        s_play_epoch_ms = esp_timer_get_time() / 1000;
        refresh_state();
    }
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    app_sfx_play(APP_SFX_TAP);
    int n = cur_list_count();
    if (n <= 0) return;
    set_track(s_cur_track <= 0 ? n - 1 : s_cur_track - 1);
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    app_sfx_play(APP_SFX_TAP);
    int n = cur_list_count();
    if (n <= 0) return;
    set_track((s_cur_track + 1) % n);
}

static void play_cb(lv_event_t *e)
{
    (void)e;
    app_sfx_play(APP_SFX_TAP);
    if (s_cur_track < 0) {
        set_track(0);
        return;
    }
    if (app_music_player_get_state() == APP_MUSIC_PAUSED) {
        app_music_player_resume();
        s_play_epoch_ms = esp_timer_get_time() / 1000;
    } else if (app_music_player_get_state() == APP_MUSIC_STOPPED) {
        const char *url = cur_track_url(s_cur_track);
        if (url) {
            s_pos_ms = 0;
            app_music_player_play(url);
            s_play_epoch_ms = esp_timer_get_time() / 1000;
        }
    }
    refresh_state();
}

static void track_cb(lv_event_t *e)
{
    app_sfx_play(APP_SFX_TAP);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    set_track(idx);
}

/* ---- AI / voice music control ------------------------------------------- */

/* Online music search: query a public search API over HTTPS, parse the JSON
 * response and copy the first streamable (http/https) URL into out_url.
 * Several endpoints are tried in order as fallbacks, since free APIs come and
 * go. The song name is URL-encoded (UTF-8 bytes) before being appended. */

/* Candidate search endpoints (each takes the song name appended after the '=').
 * Tried in order; the first that returns a playable URL wins. Kept several
 * because free music APIs appear/disappear; the generic URL scanner below
 * adapts to whatever JSON shape the live endpoint happens to return. */
static const char *const k_music_apis[] = {
    "https://api.uomg.com/api/qqyy.php?msg=",      /* long-standing; {"music":"<mp3>"} */
    "https://api.vvhan.com/api/music?name=",        /* {"data":{"url":"<mp3>"}} */
    "https://api.codelife.cc/api/music?name=",      /* {"data":{...}} */
    "https://api.injahow.cn/meting/?server=netease&type=song&name=", /* Meting proxy */
    NULL,
};

#define MUSIC_HTTP_TIMEOUT_MS 8000
#define MUSIC_BODY_MAX       2048

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} music_http_acc_t;

static esp_err_t music_http_event(esp_http_client_event_t *evt)
{
    music_http_acc_t *a = (music_http_acc_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0 && a) {
        if (a->len + evt->data_len < a->cap) {
            memcpy(a->buf + a->len, evt->data, evt->data_len);
            a->len += evt->data_len;
        }
    }
    return ESP_OK;
}

/* Percent-encode a UTF-8 string for use in a query parameter. */
static void music_url_encode(const char *src, char *dst, size_t dst_cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 1 < dst_cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[j++] = (char)c;
        } else if (j + 3 < dst_cap) {
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 0xF];
        } else {
            break;
        }
    }
    dst[j] = '\0';
}

/* Heuristic: does this URL look like an audio stream (vs a cover/lyric)? */
static bool music_is_audio_url(const char *v)
{
    return (strstr(v, ".mp3")  || strstr(v, ".m4a")  || strstr(v, ".aac") ||
            strstr(v, ".flac") || strstr(v, ".wav")  || strstr(v, ".ogg") ||
            strstr(v, ".opus") || strstr(v, "mp3")   || strstr(v, "audio") ||
            strstr(v, "music") || strstr(v, "media") || strstr(v, "play") ||
            strstr(v, "song"));
}

static bool music_is_image_url(const char *v)
{
    return (strstr(v, ".jpg") || strstr(v, ".jpeg") || strstr(v, ".png") ||
            strstr(v, ".webp") || strstr(v, ".gif"));
}

/* Recursively scan a parsed JSON tree for the first usable stream URL.
 * mode 0: prefer audio URLs; mode 1: accept any http(s) URL that is not an
 * image/lyric. This makes the search API-agnostic (handles top-level keys,
 * nested "data" objects, and JSON arrays alike). */
static const char *music_find_url(cJSON *node, char *out, size_t out_len, int mode)
{
    if (node == NULL || out == NULL) {
        return NULL;
    }
    if (cJSON_IsString(node) && node->valuestring) {
        const char *v = node->valuestring;
        if (strncmp(v, "http", 4) == 0) {
            if (mode == 0) {
                if (music_is_audio_url(v)) {
                    strncpy(out, v, out_len - 1);
                    out[out_len - 1] = '\0';
                    return out;
                }
            } else {
                if (!music_is_image_url(v) && !strstr(v, ".lrc")) {
                    strncpy(out, v, out_len - 1);
                    out[out_len - 1] = '\0';
                    return out;
                }
            }
        }
        return NULL;
    }
    if (cJSON_IsArray(node)) {
        cJSON *e;
        cJSON_ArrayForEach(e, node) {
            if (music_find_url(e, out, out_len, mode)) {
                return out;
            }
        }
    } else if (cJSON_IsObject(node)) {
        cJSON *c;
        cJSON_ArrayForEach(c, node) {
            if (music_find_url(c, out, out_len, mode)) {
                return out;
            }
        }
    }
    return NULL;
}

/* Primary online music API: yaohud (网易云 VIP 代理). Requires a key from
 * https://api.yaohud.cn (控制台 -> 密钥管理). The key is stored in NVS
 * (namespace "vocat", key "music_key") and editable from the web settings page
 * (/settings). A default key is seeded on first boot. Leave empty (or set via
 * the web page) to skip yaohud and use the keyless fallbacks only.
 * Request:  .../wyvip?key=KEY&msg=SONG&n=1  -> data.url is the mp3 stream. */
#define YAOHUD_URL      "https://api.yaohud.cn/api/music/wyvip"
#define MUSIC_KEY_NS    "vocat"
#define MUSIC_KEY_KEY   "music_key"
#define MUSIC_API_KEY   "music_api"     /* optional custom API template in NVS */
#define MUSIC_KEY_DFLT  "oIF7nJpFIcsgBBAkDmV"
#define MUSIC_KEY_MAX   128
#define MUSIC_API_MAX   256

/* Runtime copy of the key/api so we don't hit NVS on every search. */
static char s_music_key[MUSIC_KEY_MAX] = {0};
static char s_music_api[MUSIC_API_MAX] = {0};   /* empty = use yaohud */

/* Load key+api from NVS (seeding the default key on first boot). */
static void music_settings_load(void)
{
    s_music_key[0] = '\0';
    s_music_api[0] = '\0';
    nvs_handle_t h = 0;
    esp_err_t r = nvs_open(MUSIC_KEY_NS, NVS_READWRITE, &h);
    if (r != ESP_OK) {
        return;
    }
    size_t len = MUSIC_KEY_MAX;
    if (nvs_get_str(h, MUSIC_KEY_KEY, s_music_key, &len) != ESP_OK) {
        /* First boot (or key erased): seed the default key. */
        strncpy(s_music_key, MUSIC_KEY_DFLT, MUSIC_KEY_MAX - 1);
        nvs_set_str(h, MUSIC_KEY_KEY, s_music_key);
        nvs_commit(h);
        ESP_LOGI("MUSIC", "seeded default yaohud key");
    }
    len = MUSIC_API_MAX;
    nvs_get_str(h, MUSIC_API_KEY, s_music_api, &len);  /* may be empty */
    nvs_close(h);
}

/* Persist key+api to NVS (called from the web settings page). */
esp_err_t app_music_settings_save(const char *key, const char *api)
{
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h = 0;
    esp_err_t r = nvs_open(MUSIC_KEY_NS, NVS_READWRITE, &h);
    if (r != ESP_OK) {
        return r;
    }
    strncpy(s_music_key, key, MUSIC_KEY_MAX - 1);
    s_music_key[MUSIC_KEY_MAX - 1] = '\0';
    nvs_set_str(h, MUSIC_KEY_KEY, s_music_key);
    if (api != NULL) {
        strncpy(s_music_api, api, MUSIC_API_MAX - 1);
        s_music_api[MUSIC_API_MAX - 1] = '\0';
        nvs_set_str(h, MUSIC_API_KEY, s_music_api);
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI("MUSIC", "saved key='%.*s' api='%s'",
             (int)strcspn(s_music_key, ""), s_music_key, s_music_api);
    return ESP_OK;
}

void app_music_settings_get(char *key, size_t key_cap, char *api, size_t api_cap)
{
    if (key) {
        strncpy(key, s_music_key, key_cap - 1);
        key[key_cap - 1] = '\0';
    }
    if (api) {
        strncpy(api, s_music_api, api_cap - 1);
        api[api_cap - 1] = '\0';
    }
}

/* Callback type: extract a playable URL from a parsed JSON response. */
typedef const char *(*music_url_parser_t)(cJSON *root, char *out, size_t out_len);

/* Perform one GET and let `parse` pull the stream URL out of the JSON body. */
static esp_err_t music_try_request(const char *url, music_url_parser_t parse,
                                   char *out_url, size_t len)
{
    char *body = malloc(MUSIC_BODY_MAX);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    music_http_acc_t acc = { .buf = body, .cap = MUSIC_BODY_MAX, .len = 0 };
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = music_http_event,
        .user_data = &acc,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = MUSIC_HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t ret = ESP_FAIL;
    if (client != NULL) {
        esp_err_t r = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI("MUSIC", "GET %s -> status=%d perf=%s", url, status, esp_err_to_name(r));
        if (r == ESP_OK && status == 200 && acc.len > 0) {
            body[acc.len] = '\0';
            ESP_LOGD("MUSIC", "resp: %s", body);
            cJSON *root = cJSON_Parse(body);
            char found[512];
            if (root != NULL) {
                if (parse(root, found, sizeof(found)) != NULL) {
                    strncpy(out_url, found, len - 1);
                    out_url[len - 1] = '\0';
                    ret = ESP_OK;
                }
                cJSON_Delete(root);
            }
        }
        esp_http_client_cleanup(client);
    }
    free(body);
    return ret;
}

/* yaohud response (verified):
 *   {"code":200,"data":{"name":...,"url":"https://music.163.com/song/media/
 *    outer/url?id=NNN.mp3",  // outer url, needs a 302 redirect
 *    "vipmusic":{"url":"http://mNNN.music.126.net/.../NNN.mp3?vuutv=..."}}} // direct file
 * Prefer the direct vipmusic.url (no redirect, 320kbps); fall back to the
 * outer data.url (GMF follows the 302), then data.musicurl. */
static const char *music_parse_yaohud(cJSON *root, char *out, size_t out_len)
{
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (code != NULL && cJSON_IsNumber(code) && code->valueint != 200) {
        return NULL;
    }
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data == NULL || !cJSON_IsObject(data)) {
        return NULL;
    }
    /* 1) direct VIP file URL (no redirect, highest quality) */
    cJSON *vip = cJSON_GetObjectItem(data, "vipmusic");
    if (vip != NULL && cJSON_IsObject(vip)) {
        cJSON *vu = cJSON_GetObjectItem(vip, "url");
        if (cJSON_IsString(vu) && vu->valuestring && strncmp(vu->valuestring, "http", 4) == 0) {
            strncpy(out, vu->valuestring, out_len - 1);
            out[out_len - 1] = '\0';
            return out;
        }
    }
    /* 2) outer url (302 redirect to the real file) */
    cJSON *u = cJSON_GetObjectItem(data, "url");
    if (cJSON_IsString(u) && u->valuestring && strncmp(u->valuestring, "http", 4) == 0) {
        strncpy(out, u->valuestring, out_len - 1);
        out[out_len - 1] = '\0';
        return out;
    }
    /* 3) legacy musicurl field */
    cJSON *mu = cJSON_GetObjectItem(data, "musicurl");
    if (cJSON_IsString(mu) && mu->valuestring && strncmp(mu->valuestring, "http", 4) == 0) {
        strncpy(out, mu->valuestring, out_len - 1);
        out[out_len - 1] = '\0';
        return out;
    }
    return NULL;
}

/* Generic: prefer an audio URL, then any non-image http(s). */
static const char *music_parse_generic(cJSON *root, char *out, size_t out_len)
{
    if (music_find_url(root, out, out_len, 0) != NULL ||
        music_find_url(root, out, out_len, 1) != NULL) {
        return out;
    }
    return NULL;
}

esp_err_t app_music_online_search(const char *name, char *out_url, size_t len)
{
    if (name == NULL || name[0] == '\0' || out_url == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out_url[0] = '\0';

    char enc[256];
    music_url_encode(name, enc, sizeof(enc));

    /* 0) Custom API template (set via web settings). It must contain the
     *    literal "{q}" where the song name (already URL-encoded) goes. */
    if (s_music_api[0] != '\0' && strstr(s_music_api, "{q}") != NULL) {
        char *req = malloc(strlen(s_music_api) + strlen(enc) + 8);
        if (req != NULL) {
            /* Replace the literal "{q}" token with the URL-encoded song name. */
            const char *p = strstr(s_music_api, "{q}");
            size_t head = (size_t)(p - s_music_api);
            memcpy(req, s_music_api, head);
            strcpy(req + head, enc);
            strcpy(req + head + strlen(enc), p + 3);  /* skip the "{q}" token */
            if (music_try_request(req, music_parse_generic, out_url, len) == ESP_OK) {
                free(req);
                return ESP_OK;
            }
            free(req);
        }
        ESP_LOGW("MUSIC", "custom API search failed; falling back to yaohud");
    }

    /* 1) yaohud (网易云 VIP 代理): a single call with n=1 returns the mp3 URL. */
    if (s_music_key[0] != '\0') {
        char req[512];
        snprintf(req, sizeof(req), "%s?key=%s&msg=%s&n=1",
                 YAOHUD_URL, s_music_key, enc);
        if (music_try_request(req, music_parse_yaohud, out_url, len) == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW("MUSIC", "yaohud search failed; trying keyless fallbacks");
    }

    /* 2) Keyless fallbacks (free APIs may be blocked/unreliable). */
    esp_err_t ret = ESP_FAIL;
    for (int i = 0; k_music_apis[i] != NULL; i++) {
        size_t req_cap = strlen(k_music_apis[i]) + strlen(enc) + 8;
        char *req = malloc(req_cap);
        if (req == NULL) {
            continue;
        }
        snprintf(req, req_cap, "%s%s", k_music_apis[i], enc);
        if (music_try_request(req, music_parse_generic, out_url, len) == ESP_OK) {
            ret = ESP_OK;
            free(req);
            break;
        }
        free(req);
    }
    return ret;
}

/* Async online search so the chat/AI pipeline is never blocked while we hit
 * the network. The worker task performs the HTTP search and starts playback. */
#define MUSIC_SEARCH_STACK (6 * 1024)
static TaskHandle_t s_search_task = NULL;
static char s_search_name[64];

static void music_search_task(void *arg)
{
    (void)arg;
    char name[64];
    memcpy(name, s_search_name, sizeof(name));
    char url[512];
    if (app_music_online_search(name, url, sizeof(url)) == ESP_OK && url[0] != '\0') {
        if (app_music_player_play(url) == ESP_OK) {
            s_play_epoch_ms = esp_timer_get_time() / 1000;
            if (lvgl_port_lock(1000)) {
                if (s_title) lv_label_set_text(s_title, name);
                if (s_lyric) lv_label_set_text(s_lyric, "正在线播放…");
                lvgl_port_unlock();
            }
        }
    } else {
        if (lvgl_port_lock(1000)) {
            if (s_lyric) lv_label_set_text(s_lyric, "没找到这首，先放SD卡里的吧");
            lvgl_port_unlock();
        }
    }
    s_search_task = NULL;
    vTaskDelete(NULL);
}

static void start_online_search(const char *name)
{
    strncpy(s_search_name, name, sizeof(s_search_name) - 1);
    s_search_name[sizeof(s_search_name) - 1] = '\0';
    if (s_search_task != NULL) {
        return;   /* a search is already running */
    }
    if (lvgl_port_lock(1000)) {
        if (s_lyric) lv_label_set_text(s_lyric, "正在搜索在线歌曲…");
        lvgl_port_unlock();
    }
    if (xTaskCreatePinnedToCore(music_search_task, "music_srch", MUSIC_SEARCH_STACK,
                                NULL, 4, &s_search_task, 1) != pdPASS) {
        s_search_task = NULL;
        if (lvgl_port_lock(1000)) {
            if (s_lyric) lv_label_set_text(s_lyric, "搜索任务创建失败");
            lvgl_port_unlock();
        }
    }
}

static void strip_suffix_utf8(char *s, const char *suffix)
{
    size_t ls = strlen(s), lp = strlen(suffix);
    if (ls >= lp && memcmp(s + ls - lp, suffix, lp) == 0) {
        s[ls - lp] = '\0';
    }
}

bool app_music_intent(const char *text, char *out, size_t n)
{
    if (text == NULL || out == NULL || n == 0) {
        return false;
    }
    out[0] = '\0';
    static const char *const kw[] = {
        "播放", "放一首", "来一首", "听一首", "点播",
        "搜索", "搜一下", "搜", "找一首", "找", "唱一首", "唱", "放"
    };
    const char *hit = NULL;
    for (int i = 0; i < (int)(sizeof(kw) / sizeof(kw[0])); i++) {
        const char *p = strstr(text, kw[i]);
        if (p) {
            hit = p + strlen(kw[i]);
            break;
        }
    }
    if (hit == NULL) {
        return false;
    }
    int j = 0;
    while (*hit && j + 1 < (int)n) {
        char ch = *hit;
        /* Stop at ASCII punctuation/whitespace. CJK full-width punctuation is
         * multi-byte and would form an invalid multi-character constant, so it is
         * intentionally not listed here; trailing CJK particles (的歌/歌曲/…) are
         * stripped afterwards. */
        if (ch == '!' || ch == '?' || ch == '.' || ch == ',' ||
            ch == ' ' || ch == '\n' || ch == '\r' || ch == ';' || ch == ':') {
            break;
        }
        out[j++] = ch;
        hit++;
    }
    out[j] = '\0';
    /* Trim trailing CJK particles so "播放周杰伦的歌" -> "周杰伦". */
    strip_suffix_utf8(out, "的歌曲");
    strip_suffix_utf8(out, "的歌");
    strip_suffix_utf8(out, "这首歌");
    strip_suffix_utf8(out, "歌曲");
    strip_suffix_utf8(out, "音乐");
    strip_suffix_utf8(out, "歌");
    return true;
}

esp_err_t app_music_play_by_name(const char *name)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI("MUSIC", "voice request: '%s'", name);
    if (ui_page_manager_current() != PAGE_MUSIC) {
        ui_page_manager_switch(PAGE_MUSIC);   /* on_enter scans the SD library */
    }
    /* 1) Match an SD-card track by (sub)string. */
    for (int i = 0; i < s_sd_count; i++) {
        if (s_sd_items[i].title[0] != '\0' &&
            (strstr(s_sd_items[i].title, name) || strstr(name, s_sd_items[i].title))) {
            set_track(i);
            return ESP_OK;
        }
    }
    /* 2) Fall back to an async online search (non-blocking; the worker task
     *    updates the lyric label and starts playback when it finds a URL). */
    start_online_search(name);
    return ESP_OK;
}

/* Rebuild the 2x2 SD-card playlist buttons. */
static void build_playlist(void)
{
    if (s_list == NULL) {
        return;
    }
    /* clear existing children */
    lv_obj_clean(s_list);

    int n = cur_list_count();
    int show = (n > 4) ? 4 : n;   /* 2x2 grid, show first 4 */
    for (int i = 0; i < show; i++) {
        const char *name = s_sd_items[i].title;
        lv_obj_t *b = lv_btn_create(s_list);
        lv_obj_set_size(b, 100, 26);
        lv_obj_set_style_radius(b, 13, 0);
        lv_obj_set_style_bg_color(b, CARD, 0);
        lv_obj_set_style_bg_color(b, ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_add_event_cb(b, track_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        /* trim long SD filenames for display */
        char disp[32];
        strncpy(disp, name, sizeof(disp) - 1);
        disp[sizeof(disp) - 1] = '\0';
        lv_label_set_text(l, disp);
        lv_obj_set_style_text_color(l, FG, 0);
        lv_obj_center(l);
    }
    if (n > 4) {
        lv_obj_t *more = lv_label_create(s_list);
        lv_label_set_text_fmt(more, "…还有 %d 首", n - 4);
        lv_obj_set_style_text_color(more, DIM, 0);
    }
    if (n == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, "SD卡中没有音乐");
        lv_obj_set_style_text_color(empty, DIM, 0);
    }
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb,
                          uint32_t bg, int w, int h)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3A3A4A), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    return b;
}

esp_err_t page_music_on_enter(void)
{
    music_settings_load();   /* refresh key/api from NVS (web-editable) */
    lv_obj_t *root = esp_xiaozhi_chat_display_page_root();
    if (root == NULL) {
        return ESP_FAIL;
    }
    lv_obj_set_style_bg_color(root, BG, 0);

    /* --- Top bar: back button + title + bluetooth icon --- */
    extern lv_obj_t *ui_page_make_back_button(lv_obj_t *);
    ui_page_make_back_button(root);
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, FONT_AWESOME_MUSIC "  音乐播放器");
    lv_obj_set_style_text_color(title, FG, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_bt_icon = lv_label_create(root);
    lv_label_set_text(s_bt_icon, FONT_AWESOME_BLUETOOTH);
    lv_obj_set_style_text_color(s_bt_icon, DIM, 0);
    lv_obj_align(s_bt_icon, LV_ALIGN_TOP_RIGHT, -16, 8);

    /* --- Cover card (rounded gradient placeholder album art) --- */
    s_cover = lv_obj_create(root);
    lv_obj_set_size(s_cover, 88, 88);
    lv_obj_align(s_cover, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_set_style_radius(s_cover, 16, 0);
    lv_obj_set_style_bg_color(s_cover, ACCENT, 0);
    lv_obj_set_style_bg_grad_color(s_cover, lv_color_hex(0x8E44AD), 0);
    lv_obj_set_style_bg_grad_dir(s_cover, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(s_cover, 0, 0);
    lv_obj_t *cv_icon = lv_label_create(s_cover);
    lv_label_set_text(cv_icon, FONT_AWESOME_MUSIC);
    lv_obj_set_style_text_color(cv_icon, lv_color_white(), 0);
    lv_obj_center(cv_icon);

    /* --- Song name (scrolling) --- */
    s_title = lv_label_create(root);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_title, LV_HOR_RES - 40);
    lv_obj_set_style_text_color(s_title, FG, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 118);
    lv_label_set_text(s_title, "未选择曲目");

    /* --- Lyric line (scrolling) --- */
    s_lyric = lv_label_create(root);
    lv_label_set_long_mode(s_lyric, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_lyric, LV_HOR_RES - 48);
    lv_obj_set_style_text_color(s_lyric, DIM, 0);
    lv_obj_align(s_lyric, LV_ALIGN_TOP_MID, 0, 140);
    lv_label_set_text(s_lyric, "点击下方曲目开始播放");

    /* --- Progress bar --- */
    lv_obj_t *bar_wrap = lv_obj_create(root);
    lv_obj_set_size(bar_wrap, LV_HOR_RES - 56, 20);
    lv_obj_align(bar_wrap, LV_ALIGN_TOP_MID, 0, 158);
    lv_obj_set_style_bg_opa(bar_wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar_wrap, 0, 0);
    lv_obj_set_style_pad_all(bar_wrap, 0, 0);

    s_bar = lv_bar_create(bar_wrap);
    lv_obj_set_size(s_bar, LV_HOR_RES - 56, 5);
    lv_obj_align(s_bar, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x3A3A44), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_INDICATOR);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_CLICKABLE);

    s_time = lv_label_create(root);
    lv_obj_set_style_text_color(s_time, DIM, 0);
    lv_obj_align(s_time, LV_ALIGN_TOP_MID, 0, 166);
    lv_label_set_text(s_time, "00:00 / 03:30");

    /* --- Transport controls --- */
    lv_obj_t *ctrl = lv_obj_create(root);
    lv_obj_set_size(ctrl, LV_HOR_RES - 24, 46);
    lv_obj_align(ctrl, LV_ALIGN_TOP_MID, 0, 182);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(ctrl, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl, 0, 0);
    lv_obj_set_style_pad_all(ctrl, 0, 0);
    lv_obj_set_style_pad_column(ctrl, 22, 0);

    s_btn_prev = make_btn(ctrl, FONT_AWESOME_PREV, prev_cb, 0x3A3A44, 40, 40);
    s_btn_play = make_btn(ctrl, FONT_AWESOME_PLAY, play_cb, 0x2ECC71, 48, 48);
    s_btn_next = make_btn(ctrl, FONT_AWESOME_NEXT, next_cb, 0x3A3A44, 40, 40);

    /* --- Playlist (SD-card tracks only) --- */
    s_list = lv_obj_create(root);
    lv_obj_set_size(s_list, 236, 60);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 246);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_style_pad_column(s_list, 10, 0);

    s_sd_count = app_sdcard_scan_music(s_sd_items, SD_MAX_ITEMS);
    s_cur_track = -1;
    build_playlist();

    s_pos_ms = 0;
    s_last_tick_ms = esp_timer_get_time() / 1000;
    refresh_state();
    app_sfx_play(APP_SFX_PAGE);
    return ESP_OK;
}

esp_err_t page_music_on_exit(void)
{
    /* Leaving the music page must free the audio path / stream so the app does
     * not keep consuming bandwidth or the speaker while inactive (bandwidth is
     * only used while the music page is actually open). */
    app_music_player_stop();
    return ESP_OK;
}

void page_music_on_tick(uint32_t ms)
{
    (void)ms;
    int64_t now = (int64_t)(esp_timer_get_time() / 1000);
    int64_t delta = now - s_last_tick_ms;
    /* Progress bar needs 1s granularity; a 500ms tick keeps it responsive while
     * halving the per-frame work vs a 250ms tick. */
    if (delta < 500) {
        return;
    }
    s_last_tick_ms = now;
    /* Advance the simulated progress while playing (use real elapsed time). */
    if (app_music_player_get_state() == APP_MUSIC_PLAYING) {
        s_pos_ms += delta;
        if (s_pos_ms > (int64_t)TRACK_LEN_S * 1000) {
            s_pos_ms = 0;   /* loop for the demo */
        }
    }
    refresh_state();
}

const ui_page_t page_music = {
    .id = PAGE_MUSIC,
    .name = "music",
    .on_enter = page_music_on_enter,
    .on_exit = page_music_on_exit,
    .on_tick = page_music_on_tick,
};
