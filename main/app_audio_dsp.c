/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include "esp_ae_types.h"
#include "esp_ae_drc.h"
#include "esp_ae_eq.h"
#include "esp_ae_howl.h"
#include "esp_ae_alc.h"

#include "app_audio_dsp.h"

static const char *TAG = "APP_AUDIO_DSP";

/* Max HOWL frame: 1024 samples * 2 ch * 2 bytes (16 kHz max block length). */
#define APP_AUDIO_DSP_HOWL_MAX_FRAME (1024 * 2 * 2)

/* Pre-AFE mic boost (VoCat-S31): the codec PGA is already at its 36.5 dB
 * hardware max and esp-sr AGC runs after WakeNet, so we scale the raw samples
 * here (app_audio_dsp_mic_input_cb) to give WakeNet a usable level.
 * +24 dB = 16x, applied with int16 clipping. (+30 dB clipped close/loud
 * speech hard at 0 dBFS, which distorts the wake word; 16x avoids that while
 * AEC is off so no cancellation eats the signal.)
 * While the on-board speaker is playing TTS (s_bargein_playing), the mic
 * picks up the speaker echo at up to ~-40 dBFS raw; with no AEC that echo is
 * uploaded, the server ASR hears the device's own voice and answers it ->
 * self-triggering conversation loop. The boost is therefore set to 0 during
 * playback (the upload carries silence), killing the loop. Trade-off: user
 * speech is not heard while TTS is playing (no barge-in). */
#define APP_AUDIO_DSP_MIC_BOOST_FACTOR     16.0f
#define APP_AUDIO_DSP_MIC_BOOST_FACTOR_PB  0.0f

/* ---------------------------------------------------------------------------
 * Module state
 * ------------------------------------------------------------------------- */
static bool                 s_pb_inited   = false;
static uint32_t             s_pb_rate     = 16000;
static uint8_t              s_pb_ch       = 2;
static uint8_t              s_pb_bits     = 16;
static esp_ae_drc_handle_t  s_drc         = NULL;
static esp_ae_eq_handle_t   s_eq          = NULL;
static esp_ae_howl_handle_t s_howl        = NULL;
static esp_ae_alc_handle_t  s_alc         = NULL;
static uint32_t             s_howl_frame  = 0;

/* Energy meters (smoothed linear RMS, 0..~32768) */
static float s_pb_rms = 0;      /* playback (speaker) energy */
static float s_mic_rms = 0;     /* mic energy AFTER the +24dB pre-AFE boost */
static float s_mic_rms_raw = 0; /* mic energy BEFORE the boost (used by barge-in) */

/* TTS playback flow diagnostic (logged ~1 Hz so we can tell "no audio arrived"
 * from "audio arrived but speaker silent" without flooding the log). */
static int64_t s_pb_log_t0   = 0;
static float   s_pb_log_acc  = 0;
static int     s_pb_log_n    = 0;
static int     s_pb_frames   = 0;

/* Barge-in */
static bool                           s_bargein_playing = false;
static app_audio_bargein_interrupt_cb_t s_bargein_cb = NULL;
static int64_t                        s_play_start_ms = 0;
static uint32_t                        s_bargein_lockout_ms = 350;   /* ignore VAD right after playback onset */
static bool                           s_last_vad = false;

/* Playback-activity gating for the mic mute: the last time (ms) playback data
 * was written to the codec. The mic boost stays at 0 for a short window after
 * the last write so the TTS tail buffer / speaker decay is never picked up by
 * the mic and uploaded (that caused "the device hears its own voice" loops).
 * 900 ms covers a couple of decoder frames + speaker decay. */
static int64_t s_pb_last_activity_ms = -900000;
#define APP_AUDIO_DSP_PB_MUTE_TAIL_MS 900

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */
static inline int64_t app_audio_dsp_now_ms(void)
{
    return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static float app_audio_dsp_rms_16(const int16_t *pcm, size_t samples)
{
    if (samples == 0) {
        return 0;
    }
    int64_t energy = 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t v = pcm[i];
        energy += (int64_t)v * v;
    }
    return (float)sqrt((double)energy / (double)samples);
}

