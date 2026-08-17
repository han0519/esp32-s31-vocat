/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_sfx.h"

#include <math.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "APP_SFX";

static esp_codec_dev_handle_t s_play_dev = NULL;
static int s_sample_rate = 16000;
static int s_channels = 1;

void app_sfx_set_play_dev(esp_codec_dev_handle_t play_dev, int sample_rate)
{
    s_play_dev = play_dev;
    if (sample_rate > 0) {
        s_sample_rate = sample_rate;
    }
    /* The ES8389 play device is opened with 2 channels on this board. */
    s_channels = 2;
}

static void play_tone(float freq, int ms, float vol)
{
    if (s_play_dev == NULL) {
        return;
    }
    int n = (s_sample_rate * ms) / 1000;
    if (n <= 0) {
        return;
    }
    size_t bytes = (size_t)n * s_channels * sizeof(int16_t);
    int16_t *buf = (int16_t *)malloc(bytes);
    if (buf == NULL) {
        return;
    }
    const float two_pi = 2.0f * (float)M_PI;
    for (int i = 0; i < n; ++i) {
        float t = (float)i / (float)s_sample_rate;
        /* Short attack/decay envelope to avoid clicks. */
        float env = vol;
        if (i < n * 0.1f) {
            env = vol * (float)i / (n * 0.1f);
        } else if (i > n * 0.8f) {
            env = vol * (float)(n - i) / (n * 0.2f);
        }
        int16_t s = (int16_t)(sinf(two_pi * freq * t) * 32767.0f * env);
        for (int c = 0; c < s_channels; ++c) {
            buf[i * s_channels + c] = s;
        }
    }
    esp_codec_dev_write(s_play_dev, buf, (int)bytes);
    free(buf);
}

void app_sfx_play(app_sfx_t sfx)
{
    switch (sfx) {
    case APP_SFX_TAP:
        play_tone(880.0f, 50, 0.12f);
        break;
    case APP_SFX_PAGE:
        play_tone(523.0f, 45, 0.10f);
        break;
    case APP_SFX_BOOT:
        play_tone(660.0f, 80, 0.18f);
        vTaskDelay(pdMS_TO_TICKS(90));
        play_tone(990.0f, 130, 0.18f);
        break;
    case APP_SFX_ALARM:
        play_tone(880.0f, 160, 0.22f);
        vTaskDelay(pdMS_TO_TICKS(120));
        play_tone(990.0f, 160, 0.22f);
        vTaskDelay(pdMS_TO_TICKS(120));
        play_tone(1175.0f, 220, 0.24f);
        break;
    default:
        break;
    }
}
