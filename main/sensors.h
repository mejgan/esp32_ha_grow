#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Sensor placement (floor index 0 = floor 1, the bottom):
 *   floor 1: DHT11 (temp/hum), capacitive soil
 *   floor 2: AHT10 #1 on LP I2C (temp/hum), YL-69 soil
 *   floor 3: AHT10 #2 + BMP180 on I2C (temp/hum, pressure), TEMT6000 (light)
 */
typedef enum {
    SENSOR_CAP_HUMIDITY = 1 << 0,
    SENSOR_CAP_PRESSURE = 1 << 1,
    SENSOR_CAP_LUX = 1 << 2,
    SENSOR_CAP_SOIL = 1 << 3,
} sensor_cap_t;

typedef struct {
    bool temp_valid;
    float temp_c;
    bool rh_valid;
    float rh;
    bool pressure_valid;
    float pressure_hpa;
    bool lux_valid;
    float lux;
    bool soil_valid;
    int soil_mv;
    uint16_t soil_pct_x100;
} floor_readings_t;

esp_err_t sensors_init(void);

/* Bitmask of sensor_cap_t present on `floor` (temperature is always present). */
uint8_t sensors_floor_caps(uint8_t floor);

/* Reads every sensor of `floor` and logs one line, including raw soil mV for calibration. */
void sensors_read_floor(uint8_t floor, floor_readings_t *out);
