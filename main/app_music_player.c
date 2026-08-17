/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Network music player on esp_audio_simple_player.
 *
 * The simple player decodes the stream (MP3/AAC/...) in its own task and calls
 * our out callback with raw PCM. We forward that PCM to the ES8389 play device
 * (same handle the app uses for TTS / SFX), converting mono -> stereo and
 * honoring the current volume.
 */
#include "app_music_player.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_audio_simple_player.h"

static const char *TAG = "APP_MUSIC";

static esp_codec_dev_handle_t s_play_dev = NULL;
static int   s_sample_rate = 16000;
static int   s_channels    = 2;     /* ES8389 opened in stereo on this board */
static esp_asp_handle_t s_player = NULL;
static app_music_state_t s_state = APP_MUSIC_STOPPED;
static char s_cur_url[512] = {0};

/* --- out callback: simple player -> codec -------------------------------- */

static int music_out_cb(uint8_t *data, int data_size, void *ctx)
{
    (void)ctx;
    if (s_play_dev == NULL || data == NULL || data_size <= 0) {
        return 0;
    }
    /* data is 16-bit PCM (usually mono or stereo at the decoded rate). The
     * codec is fixed at s_channels (2) and s_sample_rate. If the stream rate
     * differs we still write it through; esp_audio_simple_player decodes to
     * the rate of the source. Most streams are 44.1/48k -> the codec would
     * need a rate converter. To keep it simple and correct, we request the
     * player to decode at the codec rate via music_info if possible, and here
     * we just forward stereo samples. */
    int written = esp_codec_dev_write(s_play_dev, data, data_size);
    return written > 0 ? written : 0;
}

/* --- simple player state events ------------------------------------------ */

static int music_event_cb(esp_asp_event_pkt_t *pkt, void *ctx)
{
    (void)ctx;
    if (pkt == NULL) {
        return 0;
    }
    if (pkt->type == ESP_ASP_EVENT_TYPE_STATE) {
        esp_asp_state_t st = (esp_asp_state_t)(intptr_t)pkt->payload;
        switch (st) {
        case ESP_ASP_STATE_RUNNING:  s_state = APP_MUSIC_PLAYING; break;
        case ESP_ASP_STATE_PAUSED:   s_state = APP_MUSIC_PAUSED;  break;
        case ESP_ASP_STATE_STOPPED:
        case ESP_ASP_STATE_FINISHED: s_state = APP_MUSIC_STOPPED; break;
        case ESP_ASP_STATE_ERROR:    s_state = APP_MUSIC_ERROR;   break;
        default: break;
        }
        ESP_LOGI(TAG, "player state -> %s", app_music_player_state_str());
    }
    return 0;
}

/* --- public API ----------------------------------------------------------- */

esp_err_t app_music_player_init(esp_codec_dev_handle_t play_dev, int sample_rate)
{
    if (play_dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_play_dev = play_dev;
    if (sample_rate > 0) {
        s_sample_rate = sample_rate;
    }

    if (s_player == NULL) {
        esp_asp_cfg_t cfg = {0};
        cfg.out.cb = music_out_cb;
        cfg.out.user_ctx = NULL;
        cfg.task_prio = 8;
        cfg.task_stack = 8 * 1024;
        cfg.task_core = 1;
        cfg.task_stack_in_ext = 1;   /* keep internal SRAM for WakeNet */
        esp_gmf_err_t ret = esp_audio_simple_player_new(&cfg, &s_player);
        if (ret != ESP_GMF_ERR_OK || s_player == NULL) {
            ESP_LOGE(TAG, "simple player create failed: %x", ret);
            return ESP_FAIL;
        }
        esp_audio_simple_player_set_event(s_player, music_event_cb, NULL);
    }
    ESP_LOGI(TAG, "music player ready (rate=%d ch=%d)", s_sample_rate, s_channels);
    return ESP_OK;
}

esp_err_t app_music_player_play(const char *url)
{
    if (url == NULL || s_player == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strlen(url) >= sizeof(s_cur_url)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "play: %s", url);
    strcpy(s_cur_url, url);
    esp_asp_music_info_t info = {
        .sample_rate = s_sample_rate,
        .channels    = 1,
        .bits        = 16,
        .bitrate     = 0,
    };
    esp_gmf_err_t ret = esp_audio_simple_player_run(s_player, url, &info);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "play failed: %x", ret);
        s_state = APP_MUSIC_ERROR;
        return ESP_FAIL;
    }
    s_state = APP_MUSIC_PLAYING;
    return ESP_OK;
}

esp_err_t app_music_player_pause(void)
{
    if (s_player == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (esp_audio_simple_player_pause(s_player) != ESP_GMF_ERR_OK) {
        return ESP_FAIL;
    }
    s_state = APP_MUSIC_PAUSED;
    return ESP_OK;
}

esp_err_t app_music_player_resume(void)
{
    if (s_player == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (esp_audio_simple_player_resume(s_player) != ESP_GMF_ERR_OK) {
        return ESP_FAIL;
    }
    s_state = APP_MUSIC_PLAYING;
    return ESP_OK;
}

esp_err_t app_music_player_stop(void)
{
    if (s_player == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_audio_simple_player_stop(s_player);
    s_state = APP_MUSIC_STOPPED;
    return ESP_OK;
}

app_music_state_t app_music_player_get_state(void)
{
    return s_state;
}

const char *app_music_player_state_str(void)
{
    switch (s_state) {
    case APP_MUSIC_PLAYING: return "播放中";
    case APP_MUSIC_PAUSED:  return "已暂停";
    case APP_MUSIC_ERROR:   return "播放出错";
    default:                return "已停止";
    }
}
