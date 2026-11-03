#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

/*
 * Floor sensors, each picked in menuconfig (Grow Box -> Floor N): a temperature / humidity sensor, a pressure
 * sensor, a soil moisture sensor and a light sensor, each optional. Floor index 0 = floor 1, the bottom. The
 * defaults are floor 1: DHT11 + capacitive soil; floor 2: AHT10 + YL-69 soil; floor 3: AHT10 + BMP180 + TEMT6000.
 */
typedef enum {
    SENSOR_CAP_HUMIDITY = 1 << 0,
    SENSOR_CAP_PRESSURE = 1 << 1,
    SENSOR_CAP_LUX = 1 << 2,
    SENSOR_CAP_SOIL = 1 << 3,
    SENSOR_CAP_TEMP = 1 << 4,
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

/* Values match the *_BUS options in main/Kconfig.projbuild. */
typedef enum {
    SENSORS_BUS_MAIN = 0, /* HP I2C, pins in Grow Box -> I2C buses */
    SENSORS_BUS_LP = 1,   /* LP I2C, fixed GPIO6 SDA / GPIO7 SCL */
} sensors_bus_t;

/* Creates the I2C buses that are in use, then sets up every configured sensor (floors and room). */
esp_err_t sensors_init(void);

/* I2C bus created by sensors_init() (NULL if unused or failed), so other devices such as a display can share it. */
i2c_master_bus_handle_t sensors_i2c_bus(sensors_bus_t bus);

/* True when a configured sensor (floor or room) has `addr` on `bus`; the LCD address auto-detect skips these. */
bool sensors_i2c_addr_in_use(sensors_bus_t bus, uint8_t addr);

/* Bitmask of sensor_cap_t present on `floor`. */
uint8_t sensors_floor_caps(uint8_t floor);

/* True when at least one floor has a temperature reading source (needed for the over-temperature cutoff). */
bool sensors_any_temp(void);

/* Reads every sensor of `floor` and logs one line, including the raw soil mV for calibration. */
void sensors_read_floor(uint8_t floor, floor_readings_t *out);

/* True when any sensor of `floor` gave no reading in `r`. */
bool sensors_floor_fault(uint8_t floor, const floor_readings_t *r);
