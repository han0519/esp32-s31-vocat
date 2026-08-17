/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-device conversation history store for the ESP-VoCat-S31 "喵伴" pet.
 *
 * Keeps a fixed-size ring buffer of recent conversation turns (user / assistant
 * / system) in NVS so the dialogue page can show a transcript with timestamps
 * and survive reboots. This is a read-only mirror of what the Xiaozhi cloud
 * already keeps server-side; the device stores it for local recall only.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHAT_ROLE_USER = 0,
    CHAT_ROLE_ASSISTANT,
    CHAT_ROLE_SYSTEM,
} chat_role_t;

/* Max turns kept in the ring buffer. Kept small (10) so the NVS-backed store
 * uses bounded memory; when the buffer is full a new turn evicts the OLDEST
 * one (ring behaviour, see app_chat_history_append). */
#define CHAT_HISTORY_CAP       10
/* Max bytes of a single turn's text (excluding NUL). */
#define CHAT_HISTORY_TEXT_MAX  240

typedef struct {
    chat_role_t role;
    uint32_t    timestamp;              /* unix seconds when the turn arrived */
    char        text[CHAT_HISTORY_TEXT_MAX + 1];
} chat_turn_t;

/**
 * @brief  Initialize the history store (open NVS namespace "chist").
 *         Safe to call multiple times; only the first call opens NVS.
 */
esp_err_t app_chat_history_init(void);

/**
 * @brief  Append a turn to the ring buffer.
 *
 * @param role   Who said it.
 * @param text   NUL-terminated UTF-8 text (will be truncated to
 *               CHAT_HISTORY_TEXT_MAX if longer).
 */
esp_err_t app_chat_history_append(chat_role_t role, const char *text);

/**
 * @brief  Number of turns currently stored (0 .. CHAT_HISTORY_CAP).
 */
size_t app_chat_history_count(void);

/**
 * @brief  Read a stored turn by index. Index 0 is the OLDEST turn,
 *         count-1 is the newest. Out-of-range index returns ESP_ERR_NOT_FOUND.
 */
esp_err_t app_chat_history_get(size_t index, chat_turn_t *out);

/**
 * @brief  Clear all stored history.
 */
esp_err_t app_chat_history_clear(void);

#ifdef __cplusplus
}
#endif
