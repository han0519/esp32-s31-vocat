/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tiny local sound-effects player for the pet UI. Tones are generated in
 * software (no extra flash assets) and written straight to the ES8389 play
 * device, so the touch interactions and boot self-test have audible feedback.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_codec_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_SFX_TAP = 0,    /* short click for tap / page change */
    APP_SFX_BOOT,       /* boot self-test chime */
    APP_SFX_PAGE,       /* page swipe */
    APP_SFX_ALARM,      /* timer alarm: 3x rising beep */
    APP_SFX_MAX,
} app_sfx_t;

/**
 * @brief  Register the open playback codec device and its sample rate.
 *         Called by chat_app once the audio pipeline is up.
 */
void app_sfx_set_play_dev(esp_codec_dev_handle_t play_dev, int sample_rate);

/**
 * @brief  Play a named effect. No-op if the play device is not registered.
 */
void app_sfx_play(app_sfx_t sfx);

#ifdef __cplusplus
}
#endif
