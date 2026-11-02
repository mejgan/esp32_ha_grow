#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

esp_err_t dht11_read(gpio_num_t pin, float *temp_c, float *rh);