/* ---------------------------------------------------------------------------
 * Playback chain
 * ------------------------------------------------------------------------- */
esp_err_t app_audio_dsp_init_playback(uint32_t sample_rate, uint8_t channels, uint8_t bits)
{
    if (s_pb_inited) {
        app_audio_dsp_deinit_playback();
    }
    s_pb_rate = sample_rate;
    s_pb_ch   = channels;
    s_pb_bits = bits;

    esp_ae_err_t ae = ESP_AE_ERR_OK;
    esp_err_t ret = ESP_OK;

    /* 1) DRC (peak limiter) is DISABLED to cut per-block CPU cost; the ES8389
     *    DAC volume already provides the output level, and a compressor on
     *    TTS speech is not needed. Keeping the DSP chain minimal avoids
     *    audio stutter on the S31. */

    /* 2) EQ: single high-shelf to cut the high-frequency hiss (滋滋声) only.
     *    One filter instead of three keeps the per-block cost low while still
     *    cleaning the tone. */
    static esp_ae_eq_filter_para_t eq_para[] = {
        {.filter_type = ESP_AE_EQ_FILTER_HIGH_SHELF,.fc = 8000, .q = 0.707f, .gain = -8.0f},
    };
    esp_ae_eq_cfg_t eq_cfg = {
        .sample_rate     = sample_rate,
        .channel         = channels,
        .bits_per_sample = ESP_AE_BIT16,
        .filter_num      = sizeof(eq_para) / sizeof(eq_para[0]),
        .para            = eq_para,
    };
    ae = esp_ae_eq_open(&eq_cfg, &s_eq);
    if (ae != ESP_AE_ERR_OK) {
        ESP_LOGE(TAG, "EQ open failed: %d", ae);
        ret = ESP_FAIL; goto _err;
    }

    /* 3) HOWL: FFT-based howling / acoustic-feedback suppression is DISABLED.
     * It runs on the playback (TTS) path, and with AEC off the on-board mic
     * picks up the speaker echo — HOWL then sees a "feedback loop", mis-detects
     * it and throttles the TTS output, causing choppy/stuttering speech. */
    s_howl = NULL;
    s_howl_frame = 0;

    /* 4) ALC: automatic level control / slight makeup so the limiter above does
     *    not make the overall output feel quieter. +6 dB makeup helps the small
     *    on-board speaker sound louder (VoCat-S31 PA gain is set by resistors). */
    esp_ae_alc_cfg_t alc_cfg = {
        .sample_rate = sample_rate,
        .channel     = channels,
        .bits_per_sample = ESP_AE_BIT16,
    };
    ae = esp_ae_alc_open(&alc_cfg, &s_alc);
    if (ae != ESP_AE_ERR_OK) {
        ESP_LOGE(TAG, "ALC open failed: %d", ae);
        ret = ESP_FAIL; goto _err;
    }
    for (uint8_t c = 0; c < channels; c++) {
        esp_ae_alc_set_gain(s_alc, c, 6);
    }

    s_pb_inited = true;
    ESP_LOGI(TAG, "Playback DSP chain ready: rate=%u ch=%u bits=%u (HOWL disabled: echo mis-detect caused TTS choppiness)",
             sample_rate, channels, bits);
    return ESP_OK;

_err:
    app_audio_dsp_deinit_playback();
    return ret;
}

void app_audio_dsp_deinit_playback(void)
{
    if (s_drc)  { esp_ae_drc_close(s_drc);  s_drc = NULL; }
    if (s_eq)   { esp_ae_eq_close(s_eq);   s_eq = NULL; }
    if (s_howl) { esp_ae_howl_close(s_howl); s_howl = NULL; }
    if (s_alc)  { esp_ae_alc_close(s_alc);  s_alc = NULL; }
    s_howl_frame = 0;
    s_pb_inited = false;
}

