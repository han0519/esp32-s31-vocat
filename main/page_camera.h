/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "ui_page_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Camera preview page (placeholder). The real SC101IOT preview + MJPEG server
 * is wired in by the camera-driver task; for now it shows a placeholder. */
extern const ui_page_t page_camera;

#ifdef __cplusplus
}
#endif
