/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SoftAP + captive-portal WiFi provisioning for the VoCat-S31 xiaozhi board.
 *
 * Boot flow (wired into esp_xiaozhi_chat_app after the display is up):
 *   1. wifi_sta_connect_from_nvs() tries to join the home AP using credentials
 *      previously stored in NVS ("wifi_creds").
 *   2. If no creds / cannot connect -> provisioning_enter() starts an OPEN SoftAP
 *      "Xiaozhi-XXXX", a tiny HTTP config page at http://192.168.4.1, and a DNS
 *      redirect so phones auto-open the page. The screen shows the AP name.
 *      On submit, creds are saved to NVS and the chip reboots into station mode.
 */

#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_system.h"

#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "wifi_provisioning.h"

static const char *TAG = "wifi_prov";

/* Chinese-capable font from the 78/xiaozhi-fonts component (Puhui 20px, 4bpp).
 * Without it LVGL falls back to its built-in ASCII-only font and every CJK
 * codepoint renders as a placeholder box -> the "garbled" provisioning text. */
LV_FONT_DECLARE(font_puhui_20_4);

/* ---- WiFi connect (station, from NVS) ---- */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_evt = NULL;
static bool s_wifi_inited = false;
static SemaphoreHandle_t s_prov_done = NULL;
static char s_saved_ssid[33] = {0};

/* One DISCONNECTED event must NOT fail the connect attempt: WiFi drops once
 * during association (roaming, transient RF, AP busy). We re-issue
 * esp_wifi_connect() so the link keeps trying in the background, and only the
 * caller's timeout decides when to give up. We never set WIFI_FAIL_BIT here, so
 * a temporarily unreachable AP does not throw the board into provisioning. */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_evt, WIFI_CONNECTED_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* Keep the link trying in the background. esp_wifi_connect() is async
         * and the WiFi driver applies its own backoff between association
         * attempts, so this does not create a tight loop even on a burst of
         * DISCONNECTED events. We never set WIFI_FAIL_BIT here: the caller's
         * timeout decides when to stop waiting. */
        esp_wifi_connect();
    }
}

esp_err_t wifi_sta_connect_from_nvs(int timeout_ms)
{
    nvs_handle_t nvs;
    esp_err_t r = nvs_open("wifi_creds", NVS_READONLY, &nvs);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "No stored WiFi credentials (NVS %s) -> need provisioning",
                 esp_err_to_name(r));
        return r;
    }

    char ssid[33] = {0};
    size_t len = sizeof(ssid);
    r = nvs_get_str(nvs, "ssid", ssid, &len);
    if (r != ESP_OK) {
        nvs_close(nvs);
        return r;
    }
    char pass[65] = {0};
    len = sizeof(pass);
    nvs_get_str(nvs, "pass", pass, &len); /* password may be empty (open AP) */
    nvs_close(nvs);

    ESP_LOGI(TAG, "Connecting to stored AP: %s", ssid);

    if (s_wifi_evt == NULL) {
        s_wifi_evt = xEventGroupCreate();
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    }

    if (!s_wifi_inited) {
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        s_wifi_inited = true;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, 32);
    strncpy((char *)wc.sta.password, pass, 64);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    /* esp_wifi_set_config persists the config to NVS internally. If the NVS
     * partition is FULL (repeated WiFi changes + chat history writes), that
     * returns ESP_ERR_NVS_NO_FREE_PAGES; the old ESP_ERROR_CHECK aborted and
     * the board reboot-looped. The config still applies in RAM, so treat a
     * persist failure as non-fatal (log it) and continue connecting. */
    esp_err_t set_cfg = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (set_cfg != ESP_OK) {
        ESP_LOGW(TAG, "wifi_set_config failed (%s) — NVS full? continuing in RAM",
                 esp_err_to_name(set_cfg));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    /* Wait up to `timeout_ms`. DISCONNECTED events keep reconnecting in the
     * background (see wifi_event_handler) and never set the FAIL bit, so this
     * wait only ends on a real GOT_IP or on timeout. */
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected (station)");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "WiFi connect failed/timeout after %d ms", timeout_ms);
    return ESP_FAIL;
}

