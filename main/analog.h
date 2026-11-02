#pragma once

#include "esp_err.h"

typedef enum {
    ANALOG_SOIL_CAP,
    ANALOG_SOIL_RES,
    ANALOG_LIGHT,
    ANALOG_COUNT,
} analog_ch_t;

esp_err_t analog_init(void);

/* Calibrated voltage in mV, averaged over 16 samples. */
esp_err_t analog_read_mv(analog_ch_t ch, int *mv);
