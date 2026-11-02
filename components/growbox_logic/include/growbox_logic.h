/*
 * Pure grow-box logic: scheduling, sensor math, decoding. No ESP-IDF dependencies,
 * so it can be compiled and tested on the host.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GB_HEAT_HYSTERESIS_C 2.0f

#define GB_FLOOR_COUNT 3

/* Settings of one floor. */
typedef struct {
    bool enabled;                /* false: sensors not read, light forced off */
    bool auto_mode;
    bool manual_on;
    uint8_t manual_level;        /* 1..254 */
    uint8_t on_hour;             /* 0..23 */
    uint8_t off_hour;            /* 0..23 */
    uint8_t auto_brightness_pct; /* 1..100 */
    uint8_t max_temp_c;          /* 20..50 */
} gb_settings_t;

typedef struct {
    bool on;
    uint8_t level; /* 1..254 */
} gb_light_output_t;

typedef struct {
    int16_t ac1, ac2, ac3;
    uint16_t ac4, ac5, ac6;
    int16_t b1, b2, mb, mc, md;
} gb_bmp180_calib_t;

/* True when `hour` lies in [on_hour, off_hour), wrapping past midnight; on == off means never. */
bool gb_in_light_window(uint8_t hour, uint8_t on_hour, uint8_t off_hour);

/* Over-temperature latch: set above max, cleared at max - GB_HEAT_HYSTERESIS_C. */
bool gb_heat_lock_update(bool locked, bool temp_valid, float temp_c, float max_temp_c);

/*
 * Temperature that drives a floor's over-temperature cutoff: the floor's own reading, else the hottest valid
 * reading of the other floors. `valid`/`temp_c` have `count` entries. False when no reading is valid.
 */
bool gb_floor_control_temp(const bool *valid, const float *temp_c, uint8_t count, uint8_t floor, float *out);

/* Percent (clamped 1..100) to ZCL level 1..254. */
uint8_t gb_pct_to_level(uint8_t pct);

gb_light_output_t gb_light_decide(const gb_settings_t *s, bool time_valid, uint8_t hour, bool heat_lock);

/* Soil moisture in % x100 (0..10000); wetter = lower mV. */
uint16_t gb_soil_pct_x100(int mv, int dry_mv, int wet_mv);

/* TEMT6000 with 10 kOhm load: lux = V * 200. */
float gb_temt6000_lux(int mv);

/* ZCL Illuminance MeasuredValue encoding: 10000 * log10(lux) + 1. */
uint16_t gb_lux_to_zcl(float lux);

/* Bosch BMP180 datasheet compensation. temp in 0.1 C, pressure in Pa. */
void gb_bmp180_compensate(const gb_bmp180_calib_t *c, int32_t ut, int32_t up, uint8_t oss,
                          int32_t *temp_x10, int32_t *pressure_pa);

/* Decodes a 6-byte AHT10 measurement; false while busy. */
bool gb_aht10_convert(const uint8_t raw[6], float *temp_c, float *rh);

/* Decodes a 5-byte DHT11 frame; false on all-zero frame or checksum mismatch. */
bool gb_dht11_decode(const uint8_t d[5], float *temp_c, float *rh);

/* Hour of day from ZCL Time cluster LocalTime (seconds since 2000-01-01 local). */
uint8_t gb_hour_from_zcl_local(uint32_t zcl_local_secs);

#ifdef __cplusplus
}
#endif