/* ---- HTTP config page ---- */
static const char *PAGE_HTML =
    "<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>喵伴配网</title>"
    "<style>body{font-family:sans-serif;background:#101820;color:#fff;"
    "text-align:center;padding:24px}"
    "input{font-size:18px;padding:10px;width:80%;margin:8px 0;border-radius:8px}"
    "button{font-size:18px;padding:12px 28px;border:0;border-radius:8px;"
    "background:#4caf50;color:#fff;margin-top:12px}</style></head>"
    "<body><h2>喵伴 WiFi 配网</h2>"
    "<p>请输入要连接的 2.4GHz WiFi 名称和密码</p>"
    "<form action=\"/connect\" method=\"post\">"
    "<input name=\"ssid\" placeholder=\"WiFi 名称 (SSID)\" required><br>"
    "<input name=\"password\" type=\"password\" placeholder=\"WiFi 密码\"><br>"
    "<button type=\"submit\">保存并连接</button></form></body></html>";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static void url_decode(char *dst, const char *src, size_t dsize)
{
    size_t i = 0, j = 0;
    while (src[i] && j < dsize - 1) {
        if (src[i] == '+') {
            dst[j++] = ' ';
        } else if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) &&
                   isxdigit((unsigned char)src[i + 2])) {
            dst[j++] = (char)((hex_val(src[i + 1]) << 4) | hex_val(src[i + 2]));
            i += 2;
        } else {
            dst[j++] = src[i];
        }
        i++;
    }
    dst[j] = 0;
}

/* Parse "ssid=...&password=..." (url-encoded). */
static void parse_form(const char *body, char *ssid, char *pass)
{
    const char *p = body;
    while (*p) {
        const char *eq = strchr(p, '=');
        const char *amp = strchr(p, '&');
        if (!eq) break;
        size_t klen = (size_t)(eq - p);
        const char *val = eq + 1;
        size_t vlen = amp ? (size_t)(amp - val) : strlen(val);

        char key[32] = {0};
        char valbuf[96] = {0};
        if (klen >= sizeof(key)) klen = sizeof(key) - 1;
        memcpy(key, p, klen);
        size_t vcopy = vlen < sizeof(valbuf) - 1 ? vlen : sizeof(valbuf) - 1;
        memcpy(valbuf, val, vcopy);
        valbuf[vcopy] = 0;

        char decoded[96] = {0};
        url_decode(decoded, valbuf, sizeof(decoded));

        if (strcmp(key, "ssid") == 0) {
            strncpy(ssid, decoded, 32);
        } else if (strcmp(key, "password") == 0) {
            strncpy(pass, decoded, 64);
        }
        p = amp ? amp + 1 : "";
    }
}

