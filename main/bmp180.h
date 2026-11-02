#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t bmp180_init(i2c_master_bus_handle_t bus);
esp_err_t bmp180_read(float *temp_c, float *pressure_hpa);
