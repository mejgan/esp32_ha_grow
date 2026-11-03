/*
 * Wi-Fi station used only for photo uploads: switched on for an upload and off again, so the shared radio is
 * free for Zigbee the rest of the time.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t wifi_link_init(void);

/* Starts Wi-Fi and waits up to `timeout_ms` for an IP address. */
esp_err_t wifi_link_up(uint32_t timeout_ms);

void wifi_link_down(void);
