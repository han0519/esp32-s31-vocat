/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "ui_page_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Music player page (placeholder skeleton). */
extern const ui_page_t page_music;

/* AI / voice music control.
 * app_music_intent() scans free text for a play/search command and copies the
 *   song name into out (may be empty when the user just said "play music").
 * app_music_play_by_name() jumps to the music page and starts playback: first
 *   matching an SD-card track by name, then falling back to an online search.
 * Returns ESP_OK if playback was started. */
bool app_music_intent(const char *text, char *out, size_t n);
esp_err_t app_music_play_by_name(const char *name);

#ifdef __cplusplus
}
#endif
