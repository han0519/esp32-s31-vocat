#include "page_face.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "app_sfx.h"

#define TAG "PAGE_FACE"

/* This page is the FACE mode host for the official emote engine
 * (esp_emote_expression). The emote renderer owns the LCD panel at 25fps while
 * this page is active (see esp_xiaozhi_chat_display_enable_emote / set_face_visible).
 * The page itself only turns taps / long-presses into petting reactions and
 * forwards chat emotions to the emote engine (handled by the display layer). */

esp_err_t page_face_on_enter(void)
{
    return ESP_OK;
}

esp_err_t page_face_on_exit(void)
{
    return ESP_OK;
}

void page_face_on_tick(uint32_t ms)
{
    (void)ms;
}

bool page_face_on_gesture(ui_gesture_t g)
{
    if (g == UI_GESTURE_TAP) {
        app_sfx_play(APP_SFX_TAP);
        extern void esp_xiaozhi_chat_display_pet(void);
        esp_xiaozhi_chat_display_pet();
        return true;
    }
    if (g == UI_GESTURE_LONG_PRESS) {
        app_sfx_play(APP_SFX_PAGE);
        extern void esp_xiaozhi_chat_display_laugh(void);
        esp_xiaozhi_chat_display_laugh();
        return true;
    }
    /* Swipe up from the face opens the app-grid launcher (桌面). */
    if (g == UI_GESTURE_SWIPE_UP) {
        extern esp_err_t ui_page_manager_switch(ui_page_id_t);
        ui_page_manager_switch(PAGE_LAUNCHER);
        return true;
    }
    return false;
}

/* Show the AI's reply / the user's words below the emote face. The official
 * emote engine renders this as a scrolling toast under the eyes. */
void page_face_on_chat(chat_role_t role, const char *text)
{
    extern void esp_xiaozhi_chat_display_set_chat_message(const char *role, const char *content);
    if (role == CHAT_ROLE_USER) {
        esp_xiaozhi_chat_display_set_chat_message("user", text);
    } else if (role == CHAT_ROLE_ASSISTANT) {
        esp_xiaozhi_chat_display_set_chat_message("assistant", text);
    } else {
        esp_xiaozhi_chat_display_set_chat_message("system", text);
    }
}

const ui_page_t page_face = {
    .id        = PAGE_FACE,
    .name      = "face",
    .on_enter  = page_face_on_enter,
    .on_exit   = page_face_on_exit,
    .on_tick   = page_face_on_tick,
    .on_gesture= page_face_on_gesture,
    .on_chat   = page_face_on_chat,
};
