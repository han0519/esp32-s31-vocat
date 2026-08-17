/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Network music player built on esp_audio_simple_player. Decoded PCM is routed
 * to the ES8389 codec via the same play device the app owns, so music plays
 * through the on-board speaker without disturbing the AFE/TTS pipeline setup.
 */
#pragma once

#include "esp_err.h"
#include "esp_codec_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_MUSIC_STOPPED = 0,
    APP_MUSIC_PLAYING,
    APP_MUSIC_PAUSED,
    APP_MUSIC_ERROR,
} app_music_state_t;

/**
 * @brief  Initialize the music player. Must be called after the codec play
 *         device is open (esp_xiaozhi_chat_app_audio).
 *
 * @param[in] play_dev   ES8389 codec device used for output
 * @param[in] sample_rate  codec sample rate (e.g. 16000)
 * @return ESP_OK on success
 */
esp_err_t app_music_player_init(esp_codec_dev_handle_t play_dev, int sample_rate);

/**
 * @brief  Start playing a stream by URL (https://...mp3 etc).
 */
esp_err_t app_music_player_play(const char *url);

/**
 * @brief  Pause / resume current stream.
 */
esp_err_t app_music_player_pause(void);
esp_err_t app_music_player_resume(void);

/**
 * @brief  Stop and release the current stream.
 */
esp_err_t app_music_player_stop(void);

/**
 * @brief  Current state.
 */
app_music_state_t app_music_player_get_state(void);

/**
 * @brief  Human-readable state string (for the music page / MCP status).
 */
const char *app_music_player_state_str(void);

#ifdef __cplusplus
}
#endif