void __attribute__((weak)) app_audio_dsp_playback_run(uint8_t *data, int len)
{
    if (!s_pb_inited || data == NULL || len <= 0) {
        return;
    }
    /* Record playback activity for the mic-mute tail gate. */
    s_pb_last_activity_ms = app_audio_dsp_now_ms();

    /* Keep the NS4150B PA (GPIO2) asserted HIGH. The app owns GPIO2 outright
     * (ES8389 no longer drives it), so this is just a one-time belt-and-suspenders
     * guard that also confirms, at the first TTS frame, that the pin really reads
     * HIGH — i.e. the speaker is enabled and audio is reaching it. */
    static bool s_pa_forced = false;
    if (!s_pa_forced) {
        gpio_set_direction(2, GPIO_MODE_OUTPUT);
        esp_err_t e = gpio_set_level(2, 1);
        ESP_LOGI(TAG, "[TTS_AUDIO] PA_GPIO2 guard set HIGH ret=%s lvl=%d",
                 esp_err_to_name(e), gpio_get_level(2));
        s_pa_forced = true;
    }

    /* Meter playback energy for the barge-in controller (always, 16-bit). */
    if (s_pb_bits == 16) {
        float rms = app_audio_dsp_rms_16((const int16_t *)data, len / 2);
        s_pb_rms = s_pb_rms * 0.9f + rms * 0.1f;

        /* ~1 Hz diagnostic so we can see whether TTS audio is actually reaching
         * the codec (non-zero RMS) versus arriving silent. */
        s_pb_log_acc += rms;
        s_pb_log_n++;
        s_pb_frames++;
        int64_t now = app_audio_dsp_now_ms();
        if (s_pb_log_t0 == 0) {
            s_pb_log_t0 = now;
        }
        if (now - s_pb_log_t0 >= 1000) {
            float avg = s_pb_log_n ? s_pb_log_acc / s_pb_log_n : 0;
            float db  = (avg > 0.5f) ? (20.0f * log10f(avg / 32768.0f)) : -99.0f;
            ESP_LOGI(TAG, "[TTS_AUDIO] frames=%d avg_rms=%.0f ~%.1f dBFS",
                     s_pb_frames, avg, db);
            s_pb_log_t0 = now;
            s_pb_log_acc = 0;
            s_pb_log_n = 0;
            s_pb_frames = 0;
        }
    }

    if (s_pb_bits != 16) {
        /* Chain runs on 16-bit PCM; skip DSP but keep metering above. */
        return;
    }

    uint32_t sample_num = (uint32_t)(len / (s_pb_ch * (16 >> 3)));
    uint8_t *p = data;

    /* EQ -> ALC: arbitrary length, in place. DRC is disabled (NULL) to keep
     * the per-block cost low and avoid audio stutter. */
    if (s_drc) {
        esp_ae_drc_process(s_drc, sample_num, p, p);
    }
    esp_ae_eq_process(s_eq, sample_num, p, p);
    esp_ae_alc_process(s_alc, sample_num, p, p);

    /* HOWL: frame-aligned. Pad a trailing partial frame with silence so no
     * samples are dropped (no cross-call carry buffer is possible because the
     * caller writes exactly `data` to the codec right after this returns). */
    if (s_howl && s_howl_frame > 0) {
        int off = 0;
        while (off + (int)s_howl_frame <= len) {
            esp_ae_howl_process(s_howl, p + off, p + off);
            off += s_howl_frame;
        }
        int rem = len - off;
        if (rem > 0) {
            uint8_t tmp[APP_AUDIO_DSP_HOWL_MAX_FRAME];
            if ((uint32_t)rem <= sizeof(tmp) && (uint32_t)(rem + s_howl_frame) <= sizeof(tmp)) {
                memcpy(tmp, p + off, rem);
                memset(tmp + rem, 0, s_howl_frame - rem);
                esp_ae_howl_process(s_howl, tmp, tmp);
                memcpy(p + off, tmp, rem);
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Record path + barge-in
 * ------------------------------------------------------------------------- */
void app_audio_dsp_mic_input_cb(uint8_t *data, int data_size, void *ctx)
{
    (void)ctx;
    if (data == NULL || data_size <= 0) {
        return;
    }
    /* Meter RAW (pre-boost) mic energy (16-bit interleaved). Used by the
     * barge-in controller to distinguish real user speech from speaker
     * playback picked up by the mics, and for the ~1 Hz live diagnostic.
     * This runs BEFORE the boost below, so barge-in thresholds and historical
     * MIC_LIVE readings stay comparable. s_mic_rms_raw feeds the barge-in
     * comparison (the boosted value would falsely trip "user is speaking" on
     * the device's own TTS echo). */
    if (data_size >= 4) {
        float rms = app_audio_dsp_rms_16((const int16_t *)data, data_size / 2);
        s_mic_rms = s_mic_rms * 0.85f + rms * 0.15f;
        s_mic_rms_raw = s_mic_rms_raw * 0.85f + rms * 0.15f;

        /* ~1 Hz diagnostic: shows the LIVE mic level reaching the AFE during
         * idle (pre-wake) too, so we can tell "mic dead / not fed" (rms stays
         * ~0..20) from "mic live but WakeNet won't trigger" (rms jumps when you
         * speak). Peak of 0 at all times = I2S RX / recorder feed is broken. */
        static int64_t s_mic_log_t0 = 0;
        int64_t now = app_audio_dsp_now_ms();
        if (s_mic_log_t0 == 0) {
            s_mic_log_t0 = now;
        }
        if (now - s_mic_log_t0 >= 1000) {
            float db = (s_mic_rms > 0.5f) ? (20.0f * log10f(s_mic_rms / 32768.0f)) : -99.0f;
            ESP_LOGI(TAG, "[MIC_LIVE] rms=%.0f ~%.1f dBFS", s_mic_rms, db);
            s_mic_log_t0 = now;
        }
    }

    /* === VoCat-S31 pre-AFE mic boost ===
     * The on-board mic only reaches ~-55 dBFS RMS during speech even with the
     * ES8389 PGA already at its 36.5 dB hardware ceiling, and esp-sr's
     * AGC/afe_linear_gain act AFTER WakeNet, so WakeNet sees a signal roughly
     * 25-35 dB too quiet to trigger reliably. The recorder feeds the AFE
     * straight from this buffer (recorder_inport_acquire_read -> input_cb ->
     * fifo -> ai_afe), so we scale the samples in-place, BEFORE the AFE sees
     * them.
     * +30 dB = 32x, clamped to int16. The AEC reference channel (ch1, the
     * DAC-loopback) is near-silent during idle/wake so boosting it is harmless
     * here; we also log ch0 vs ch1 peaks separately to confirm the channel
     * mapping (which sample slot carries the mic). */
    {
        int16_t *s = (int16_t *)data;
        int n = data_size / 2;              /* total 16-bit samples (stereo) */
        int32_t peak0 = 0, peak1 = 0;
        /* While the speaker is playing (or just finished) TTS, the mic picks up
         * the device's own echo; mute the boost so the echo never reaches the
         * upload. The gate is based on actual playback activity + a tail window,
         * not just the barge-in flag, so the TTS tail buffer and speaker decay
         * are covered too. */
        int64_t pb_age = app_audio_dsp_now_ms() - s_pb_last_activity_ms;
        bool pb_mute = (s_bargein_playing || pb_age < APP_AUDIO_DSP_PB_MUTE_TAIL_MS);
        float factor = pb_mute ? APP_AUDIO_DSP_MIC_BOOST_FACTOR_PB
                               : APP_AUDIO_DSP_MIC_BOOST_FACTOR;
        for (int i = 0; i < n; i++) {
            int32_t v = (int32_t)s[i] * factor;
            v = (v > 32767) ? 32767 : (v < -32768) ? -32768 : v;
            s[i] = (int16_t)v;
            int32_t a = (v < 0) ? -v : v;
            if (i & 1) {
                if (a > peak1) peak1 = a;   /* ch1 (odd sample) */
            } else {
                if (a > peak0) peak0 = a;   /* ch0 (even sample) */
            }
        }
        /* ~1 Hz diagnostic: what WakeNet actually receives after the boost,
         * split per channel so we can see which I2S slot carries the mic. */
        static int64_t s_mic_boost_t0 = 0;
        int64_t now = app_audio_dsp_now_ms();
        if (s_mic_boost_t0 == 0) {
            s_mic_boost_t0 = now;
        }
        if (now - s_mic_boost_t0 >= 1000) {
            float db0 = (peak0 > 1) ? (20.0f * log10f((float)peak0 / 32768.0f)) : -99.0f;
            float db1 = (peak1 > 1) ? (20.0f * log10f((float)peak1 / 32768.0f)) : -99.0f;
            ESP_LOGI(TAG, "[MIC_BOOST] ch0=%.0f(~%.1f dBFS) ch1=%.0f(~%.1f dBFS) x%.0f=+%.0f dB%s",
                     (float)peak0, db0, (float)peak1, db1, factor,
                     20.0f * log10f(factor),
                     pb_mute ? " (mute)" : "");
            s_mic_boost_t0 = now;
        }
    }
}

float app_audio_dsp_get_playback_rms(void) { return s_pb_rms; }
float app_audio_dsp_get_mic_rms(void)     { return s_mic_rms; }

void app_audio_bargein_register_interrupt_cb(app_audio_bargein_interrupt_cb_t cb)
{
    s_bargein_cb = cb;
}

void app_audio_bargein_set_playing(bool playing)
{
    s_bargein_playing = playing;
    if (playing) {
        s_play_start_ms = app_audio_dsp_now_ms();
    }
}

void app_audio_bargein_on_vad(bool vad_active)
{
    if (vad_active == s_last_vad) {
        return;
    }
    s_last_vad = vad_active;
    if (!vad_active) {
        return;
    }

    /* A VAD start during playback is only a real barge-in if the user's speech
     * clearly exceeds the speaker playback level (otherwise it is just the
     * speaker echo) and we are past the playback-onset lock-out. */
    if (!s_bargein_playing) {
        return;
    }
    if (app_audio_dsp_now_ms() - s_play_start_ms < s_bargein_lockout_ms) {
        return;
    }

    /* Compare the RAW (un-boosted) mic level against the speaker level.
     * With the +24 dB pre-AFE boost, the boosted mic value would always exceed
     * the playback echo and falsely interrupt TTS on the device's own voice;
     * the raw value keeps the comparison physical. Require the user's speech
     * to be ~10 dB (3.2x) above playback (was 2x = 6 dB, too twitchy). */
    float pb = s_pb_rms;
    float mic = s_mic_rms_raw;
    bool real_speech = (pb < 1.0f) ? (mic > 500.0f) : (mic > pb * 3.2f);
    if (real_speech && s_bargein_cb) {
        ESP_LOGI(TAG, "Barge-in: mic_raw_rms=%.0f playback_rms=%.0f -> interrupt TTS", mic, pb);
        s_bargein_cb();
    } else {
        ESP_LOGD(TAG, "Barge-in ignored (echo): mic_raw_rms=%.0f playback_rms=%.0f", mic, pb);
    }
}

void app_audio_bargein_reset(void)
{
    s_bargein_playing = false;
    s_last_vad = false;
    s_play_start_ms = 0;
}
