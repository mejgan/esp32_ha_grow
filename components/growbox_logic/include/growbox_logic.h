/*
 * Pure grow-box logic: scheduling, sensor math, decoding. No ESP-IDF dependencies,
 * so it can be compiled and tested on the host (see test/host).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GB_HEAT_HYSTERESIS_C 2.0f

#define GB_FLOOR_COUNT 3

#define GB_MINUTES_PER_DAY 1440u

/* ZCL Time cluster counts seconds from 2000-01-01 00:00; this is that instant as a Unix timestamp. */
#define GB_ZCL_EPOCH_UNIX 946684800u

/* Settings of one floor. */
typedef struct {
    bool enabled;                /* false: sensors not read, light forced off */
    bool auto_mode;
    bool manual_on;
    uint8_t manual_level;        /* 1..254 */
    uint8_t on_hour;             /* 0..23 */
    uint8_t on_minute;           /* 0..59 */
    uint8_t off_hour;            /* 0..23 */
    uint8_t off_minute;          /* 0..59 */
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

/* Clamps every field to its valid range (booleans to 0/1). */
void gb_settings_clamp(gb_settings_t *s);

static inline uint16_t gb_on_minute_of_day(const gb_settings_t *s)
{
    return (uint16_t)(s->on_hour * 60u + s->on_minute);
}

static inline uint16_t gb_off_minute_of_day(const gb_settings_t *s)
{
    return (uint16_t)(s->off_hour * 60u + s->off_minute);
}

/* True when `now` lies in [on, off), all minutes of the day, wrapping past midnight; on == off means never. */
bool gb_in_light_window(uint16_t now, uint16_t on, uint16_t off);

/* Over-temperature latch: set above max, cleared at max - GB_HEAT_HYSTERESIS_C. */
bool gb_heat_lock_update(bool locked, bool temp_valid, float temp_c, float max_temp_c);

/*
 * Temperature that drives a floor's over-temperature cutoff: the floor's own reading, else the hottest valid
 * reading of the other floors. `valid`/`temp_c` have `count` entries. False when no reading is valid.
 */
bool gb_floor_control_temp(const bool *valid, const float *temp_c, uint8_t count, uint8_t floor, float *out);

/* Percent (clamped 1..100) to ZCL level 1..254. */
uint8_t gb_pct_to_level(uint8_t pct);

/* ZCL level 1..254 to percent 1..100 (0 stays 0). */
uint8_t gb_level_to_pct(uint8_t level);

/* `minute_of_day` is only used when `time_valid`. */
gb_light_output_t gb_light_decide(const gb_settings_t *s, bool time_valid, uint16_t minute_of_day, bool heat_lock);

/* Minute of the day (0..1439) of a non-negative seconds count whose day starts at a multiple of 86400. */
uint16_t gb_minute_of_day(int64_t secs);

/* Soil moisture in % x100 (0..10000); wetter = lower mV. */
uint16_t gb_soil_pct_x100(int mv, int dry_mv, int wet_mv);

/* TEMT6000 with 10 kOhm load: lux = V * 200. */
float gb_temt6000_lux(int mv);

/* BH1750 high-resolution result (2 bytes, MSB first) in lux: raw / 1.2. */
float gb_bh1750_lux(const uint8_t raw[2]);

/* ZCL Illuminance MeasuredValue encoding: 10000 * log10(lux) + 1. */
uint16_t gb_lux_to_zcl(float lux);

/* Bosch BMP180 datasheet compensation. temp in 0.1 C, pressure in Pa. */
void gb_bmp180_compensate(const gb_bmp180_calib_t *c, int32_t ut, int32_t up, uint8_t oss,
                          int32_t *temp_x10, int32_t *pressure_pa);

/* Decodes a 6-byte AHT10 measurement; false while busy. */
bool gb_aht10_convert(const uint8_t raw[6], float *temp_c, float *rh);

/* Decodes a 5-byte DHT11 frame; false on all-zero frame or checksum mismatch. */
bool gb_dht11_decode(const uint8_t d[5], float *temp_c, float *rh);

/* Decodes a 5-byte DHT22/AM2302 frame (0.1 resolution, sign bit); false on all-zero frame or checksum mismatch. */
bool gb_dht22_decode(const uint8_t d[5], float *temp_c, float *rh);

/* Sensirion CRC-8 (poly 0x31, init 0xFF) used by SHT3x/SHT4x. */
uint8_t gb_crc8_sensirion(const uint8_t *data, size_t len);

/* SHT3x / SHT4x measurement: temp MSB, LSB, CRC, humidity MSB, LSB, CRC. False on CRC mismatch. */
bool gb_sht3x_convert(const uint8_t raw[6], float *temp_c, float *rh);
bool gb_sht4x_convert(const uint8_t raw[6], float *temp_c, float *rh);

/* HTU21D / SHT21 / Si7021: 16-bit value + CRC (poly 0x31, init 0x00). False on CRC mismatch. */
uint8_t gb_crc8_htu21(const uint8_t *data, size_t len);
bool gb_htu21_temp(const uint8_t raw[3], float *temp_c);
bool gb_htu21_rh(const uint8_t raw[3], float *rh);

/* Bosch BME280 / BMP280 calibration (registers 0x88..0xA1 and, BME280 only, 0xE1..0xE7). */
typedef struct {
    uint16_t t1;
    int16_t t2, t3;
    uint16_t p1;
    int16_t p2, p3, p4, p5, p6, p7, p8, p9;
    uint8_t h1, h3;
    int16_t h2, h4, h5;
    int8_t h6;
} gb_bme280_calib_t;

/* `tp` = 26 bytes from 0x88, `h` = 7 bytes from 0xE1 (NULL for a BMP280). */
void gb_bme280_parse_calib(const uint8_t tp[26], const uint8_t *h, gb_bme280_calib_t *c);

/* Bosch reference integer compensation. Temperature in 0.01 C, pressure in Pa * 256, humidity in % * 1024. */
void gb_bme280_compensate(const gb_bme280_calib_t *c, int32_t adc_t, int32_t adc_p, int32_t adc_h, int32_t *temp_x100,
                          uint32_t *pressure_q24_8, uint32_t *rh_q22_10);

/* One level of the DHT11 data line and how long it lasted. */
typedef struct {
    uint8_t level;
    uint16_t us;
} gb_pulse_t;

/*
 * Rebuilds the 5 DHT11/DHT22 frame bytes from captured line pulses. Every bit is a ~50 us low followed by a high of
 * ~26 us (0) or ~70 us (1); the last 40 such low/high pairs are the data, so a missed start or response does not
 * matter. False when fewer than 40 bits were captured. The checksum is not checked here (see gb_dht11_decode).
 */
bool gb_dht11_frame_from_pulses(const gb_pulse_t *p, size_t n, uint8_t frame[5]);

/* ---------- Zigbee OTA upgrade file (ZCL spec 11.4) ---------- */

#define GB_OTA_FILE_ID       0x0BEEF11Eu
#define GB_OTA_HEADER_MIN    56u
#define GB_OTA_TAG_UPGRADE   0x0000u

typedef enum {
    GB_OTA_OK = 0,
    GB_OTA_ERR_MAGIC,    /* not a Zigbee OTA file */
    GB_OTA_ERR_HEADER,   /* header length or total size inconsistent */
    GB_OTA_ERR_OVERFLOW, /* more data than the header announced */
    GB_OTA_ERR_SINK,     /* the image sink reported an error */
} gb_ota_err_t;

/* Receives bytes of the upgrade image (tag 0x0000) in order; returns 0 on success. */
typedef int (*gb_ota_sink_t)(void *ctx, const uint8_t *data, size_t len);

typedef struct {
    int state;
    uint32_t pos;         /* file bytes consumed */
    uint8_t buf[GB_OTA_HEADER_MIN];
    uint8_t fill;         /* bytes collected in buf */
    uint16_t header_len;
    uint32_t total_size;  /* whole file, from the header */
    uint16_t manuf_code;
    uint16_t image_type;
    uint32_t file_version;
    uint16_t tag;         /* current sub-element */
    uint32_t remaining;   /* bytes left in the current header or sub-element */
    uint32_t image_bytes; /* upgrade image bytes passed to the sink */
} gb_ota_parser_t;

void gb_ota_parser_init(gb_ota_parser_t *p);

/* Feeds the next chunk of the OTA file. Sub-elements other than the upgrade image are skipped. */
gb_ota_err_t gb_ota_parser_feed(gb_ota_parser_t *p, const uint8_t *data, size_t len, gb_ota_sink_t sink, void *ctx);

/* True once the whole file announced by the header has been consumed. */
bool gb_ota_parser_done(const gb_ota_parser_t *p);

#ifdef __cplusplus
}
#endif
