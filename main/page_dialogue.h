/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "ui_page_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Read-only dialogue / transcript page. Shows the recent conversation (user +
 * assistant + system turns) with timestamps, a live status line (listening /
 * speaking / error), and auto-scrolls to the newest message. The full history
 * is persisted in flash by app_chat_history. */
extern const ui_page_t page_dialogue;

#ifdef __cplusplus
}
#endif
