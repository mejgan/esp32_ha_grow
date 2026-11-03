/*
 * Drivers for the digital sensors a floor or the room sensor can use: temperature / humidity, pressure and the
 * BH1750 light sensor. One sensor_dev_t per physical sensor; its kind, bus, address or pin come from menuconfig.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dht.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "growbox_logic.h"
#include "sensors.h"

/* Numbers 1-9 match the *_CLIMATE_TYPE / *_PRESSURE_TYPE / ROOM_TYPE options in main/Kconfig.projbuild. */
typedef enum {
    SENSOR_DEV_NONE = 0,
    SENSOR_DEV_DHT11 = 1,
    SENSOR_DEV_DHT22 = 2,  /* also AM2302 */
    SENSOR_DEV_AHT = 3,    /* AHT10 / AHT20 / AHT21, 0x38 */
    SENSOR_DEV_SHT3X = 4,  /* 0x44 / 0x45 */
    SENSOR_DEV_SHT4X = 5,  /* 0x44 */
    SENSOR_DEV_HTU21D = 6, /* also SHT21, Si7021; 0x40 */
    SENSOR_DEV_BME280 = 7, /* 0x76 / 0x77; temperature, humidity, pressure */
    SENSOR_DEV_BMP180 = 8, /* 0x77; temperature, pressure */
    SENSOR_DEV_BMP280 = 9, /* 0x76 / 0x77; temperature, pressure (a BME280's humidity is ignored) */
    SENSOR_DEV_BH1750 = 10,
} sensor_dev_kind_t;

typedef struct {
    sensor_dev_kind_t kind;
    sensors_bus_t bus;
    uint8_t addr;
    dht_t dht; /* DHT kinds: pin and type */
    i2c_master_dev_handle_t dev;
    bool ready;
    bool has_humidity; /* BME280: false when the chip turns out to be a BMP280 */
    union {
        gb_bme280_calib_t bme;
        gb_bmp180_calib_t bmp180;
    } calib;
} sensor_dev_t;

#define SENSOR_DEV(kind_, bus_, addr_, gpio_)                                                                    \
    {                                                                                                            \
        .kind = (sensor_dev_kind_t)(kind_), .bus = (sensors_bus_t)(bus_), .addr = (uint8_t)(addr_),              \
        .dht = DHT_INIT((gpio_), (kind_) == SENSOR_DEV_DHT22 ? DHT_TYPE_DHT22 : DHT_TYPE_DHT11),                 \
    }

/* SENSOR_CAP_* a sensor of this kind delivers. */
uint8_t sensor_dev_caps(sensor_dev_kind_t kind);

/* Short name for logs, e.g. "SHT3x". */
const char *sensor_dev_name(sensor_dev_kind_t kind);

bool sensor_dev_is_i2c(sensor_dev_kind_t kind);

/* Adds an I2C sensor to its bus and sets the chip up. No-op for SENSOR_DEV_NONE and DHT sensors. Call after the
 * buses exist; a failed init is retried on the next read. */
esp_err_t sensor_dev_init(sensor_dev_t *d);

/* One measurement. Sets the fields the sensor measures (temperature, humidity, pressure, light) and their valid
 * flags in `out`; leaves the other fields alone. */
esp_err_t sensor_dev_read(sensor_dev_t *d, floor_readings_t *out);
