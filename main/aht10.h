#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

/* One AHT10 (fixed address 0x38), so each instance needs its own I2C bus. */
typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    bool ready;
} aht10_t;

esp_err_t aht10_init(aht10_t *aht, i2c_master_bus_handle_t bus);

/* Re-initializes first if the previous init or read failed. */
esp_err_t aht10_read(aht10_t *aht, float *temp_c, float *rh);