static esp_err_t connect_post_handler(httpd_req_t *req)
{
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        return ESP_FAIL;
    }
    buf[len] = 0;

    char ssid[33] = {0};
    char pass[65] = {0};
    parse_form(buf, ssid, pass);

    esp_err_t r = ESP_FAIL;
    if (strlen(ssid) > 0) {
        nvs_handle_t nvs;
        if (nvs_open("wifi_creds", NVS_READWRITE, &nvs) == ESP_OK) {
            /* Check EVERY NVS call: if the partition is full (accumulated stale
             * pages after repeated WiFi re-provisioning), the write can fail
             * with ESP_ERR_NVS_NO_FREE_PAGES and silently leave old/empty creds,
             * which after the reboot would bounce straight back into
             * provisioning (the "saving the network then it crashes/loops"
             * symptom). Compact by erasing the namespace first, then store. */
            esp_err_t e1 = nvs_set_str(nvs, "ssid", ssid);
            esp_err_t e2 = nvs_set_str(nvs, "pass", pass);
            esp_err_t e3 = nvs_commit(nvs);
            if (e1 == ESP_ERR_NVS_NO_FREE_PAGES || e2 == ESP_ERR_NVS_NO_FREE_PAGES ||
                e3 == ESP_ERR_NVS_NO_FREE_PAGES) {
                ESP_LOGW(TAG, "wifi_creds NVS full (%s/%s/%s), compacting",
                         esp_err_to_name(e1), esp_err_to_name(e2), esp_err_to_name(e3));
                nvs_erase_all(nvs);
                nvs_commit(nvs);
                e1 = nvs_set_str(nvs, "ssid", ssid);
                e2 = nvs_set_str(nvs, "pass", pass);
                e3 = nvs_commit(nvs);
            }
            nvs_close(nvs);
            if (e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK) {
                r = ESP_OK;
                strlcpy(s_saved_ssid, ssid, sizeof(s_saved_ssid));
                ESP_LOGI(TAG, "Saved WiFi credentials for \"%s\"", ssid);
            } else {
                ESP_LOGE(TAG, "Failed to save WiFi credentials (%s/%s/%s)",
                         esp_err_to_name(e1), esp_err_to_name(e2), esp_err_to_name(e3));
            }
        }
    }

    const char *page_ok =
        "<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\">"
        "<title>已保存</title></head><body style=\"font-family:sans-serif;"
        "background:#101820;color:#fff;text-align:center;padding:32px\">"
        "<h2>已保存 ✓</h2><p>设备即将重启并连接 WiFi，请回到手机 WiFi 列表连接原网络。</p>"
        "</body></html>";
    const char *page_bad =
        "<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\">"
        "</head><body style=\"font-family:sans-serif;background:#101820;color:#fff;"
        "text-align:center;padding:32px\"><h2>缺少 WiFi 名称</h2>"
        "<p><a href=\"/\" style=\"color:#4caf50\">返回重试</a></p></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, (r == ESP_OK) ? page_ok : page_bad, HTTPD_RESP_USE_STRLEN);

    if (r == ESP_OK && s_prov_done) {
        xSemaphoreGive(s_prov_done);
    }
    return ESP_OK;
}

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;

    static const httpd_uri_t uris[] = {
        { .uri = "/",                .method = HTTP_GET,  .handler = root_get_handler },
        { .uri = "/connect",         .method = HTTP_POST, .handler = connect_post_handler },
        { .uri = "/generate_204",    .method = HTTP_GET,  .handler = root_get_handler },
        { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = root_get_handler },
        { .uri = "/ncsi.txt",        .method = HTTP_GET,  .handler = root_get_handler },
        { .uri = "/redirect",        .method = HTTP_GET,  .handler = root_get_handler },
    };

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
            httpd_register_uri_handler(server, &uris[i]);
        }
        ESP_LOGI(TAG, "HTTP config server started on 192.168.4.1");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}

/* ---- DNS captive-portal redirect (reply 192.168.4.1 for any A query) ---- */
static void dns_server_task(void *arg)
{
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        ESP_LOGE(TAG, "DNS socket create failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = inet_addr("192.168.4.1"),
    };
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(s);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS captive-portal redirect active");

    char buf[256];
    while (1) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&client, &clen);
        if (n < 12) continue;

        char resp[256];
        int rl = 0;
        memcpy(resp, buf, 2); rl = 2;                 /* transaction id */
        resp[2] = 0x81; resp[3] = 0x80; rl = 4;       /* QR=1 AA=1 RA=1 */
        resp[4] = buf[4]; resp[5] = buf[5];           /* qdcount (from query) */
        resp[6] = 0x00; resp[7] = 0x01;               /* ancount = 1 */
        resp[8] = 0x00; resp[9] = 0x00;               /* nscount = 0 */
        resp[10] = 0x00; resp[11] = 0x00; rl = 12;    /* arcount = 0 */

        int qend = 12;
        while (qend < n && buf[qend] != 0) {
            qend += (uint8_t)buf[qend] + 1;
        }
        qend += 5; /* terminating 0 + QTYPE(2) + QCLASS(2) */
        if (qend > n) qend = n;
        memcpy(resp + 12, buf + 12, qend - 12);
        rl = qend;

        /* Answer: pointer to question name, A record, TTL 60, 4-byte IPv4 */
        resp[rl++] = 0xC0; resp[rl++] = 0x0C;
        resp[rl++] = 0x00; resp[rl++] = 0x01;         /* type A */
        resp[rl++] = 0x00; resp[rl++] = 0x01;         /* class IN */
        resp[rl++] = 0x00; resp[rl++] = 0x00;
        resp[rl++] = 0x00; resp[rl++] = 0x3C;         /* TTL = 60 */
        resp[rl++] = 0x00; resp[rl++] = 0x04;         /* RDLENGTH = 4 */
        uint8_t ip[4] = {192, 168, 4, 1};
        memcpy(resp + rl, ip, 4);
        rl += 4;

        sendto(s, resp, rl, 0, (struct sockaddr *)&client, clen);
    }
}

