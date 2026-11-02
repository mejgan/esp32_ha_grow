#pragma once

#include "esp_err.h"
#include "growbox_logic.h"

/* Loads persisted per-floor settings, falling back to Kconfig defaults; values are clamped to valid ranges. */
void settings_load(gb_settings_t out[GB_FLOOR_COUNT]);
esp_err_t settings_save(const gb_settings_t floors[GB_FLOOR_COUNT]);
