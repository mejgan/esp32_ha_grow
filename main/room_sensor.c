#include "room_sensor.h"

#include "sdkconfig.h"

#if CONFIG_GROWBOX_ROOM_NONE

esp_err_t room_sensor_init(void)
{
    return ESP_OK;
}

uint8_t room_sensor_caps(void)
{
    return 0;
}

void room_sensor_read(floor_readings_t *out)
{
    *out = (floor_readings_t){0};
}

#else

#include "delay.h"
#include "esp_log.h"
#include "growbox_logic.h"

static const char *TAG = "room";

/* ---------------------------------------------------------------------------------------------------------- */
#if CONFIG_GROWBOX_ROOM_DHT22 || CONFIG_GROWBOX_ROOM_DHT11

#include "dht.h"

#if CONFIG_GROWBOX_ROOM_DHT22
static dht_t s_dht = DHT_INIT(CONFIG_GROWBOX_ROOM_GPIO, DHT_TYPE_DHT22);
#else
static dht_t s_dht = DHT_INIT(CONFIG_GROWBOX_ROOM_GPIO, DHT_TYPE_DHT11);
#endif

static esp_err_t sensor_init(void)
{
    return ESP_OK; /* the RMT channel is set up on the first read */
}

static esp_err_t sensor_read(floor_readings_t *out)
{
    esp_err_t err = dht_read(&s_dht, &out->temp_c, &out->rh);
    out->temp_valid = out->rh_valid = err == ESP_OK;
    return err;
}

/* ---------------------------------------------------------------------------------------------------------- */
#else /* I2C sensors */

#include "driver/i2c_master.h"

#define TIMEOUT_MS 100

#if CONFIG_GROWBOX_ROOM_I2C_BUS_LP
#define ROOM_BUS SENSORS_BUS_LP
#else
#define ROOM_BUS SENSORS_BUS_MAIN
#endif

static i2c_master_dev_handle_t s_dev;
static bool s_ready;

static esp_err_t write_bytes(const uint8_t *b, size_t n)
{
    return i2c_master_transmit(s_dev, b, n, TIMEOUT_MS);
}

#if CONFIG_GROWBOX_ROOM_BME280
static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, TIMEOUT_MS);
}
#endif

static esp_err_t device_setup(void);

static esp_err_t sensor_init(void)
{
    if (s_dev == NULL) {
        i2c_master_bus_handle_t bus = sensors_i2c_bus(ROOM_BUS);
        if (bus == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = CONFIG_GROWBOX_ROOM_I2C_ADDR,
            .scl_speed_hz = 100000,
        };
        esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
        if (err != ESP_OK) {
            s_dev = NULL;
            return err;
        }
    }
    esp_err_t err = device_setup();
    s_ready = err == ESP_OK;
    return err;
}

#if CONFIG_GROWBOX_ROOM_SHT3X || CONFIG_GROWBOX_ROOM_SHT4X

static esp_err_t device_setup(void)
{
#if CONFIG_GROWBOX_ROOM_SHT3X
    const uint8_t reset[] = {0x30, 0xA2};
#else
    const uint8_t reset[] = {0x94};
#endif
    esp_err_t err = write_bytes(reset, sizeof(reset));
    delay_at_least_ms(2);
    return err;
}

