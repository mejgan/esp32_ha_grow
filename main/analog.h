#pragma once

#include "esp_err.h"

/* Creates the ADC unit. Channels are set up on their first read. */
esp_err_t analog_init(void);

/* Calibrated voltage on `gpio` (an ADC pin, GPIO0-6) in mV, averaged over 16 samples. */
esp_err_t analog_read_mv(int gpio, int *mv);
