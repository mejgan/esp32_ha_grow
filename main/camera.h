/*
 * Optional Arducam Mega cameras, one per floor (camera N looks at floor N). While a floor's light is on, its camera
 * takes a photo every CONFIG_GROWBOX_PHOTO_INTERVAL_MIN minutes and uploads it over Wi-Fi to the photo server
 * (tools/photo_server.py); nothing is stored on the box.
 */
#pragma once

#include <stdint.h>

#include "sdkconfig.h"

#ifndef CONFIG_GROWBOX_CAMERA_COUNT
#define CONFIG_GROWBOX_CAMERA_COUNT 0
#endif

static inline uint8_t camera_count(void)
{
    return CONFIG_GROWBOX_CAMERA_COUNT;
}

/* Starts the camera task (no-op without cameras). */
void camera_start(void);

/* Takes and uploads a photo of `floor` as soon as possible, regardless of light and interval. */
void camera_request(uint8_t floor);
