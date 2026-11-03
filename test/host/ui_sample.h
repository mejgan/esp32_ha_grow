/* A representative status used by the UI tests and previews. */
#pragma once

#include "growbox_ui.h"

static inline gb_ui_status_t ui_sample_status(void)
{
    gb_ui_status_t st = {
        .joined = true,
        .time_valid = true,
        .minute_of_day = 13 * 60 + 5,
    };
    st.floor[0] = (gb_ui_floor_t){
        .enabled = true,
        .light_on = true,
        .level_pct = 80,
        .auto_mode = true,
        .on_min = 6 * 60 + 30,
        .off_min = 0,
        .auto_brightness_pct = 80,
        .max_temp_c = 35,
        .has_humidity = true,
        .has_soil = true,
        .temp_valid = true,
        .rh_valid = true,
        .soil_valid = true,
        .temp_c = 24.5f,
        .rh = 55.0f,
        .soil_pct = 43.2f,
    };
    st.floor[1] = (gb_ui_floor_t){
        .enabled = true,
        .light_on = false,
        .level_pct = 100,
        .auto_mode = false,
        .on_min = 6 * 60,
        .off_min = 22 * 60,
        .auto_brightness_pct = 100,
        .max_temp_c = 30,
        .heat_lock = true,
        .has_humidity = true,
        .has_soil = true,
        .temp_valid = true,
        .rh_valid = true,
        .soil_valid = true,
        .temp_c = 31.2f,
        .rh = 48.0f,
        .soil_pct = 61.0f,
    };
    st.floor[2] = (gb_ui_floor_t){
        .enabled = false,
        .auto_mode = true,
        .on_min = 6 * 60,
        .off_min = 0,
        .auto_brightness_pct = 100,
        .max_temp_c = 35,
        .has_humidity = true,
        .has_pressure = true,
        .has_lux = true,
        .temp_valid = true,
        .rh_valid = true,
        .pressure_valid = true,
        .lux_valid = true,
        .temp_c = 26.0f,
        .rh = 40.0f,
        .pressure_hpa = 1013.0f,
        .lux = 452.0f,
    };
    return st;
}