static esp_err_t measure(floor_readings_t *out)
{
#if CONFIG_GROWBOX_ROOM_SHT3X
    const uint8_t cmd[] = {0x24, 0x00}; /* single shot, high repeatability, no clock stretching */
    const uint32_t wait_ms = 16;
#else
    const uint8_t cmd[] = {0xFD}; /* high precision */
    const uint32_t wait_ms = 10;
#endif
    esp_err_t err = write_bytes(cmd, sizeof(cmd));
    if (err != ESP_OK) {
        return err;
    }
    delay_at_least_ms(wait_ms);
    uint8_t raw[6];
    err = i2c_master_receive(s_dev, raw, sizeof(raw), TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
#if CONFIG_GROWBOX_ROOM_SHT3X
    bool ok = gb_sht3x_convert(raw, &out->temp_c, &out->rh);
#else
    bool ok = gb_sht4x_convert(raw, &out->temp_c, &out->rh);
#endif
    out->temp_valid = out->rh_valid = ok;
    return ok ? ESP_OK : ESP_ERR_INVALID_CRC;
}

#elif CONFIG_GROWBOX_ROOM_HTU21D

static esp_err_t device_setup(void)
{
    const uint8_t reset[] = {0xFE};
    esp_err_t err = write_bytes(reset, sizeof(reset));
    delay_at_least_ms(15);
    return err;
}

static esp_err_t read_word(uint8_t cmd, uint32_t wait_ms, uint8_t raw[3])
{
    esp_err_t err = write_bytes(&cmd, 1); /* "no hold master" measurement */
    if (err != ESP_OK) {
        return err;
    }
    delay_at_least_ms(wait_ms);
    return i2c_master_receive(s_dev, raw, 3, TIMEOUT_MS);
}

static esp_err_t measure(floor_readings_t *out)
{
    uint8_t raw[3];
    esp_err_t err = read_word(0xF3, 50, raw);
    if (err == ESP_OK) {
        out->temp_valid = gb_htu21_temp(raw, &out->temp_c);
        err = read_word(0xF5, 16, raw);
    }
    if (err == ESP_OK) {
        out->rh_valid = gb_htu21_rh(raw, &out->rh);
    }
    if (err == ESP_OK && !(out->temp_valid && out->rh_valid)) {
        err = ESP_ERR_INVALID_CRC;
    }
    return err;
}

#elif CONFIG_GROWBOX_ROOM_BME280

static gb_bme280_calib_t s_calib;
static bool s_has_humidity;

static esp_err_t device_setup(void)
{
    uint8_t id = 0;
    esp_err_t err = read_regs(0xD0, &id, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (id != 0x60 && id != 0x58) {
        ESP_LOGW(TAG, "unexpected chip id 0x%02x (BME280 0x60, BMP280 0x58)", id);
        return ESP_ERR_NOT_FOUND;
    }
    s_has_humidity = id == 0x60;
    const uint8_t reset[] = {0xE0, 0xB6};
    err = write_bytes(reset, sizeof(reset));
    delay_at_least_ms(5);
    uint8_t tp[26], h[7];
    if (err == ESP_OK) {
        err = read_regs(0x88, tp, sizeof(tp));
    }
    if (err == ESP_OK && s_has_humidity) {
        err = read_regs(0xE1, h, sizeof(h));
    }
    if (err == ESP_OK) {
        gb_bme280_parse_calib(tp, s_has_humidity ? h : NULL, &s_calib);
    }
    return err;
}

static esp_err_t measure(floor_readings_t *out)
{
    /* Forced mode, x1 oversampling: one measurement, then the chip sleeps (no self-heating). */
    const uint8_t hum[] = {0xF2, 0x01};
    const uint8_t meas[] = {0xF4, 0x25};
    esp_err_t err = s_has_humidity ? write_bytes(hum, sizeof(hum)) : ESP_OK;
    if (err == ESP_OK) {
        err = write_bytes(meas, sizeof(meas));
    }
    if (err != ESP_OK) {
        return err;
    }
    delay_at_least_ms(10);
    for (int i = 0; i < 10; i++) {
        uint8_t status = 0;
        err = read_regs(0xF3, &status, 1);
        if (err != ESP_OK || !(status & 0x08)) {
            break;
        }
        delay_at_least_ms(2);
    }
    uint8_t d[8] = {0};
    if (err == ESP_OK) {
        err = read_regs(0xF7, d, s_has_humidity ? 8 : 6);
    }
    if (err != ESP_OK) {
        return err;
    }
    int32_t adc_p = (d[0] << 12) | (d[1] << 4) | (d[2] >> 4);
    int32_t adc_t = (d[3] << 12) | (d[4] << 4) | (d[5] >> 4);
    int32_t adc_h = (d[6] << 8) | d[7];
    int32_t t100;
    uint32_t pq, hq;
    gb_bme280_compensate(&s_calib, adc_t, adc_p, adc_h, &t100, &pq, &hq);
    out->temp_c = t100 / 100.0f;
    out->temp_valid = true;
    out->pressure_hpa = pq / 25600.0f;
    out->pressure_valid = pq != 0;
    out->rh = hq / 1024.0f;
    out->rh_valid = s_has_humidity;
    return ESP_OK;
}

#endif

static esp_err_t sensor_read(floor_readings_t *out)
{
    if (!s_ready) {
        esp_err_t err = sensor_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    esp_err_t err = measure(out);
    if (err != ESP_OK) {
        s_ready = false; /* set up again next time */
    }
    return err;
}

#endif /* I2C sensors */

/* ---------------------------------------------------------------------------------------------------------- */

esp_err_t room_sensor_init(void)
{
    esp_err_t err = sensor_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "init failed (%s), retrying on every read", esp_err_to_name(err));
    }
    return err;
}

uint8_t room_sensor_caps(void)
{
#if CONFIG_GROWBOX_ROOM_BME280
    return SENSOR_CAP_HUMIDITY | SENSOR_CAP_PRESSURE;
#else
    return SENSOR_CAP_HUMIDITY;
#endif
}

void room_sensor_read(floor_readings_t *out)
{
    *out = (floor_readings_t){0};
    esp_err_t err = sensor_read(out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "temp %s %.1fC | hum %s %.1f%% | pressure %s %.1fhPa", out->temp_valid ? "ok" : "ERR", out->temp_c,
             out->rh_valid ? "ok" : "ERR", out->rh, out->pressure_valid ? "ok" : "--", out->pressure_hpa);
}

#endif /* !CONFIG_GROWBOX_ROOM_NONE */
