/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Audio DSP chain for the ESP-VoCat-S31 (喵伴) board.
 *
 * Hardware provides the raw audio capability (ES8389 codec + NS4150B PA +
 * dual differential electret mics MIC1P/N and MIC2P/N). This module is the
 * "Audio DSP" layer that shapes the final listening experience:
 *
 *   Playback (speaker) path:
 *     DRC (peak limiter) -> EQ (tone shape + hiss cut) -> HOWL (howling
 *     suppression) -> ALC (output level control)
 *   Record (mic) path:
 *     energy metering only (AFE owns the actual mic processing); the meter
 *     feeds the barge-in controller.
 *
 * All ESP-Audio-Effects modules are from espressif__esp_audio_effects and are
 * verified to support ESP32-S31.
 */

/* ---------------------------------------------------------------------------
 * Playback (speaker) path
 * ------------------------------------------------------------------------- */

/**
 * @brief  Initialise the playback DSP chain.
 *
 * @param[in]  sample_rate  Playback sample rate (e.g. 16000).
 * @param[in]  channels     Playback channel count (board uses 2).
 * @param[in]  bits         Playback bits per sample. The chain runs on 16-bit
 *                         PCM; if the board delivers a different width the
 *                         chain is bypassed (but the RMS meter still updates).
 */
esp_err_t app_audio_dsp_init_playback(uint32_t sample_rate, uint8_t channels, uint8_t bits);

/** Deinitialise and free the playback DSP chain. */
void app_audio_dsp_deinit_playback(void);

/**
 * @brief  Process one decoded playback frame in place.
 *
 * Called by av_processor (patched feeder_outport_release_write) immediately
 * before esp_codec_dev_write(). Declared as a weak symbol so the firmware
 * links even if the patch is absent; the real implementation runs the
 * ESP-Audio-Effects chain. Safe to call before init (no-op).
 *
 * @param[in,out]  data  PCM buffer (16-bit interleaved expected).
 * @param[in]      len   Buffer length in bytes.
 */
void app_audio_dsp_playback_run(uint8_t *data, int len);

/* ---------------------------------------------------------------------------
 * Record (mic) path + barge-in
 * ------------------------------------------------------------------------- */

/**
 * @brief  Recorder input callback (recorder_config.input_cb).
 *
 * Receives the raw codec PCM right after esp_codec_dev_read(), before the AFE.
 * Meters mic energy for the barge-in controller. Does NOT modify the samples
 * so the AFE always sees the original signal.
 */
void app_audio_dsp_mic_input_cb(uint8_t *data, int data_size, void *ctx);

/** Smoothed playback RMS in linear amplitude (0..~32768). */
float app_audio_dsp_get_playback_rms(void);

/** Smoothed mic RMS in linear amplitude (0..~32768). */
float app_audio_dsp_get_mic_rms(void);

/**
 * @brief  Barge-in (real-time voice interruption) controller.
 *
 * Without a hardware AEC reference the speaker playback is picked up by the
 * mics, so a naive VAD would falsely trigger during TTS. The controller only
 * flags a real barge-in when user speech energy clearly exceeds the speaker
 * playback energy, with a short lock-out after playback starts.
 */
typedef void (*app_audio_bargein_interrupt_cb_t)(void);

/** Register the callback invoked when a real barge-in is detected. */
void app_audio_bargein_register_interrupt_cb(app_audio_bargein_interrupt_cb_t cb);

/** Tell the controller whether TTS is currently playing. */
void app_audio_bargein_set_playing(bool playing);

/** Feed a VAD transition (true = speech start, false = speech end). */
void app_audio_bargein_on_vad(bool vad_active);

/** Reset barge-in state (e.g. on audio re-init). */
void app_audio_bargein_reset(void);

#ifdef __cplusplus
}
#endif
