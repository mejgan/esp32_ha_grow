#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* One LEDC channel per floor, all on one timer. */
esp_err_t light_pwm_init(void);

/* level is a ZCL level 1..254; off forces duty 0. */
void light_pwm_set(uint8_t floor, bool on, uint8_t level);
