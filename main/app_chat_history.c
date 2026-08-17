/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_chat_history.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "APP_CHAT_HISTORY";

#define NVS_NS      "chist"
#define META_KEY    "meta"
#define TURN_KEY(i) (("t" #i))   /* replaced below; we build keys dynamically */

/* Meta blob: count + head (head points at the next slot to write). */
typedef struct {
    uint16_t count;
    uint16_t head;
} hist_meta_t;

/* One turn as stored in NVS (packed, no padding assumptions relied upon). */
typedef struct {
    uint8_t  role;
    uint32_t timestamp;
    char     text[CHAT_HISTORY_TEXT_MAX + 1];
} hist_turn_raw_t;

static bool s_inited = false;
static nvs_handle_t s_nvs = 0;

static void make_turn_key(uint16_t slot, char *buf, size_t len)
{
    snprintf(buf, len, "t%02u", (unsigned)slot);
}

esp_err_t app_chat_history_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", NVS_NS, esp_err_to_name(ret));
        return ret;
    }
    s_inited = true;
    ESP_LOGI(TAG, "history store ready (cap=%d)", CHAT_HISTORY_CAP);
    return ESP_OK;
}

esp_err_t app_chat_history_append(chat_role_t role, const char *text)
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(text, ESP_ERR_INVALID_ARG, TAG, "null text");

    hist_meta_t meta = {0, 0};
    size_t mlen = sizeof(meta);
    if (nvs_get_blob(s_nvs, META_KEY, &meta, &mlen) != ESP_OK) {
        meta.count = 0;
        meta.head = 0;
    }

    hist_turn_raw_t raw;
    memset(&raw, 0, sizeof(raw));
    raw.role = (uint8_t)role;
    raw.timestamp = (uint32_t)time(NULL);
    strncpy(raw.text, text, CHAT_HISTORY_TEXT_MAX);
    raw.text[CHAT_HISTORY_TEXT_MAX] = '\0';

    /* NVS overwrites accumulate stale pages; when the partition fills up the
     * write fails with ESP_ERR_NVS_NO_FREE_PAGES. Recover by clearing the
     * namespace and re-appending — the transcript is only a local recall
     * mirror, losing it is fine and beats hard-failing. */
    char key[8];
    make_turn_key(meta.head, key, sizeof(key));
    esp_err_t r1 = nvs_set_blob(s_nvs, key, &raw, sizeof(raw));
    meta.head = (uint16_t)((meta.head + 1) % CHAT_HISTORY_CAP);
    if (meta.count < CHAT_HISTORY_CAP) {
        meta.count++;
    }
    esp_err_t r2 = nvs_set_blob(s_nvs, META_KEY, &meta, sizeof(meta));
    esp_err_t rc = nvs_commit(s_nvs);
    if (r1 == ESP_ERR_NVS_NO_FREE_PAGES || r2 == ESP_ERR_NVS_NO_FREE_PAGES ||
        rc == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_LOGW(TAG, "NVS full (%s/%s/%s), compacting history",
                 esp_err_to_name(r1), esp_err_to_name(r2), esp_err_to_name(rc));
        nvs_erase_all(s_nvs);
        nvs_commit(s_nvs);
        /* Re-write just this one turn so the newest message is kept. */
        meta.count = 1;
        meta.head = 0;
        make_turn_key(0, key, sizeof(key));
        nvs_set_blob(s_nvs, key, &raw, sizeof(raw));
        nvs_set_blob(s_nvs, META_KEY, &meta, sizeof(meta));
        nvs_commit(s_nvs);
    } else if (r1 != ESP_OK || r2 != ESP_OK || rc != ESP_OK) {
        ESP_LOGW(TAG, "history write failed (%s/%s/%s)",
                 esp_err_to_name(r1), esp_err_to_name(r2), esp_err_to_name(rc));
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "appended %s turn (count=%u): %.40s",
             role == CHAT_ROLE_USER ? "user" : (role == CHAT_ROLE_ASSISTANT ? "assistant" : "system"),
             meta.count, text);
    return ESP_OK;
}

size_t app_chat_history_count(void)
{
    if (!s_inited) {
        return 0;
    }
    hist_meta_t meta = {0, 0};
    size_t mlen = sizeof(meta);
    if (nvs_get_blob(s_nvs, META_KEY, &meta, &mlen) != ESP_OK) {
        return 0;
    }
    return meta.count;
}

esp_err_t app_chat_history_get(size_t index, chat_turn_t *out)
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "null out");

    hist_meta_t meta = {0, 0};
    size_t mlen = sizeof(meta);
    if (nvs_get_blob(s_nvs, META_KEY, &meta, &mlen) != ESP_OK || meta.count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (index >= meta.count) {
        return ESP_ERR_NOT_FOUND;
    }

    /* oldest turn sits at (head - count), newest at (head - 1). */
    uint16_t slot = (uint16_t)((meta.head - meta.count + index + CHAT_HISTORY_CAP * 2)
                               % CHAT_HISTORY_CAP);
    char key[8];
    make_turn_key(slot, key, sizeof(key));

    hist_turn_raw_t raw;
    memset(&raw, 0, sizeof(raw));
    size_t rlen = sizeof(raw);
    esp_err_t ret = nvs_get_blob(s_nvs, key, &raw, &rlen);
    ESP_RETURN_ON_ERROR(ret, TAG, "get turn %s failed", key);

    out->role = (chat_role_t)raw.role;
    out->timestamp = raw.timestamp;
    strncpy(out->text, raw.text, CHAT_HISTORY_TEXT_MAX);
    out->text[CHAT_HISTORY_TEXT_MAX] = '\0';
    return ESP_OK;
}

esp_err_t app_chat_history_clear(void)
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    hist_meta_t meta = {0, 0};
    size_t mlen = sizeof(meta);
    if (nvs_get_blob(s_nvs, META_KEY, &meta, &mlen) == ESP_OK && meta.count > 0) {
        for (uint16_t i = 0; i < meta.count; ++i) {
            uint16_t slot = (uint16_t)((meta.head - meta.count + i + CHAT_HISTORY_CAP * 2)
                                       % CHAT_HISTORY_CAP);
            char key[8];
            make_turn_key(slot, key, sizeof(key));
            nvs_erase_key(s_nvs, key);
        }
    }
    meta.count = 0;
    meta.head = 0;
    ESP_RETURN_ON_ERROR(nvs_set_blob(s_nvs, META_KEY, &meta, sizeof(meta)), TAG, "reset meta failed");
    ESP_RETURN_ON_ERROR(nvs_commit(s_nvs), TAG, "commit failed");
    return ESP_OK;
}