static void draw_provisioning_screen(const char *ap_ssid)
{
    if (!lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        ESP_LOGW(TAG, "Provisioning screen: failed to acquire LVGL lock");
        return;
    }

    lv_obj_t *top = lv_layer_top();
    lv_obj_set_style_bg_color(top, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);

    lv_obj_t *label = lv_label_create(top);
    char txt[192];
    snprintf(txt, sizeof(txt),
             "配网模式\n\n请连接 WiFi：\n%s\n\n然后浏览器打开\n192.168.4.1",
             ap_ssid);
    lv_label_set_text(label, txt);
    lv_obj_set_style_text_font(label, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_HOR_RES > 40 ? LV_HOR_RES - 40 : 280);
    lv_obj_center(label);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "Provisioning screen drawn (AP=%s)", ap_ssid);
}

/* Full-screen "credentials saved" confirmation shown right before the reboot
 * into station mode, so the user gets visual feedback instead of the screen
 * jumping straight into the chat UI. */
static void draw_provisioning_done_screen(const char *ssid)
{
    if (!lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        return;
    }

    lv_obj_t *top = lv_layer_top();
    lv_obj_clean(top);
    lv_obj_set_style_bg_color(top, lv_color_hex(0x0E2A16), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);

    lv_obj_t *label = lv_label_create(top);
    char txt[160];
    snprintf(txt, sizeof(txt), "配网成功\n\n%s\n\n正在重启连接...", ssid);
    lv_label_set_text(label, txt);
    lv_obj_set_style_text_font(label, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_HOR_RES > 40 ? LV_HOR_RES - 40 : 280);
    lv_obj_center(label);

    lvgl_port_unlock();
}

/* Hide the provisioning overlay so the chat UI underneath becomes visible. */
void provisioning_hide_screen(void)
{
    if (!lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        return;
    }
    lv_obj_t *top = lv_layer_top();
    lv_obj_clean(top);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lvgl_port_unlock();
}

void provisioning_enter(void)
{
    ESP_LOGI(TAG, "Entering SoftAP provisioning mode");

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "Xiaozhi-%02X%02X", mac[4], mac[5]);

    if (s_wifi_inited) {
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (!s_wifi_inited) {
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        s_wifi_inited = true;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, ap_ssid, 32);
    ap.ap.ssid_len = (uint8_t)strlen(ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP '%s' up at 192.168.4.1", ap_ssid);

    start_http_server();
    xTaskCreate(dns_server_task, "dns_prov", 4096, NULL, 5, NULL);

    draw_provisioning_screen(ap_ssid);

    s_prov_done = xSemaphoreCreateBinary();
    xSemaphoreTake(s_prov_done, portMAX_DELAY);

    ESP_LOGI(TAG, "Provisioning complete, restarting into station mode");
    draw_provisioning_done_screen(s_saved_ssid[0] ? s_saved_ssid : "WiFi");
    vTaskDelay(pdMS_TO_TICKS(2500));
    esp_restart();
    /* never returns */
}

void wifi_creds_erase_and_reboot(void)
{
    ESP_LOGI(TAG, "Erasing stored WiFi credentials and rebooting");
    nvs_handle_t nvs;
    if (nvs_open("wifi_creds", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "WiFi credentials erased");
    } else {
        ESP_LOGW(TAG, "No wifi_creds namespace to erase");
    }
    /* Let the UI show a short confirmation before the reboot. */
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    /* never returns */
}
