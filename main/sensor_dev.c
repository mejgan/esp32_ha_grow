#include "sensor_dev.h"

#include "delay.h"
#include "esp_log.h"

static const char *TAG = "sensor";

#define I2C_TIMEOUT_MS 100
#define I2C_HZ         100000

uint8_t sensor_dev_caps(sensor_dev_kind_t kind)
{
    switch (kind) {
    case SENSOR_DEV_DHT11:
    case SENSOR_DEV_DHT22:
    case SENSOR_DEV_AHT:
    case SENSOR_DEV_SHT3X:
    case SENSOR_DEV_SHT4X:
    case SENSOR_DEV_HTU21D:
        return SENSOR_CAP_TEMP | SENSOR_CAP_HUMIDITY;
    case SENSOR_DEV_BME280:
        return SENSOR_CAP_TEMP | SENSOR_CAP_HUMIDITY | SENSOR_CAP_PRESSURE;
    case SENSOR_DEV_BMP180:
    case SENSOR_DEV_BMP280:
        return SENSOR_CAP_TEMP | SENSOR_CAP_PRESSURE;
    case SENSOR_DEV_BH1750:
        return SENSOR_CAP_LUX;
    default:
        return 0;
    }
}

const char *sensor_dev_name(sensor_dev_kind_t kind)
{
    switch (kind) {
    case SENSOR_DEV_DHT11:
        return "DHT11";
    case SENSOR_DEV_DHT22:
        return "DHT22";
    case SENSOR_DEV_AHT:
        return "AHT";
    case SENSOR_DEV_SHT3X:
        return "SHT3x";
    case SENSOR_DEV_SHT4X:
        return "SHT4x";
    case SENSOR_DEV_HTU21D:
        return "HTU21D";
    case SENSOR_DEV_BME280:
        return "BME280";
    case SENSOR_DEV_BMP180:
        return "BMP180";
    case SENSOR_DEV_BMP280:
        return "BMP280";
    case SENSOR_DEV_BH1750:
        return "BH1750";
    default:
        return "none";
    }
}

bool sensor_dev_is_i2c(sensor_dev_kind_t kind)
{
    return kind >= SENSOR_DEV_AHT && kind <= SENSOR_DEV_BH1750;
}

/* ---------- I2C helpers ---------- */

static esp_err_t tx(sensor_dev_t *d, const uint8_t *b, size_t n)
{
    return i2c_master_transmit(d->dev, b, n, I2C_TIMEOUT_MS);
}

static esp_err_t rx(sensor_dev_t *d, uint8_t *b, size_t n)
{
    return i2c_master_receive(d->dev, b, n, I2C_TIMEOUT_MS);
}

static esp_err_t read_regs(sensor_dev_t *d, uint8_t reg, uint8_t *b, size_t n)
{
    return i2c_master_transmit_receive(d->dev, &reg, 1, b, n, I2C_TIMEOUT_MS);
}

static esp_err_t write_reg(sensor_dev_t *d, uint8_t reg, uint8_t value)
{
    const uint8_t b[] = {reg, value};
    return tx(d, b, sizeof(b));
}

/* ---------- AHT10 / AHT20 ---------- */

static esp_err_t aht_setup(sensor_dev_t *d)
{
    const uint8_t reset[] = {0xBA};
    esp_err_t err = tx(d, reset, sizeof(reset));
    if (err != ESP_OK) {
        return err;
    }
    delay_at_least_ms(20);
    /* AHT10 calibrate command; an AHT20 that rejects it, or isn't calibrated afterwards, gets its own. */
    const uint8_t init_aht10[] = {0xE1, 0x08, 0x00};
    const uint8_t init_aht20[] = {0xBE, 0x08, 0x00};
    err = tx(d, init_aht10, sizeof(init_aht10));
    delay_at_least_ms(10);
    uint8_t status = 0;
    if (err != ESP_OK || (rx(d, &status, 1) == ESP_OK && !(status & 0x08))) {
        err = tx(d, init_aht20, sizeof(init_aht20));
        delay_at_least_ms(10);
    }
    return err;
}

static esp_err_t aht_measure(sensor_dev_t *d, floor_readings_t *out)
{
    const uint8_t trigger[] = {0xAC, 0x33, 0x00};
    esp_err_t err = tx(d, trigger, sizeof(trigger));
    if (err != ESP_OK) {
        return err;
    }
    delay_at_least_ms(80); /* datasheet: > 75 ms */
    /* Still busy (status bit 7) right after the nominal time happens; give it a few more tries. */
    for (int i = 0; i < 5; i++) {
        uint8_t raw[6];
        err = rx(d, raw, sizeof(raw));
        if (err != ESP_OK) {
            return err;
        }
        if (gb_aht10_convert(raw, &out->temp_c, &out->rh)) {
            out->temp_valid = out->rh_valid = true;
            return ESP_OK;
        }
        delay_at_least_ms(10);
    }
    return ESP_ERR_TIMEOUT;
}

/* ---------- SHT3x / SHT4x ---------- */

static esp_err_t sht_setup(sensor_dev_t *d)
{
    static const uint8_t reset3[] = {0x30, 0xA2};
    static const uint8_t reset4[] = {0x94};
    esp_err_t err = d->kind == SENSOR_DEV_SHT3X ? tx(d, reset3, sizeof(reset3)) : tx(d, reset4, sizeof(reset4));
    delay_at_least_ms(2);
    return err;
}

static esp_err_t sht_measure(sensor_dev_t *d, floor_readings_t *out)
{
    static const uint8_t cmd3[] = {0x24, 0x00}; /* single shot, high repeatability, no clock stretching */
    static const uint8_t cmd4[] = {0xFD};       /* high precision */
    bool sht3 = d->kind == SENSOR_DEV_SHT3X;
    esp_err_t err = sht3 ? tx(d, cmd3, sizeof(cmd3)) : tx(d, cmd4, sizeof(cmd4));
    if (err != ESP_OK) {
        return err;
    }
    delay_at_least_ms(sht3 ? 16 : 10);
    uint8_t raw[6];
    err = rx(d, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }
    bool ok = sht3 ? gb_sht3x_convert(raw, &out->temp_c, &out->rh) : gb_sht4x_convert(raw, &out->temp_c, &out->rh);
    out->temp_valid = out->rh_valid = ok;
    return ok ? ESP_OK : ESP_ERR_INVALID_CRC;
}

/* ---------- HTU21D / SHT21 / Si7021 ---------- */

static esp_err_t htu_setup(sensor_dev_t *d)
{
    const uint8_t reset[] = {0xFE};
    esp_err_t err = tx(d, reset, sizeof(reset));
    delay_at_least_ms(15);
    return err;
}

static esp_err_t htu_read_word(sensor_dev_t *d, uint8_t cmd, uint32_t wait_ms, uint8_t raw[3])
{
    esp_err_t err = tx(d, &cmd, 1); /* "no hold master" measurement */
    if (err != ESP_OK) {
        return err;
    }
    delay_at_least_ms(wait_ms);
    return rx(d, raw, 3);
}

static esp_err_t htu_measure(sensor_dev_t *d, floor_readings_t *out)
{
    uint8_t raw[3];
    esp_err_t err = htu_read_word(d, 0xF3, 50, raw);
    if (err == ESP_OK) {
        out->temp_valid = gb_htu21_temp(raw, &out->temp_c);
        err = htu_read_word(d, 0xF5, 16, raw);
    }
    if (err == ESP_OK) {
        out->rh_valid = gb_htu21_rh(raw, &out->rh);
    }
    if (err == ESP_OK && !(out->temp_valid && out->rh_valid)) {
        err = ESP_ERR_INVALID_CRC;
    }
    return err;
}

/* ---------- BME280 / BMP280 ---------- */

static esp_err_t bme_setup(sensor_dev_t *d)
{
    uint8_t id = 0;
    esp_err_t err = read_regs(d, 0xD0, &id, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (id != 0x60 && id != 0x58) {
        ESP_LOGW(TAG, "0x%02x: unexpected chip id 0x%02x (BME280 0x60, BMP280 0x58)", d->addr, id);
        return ESP_ERR_NOT_FOUND;
    }
    bool chip_has_humidity = id == 0x60;
    if (d->kind == SENSOR_DEV_BME280 && !chip_has_humidity) {
        ESP_LOGW(TAG, "0x%02x is a BMP280 (no humidity): choose it as the pressure sensor instead", d->addr);
    }
    d->has_humidity = chip_has_humidity && d->kind == SENSOR_DEV_BME280;
    const uint8_t reset[] = {0xE0, 0xB6};
    err = tx(d, reset, sizeof(reset));
    delay_at_least_ms(5);
    uint8_t tp[26], h[7];
    if (err == ESP_OK) {
        err = read_regs(d, 0x88, tp, sizeof(tp));
    }
    if (err == ESP_OK && d->has_humidity) {
        err = read_regs(d, 0xE1, h, sizeof(h));
    }
    if (err == ESP_OK) {
        gb_bme280_parse_calib(tp, d->has_humidity ? h : NULL, &d->calib.bme);
    }
    return err;
}

static esp_err_t bme_measure(sensor_dev_t *d, floor_readings_t *out)
{
    /* Forced mode, x1 oversampling: one measurement, then the chip sleeps (no self-heating). */
    esp_err_t err = d->has_humidity ? write_reg(d, 0xF2, 0x01) : ESP_OK;
    if (err == ESP_OK) {
        err = write_reg(d, 0xF4, 0x25);
    }
    if (err != ESP_OK) {
        return err;
    }
    delay_at_least_ms(10);
    for (int i = 0; i < 10; i++) {
        uint8_t status = 0;
        err = read_regs(d, 0xF3, &status, 1);
        if (err != ESP_OK || !(status & 0x08)) {
            break;
        }
        delay_at_least_ms(2);
    }
    uint8_t r[8] = {0};
    if (err == ESP_OK) {
        err = read_regs(d, 0xF7, r, d->has_humidity ? 8 : 6);
    }
    if (err != ESP_OK) {
        return err;
    }
    int32_t adc_p = (r[0] << 12) | (r[1] << 4) | (r[2] >> 4);
    int32_t adc_t = (r[3] << 12) | (r[4] << 4) | (r[5] >> 4);
    int32_t adc_h = (r[6] << 8) | r[7];
    int32_t t100;
    uint32_t pq, hq;
    gb_bme280_compensate(&d->calib.bme, adc_t, adc_p, adc_h, &t100, &pq, &hq);
    out->temp_c = t100 / 100.0f;
    out->temp_valid = true;
    out->pressure_hpa = pq / 25600.0f;
    out->pressure_valid = pq != 0;
    if (d->has_humidity) {
        out->rh = hq / 1024.0f;
        out->rh_valid = true;
    }
    return ESP_OK;
}

/* ---------- BMP180 ---------- */

#define BMP180_OSS       3
#define BMP180_REG_CTRL  0xF4
#define BMP180_REG_OUT   0xF6
#define BMP180_CTRL_SCO  0x20 /* start of conversion; reads 1 until the conversion has finished */

static esp_err_t bmp180_setup(sensor_dev_t *d)
{
    uint8_t id = 0;
    esp_err_t err = read_regs(d, 0xD0, &id, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (id != 0x55) {
        ESP_LOGW(TAG, "0x%02x: unexpected chip id 0x%02x (BMP180 0x55)", d->addr, id);
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t c[22];
    err = read_regs(d, 0xAA, c, sizeof(c));
    if (err != ESP_OK) {
        return err;
    }
#define BE16(i) ((uint16_t)((c[(i)] << 8) | c[(i) + 1]))
    gb_bmp180_calib_t *k = &d->calib.bmp180;
    k->ac1 = (int16_t)BE16(0);
    k->ac2 = (int16_t)BE16(2);
    k->ac3 = (int16_t)BE16(4);
    k->ac4 = BE16(6);
    k->ac5 = BE16(8);
    k->ac6 = BE16(10);
    k->b1 = (int16_t)BE16(12);
    k->b2 = (int16_t)BE16(14);
    k->mb = (int16_t)BE16(16);
    k->mc = (int16_t)BE16(18);
    k->md = (int16_t)BE16(20);
#undef BE16
    return ESP_OK;
}

/* Waits the datasheet maximum, then polls Sco until the conversion result is in the output registers. */
static esp_err_t bmp180_wait(sensor_dev_t *d, uint32_t max_ms)
{
    delay_at_least_ms(max_ms);
    for (int i = 0; i < 10; i++) {
        uint8_t ctrl;
        esp_err_t err = read_regs(d, BMP180_REG_CTRL, &ctrl, 1);
        if (err != ESP_OK) {
            return err;
        }
        if (!(ctrl & BMP180_CTRL_SCO)) {
            return ESP_OK;
        }
        delay_at_least_ms(2);
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t bmp180_measure(sensor_dev_t *d, floor_readings_t *out)
{
    uint8_t buf[3];
    int32_t ut = 0;
    esp_err_t err = write_reg(d, BMP180_REG_CTRL, 0x2E);
    if (err == ESP_OK) {
        err = bmp180_wait(d, 5); /* datasheet 4.5 ms */
    }
    if (err == ESP_OK) {
        err = read_regs(d, BMP180_REG_OUT, buf, 2);
    }
    if (err == ESP_OK) {
        ut = (buf[0] << 8) | buf[1];
        err = write_reg(d, BMP180_REG_CTRL, 0x34 + (BMP180_OSS << 6));
    }
    if (err == ESP_OK) {
        err = bmp180_wait(d, 26); /* datasheet 25.5 ms at OSS = 3 */
    }
    if (err == ESP_OK) {
        err = read_regs(d, BMP180_REG_OUT, buf, 3);
    }
    if (err != ESP_OK) {
        return err;
    }
    int32_t up = (((int32_t)buf[0] << 16) | ((int32_t)buf[1] << 8) | buf[2]) >> (8 - BMP180_OSS);
    int32_t t10, pa;
    gb_bmp180_compensate(&d->calib.bmp180, ut, up, BMP180_OSS, &t10, &pa);
    out->temp_c = t10 / 10.0f;
    out->temp_valid = true;
    out->pressure_hpa = pa / 100.0f;
    out->pressure_valid = true;
    return ESP_OK;
}

/* ---------- BH1750 ---------- */

static esp_err_t bh1750_setup(sensor_dev_t *d)
{
    const uint8_t power_on = 0x01;
    return tx(d, &power_on, 1);
}

static esp_err_t bh1750_measure(sensor_dev_t *d, floor_readings_t *out)
{
    /* Power on, then a one-time high-resolution measurement; the chip powers down by itself afterwards. */
    const uint8_t power_on = 0x01, one_time_h = 0x20;
    esp_err_t err = tx(d, &power_on, 1);
    if (err == ESP_OK) {
        err = tx(d, &one_time_h, 1);
    }
    if (err != ESP_OK) {
        return err;
    }
    delay_at_least_ms(180); /* datasheet maximum */
    uint8_t raw[2];
    err = rx(d, raw, sizeof(raw));
    if (err == ESP_OK) {
        out->lux = gb_bh1750_lux(raw);
        out->lux_valid = true;
    }
    return err;
}

/* ---------- dispatch ---------- */

static esp_err_t chip_setup(sensor_dev_t *d)
{
    switch (d->kind) {
    case SENSOR_DEV_AHT:
        return aht_setup(d);
    case SENSOR_DEV_SHT3X:
    case SENSOR_DEV_SHT4X:
        return sht_setup(d);
    case SENSOR_DEV_HTU21D:
        return htu_setup(d);
    case SENSOR_DEV_BME280:
    case SENSOR_DEV_BMP280:
        return bme_setup(d);
    case SENSOR_DEV_BMP180:
        return bmp180_setup(d);
    case SENSOR_DEV_BH1750:
        return bh1750_setup(d);
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t chip_measure(sensor_dev_t *d, floor_readings_t *out)
{
    switch (d->kind) {
    case SENSOR_DEV_AHT:
        return aht_measure(d, out);
    case SENSOR_DEV_SHT3X:
    case SENSOR_DEV_SHT4X:
        return sht_measure(d, out);
    case SENSOR_DEV_HTU21D:
        return htu_measure(d, out);
    case SENSOR_DEV_BME280:
    case SENSOR_DEV_BMP280:
        return bme_measure(d, out);
    case SENSOR_DEV_BMP180:
        return bmp180_measure(d, out);
    case SENSOR_DEV_BH1750:
        return bh1750_measure(d, out);
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t sensor_dev_init(sensor_dev_t *d)
{
    d->ready = false;
    if (!sensor_dev_is_i2c(d->kind)) {
        return ESP_OK; /* nothing to set up; a DHT takes its RMT channel only while reading */
    }
    if (d->dev == NULL) {
        i2c_master_bus_handle_t bus = sensors_i2c_bus(d->bus);
        if (bus == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = d->addr,
            .scl_speed_hz = I2C_HZ,
        };
        esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &d->dev);
        if (err != ESP_OK) {
            d->dev = NULL;
            return err;
        }
    }
    esp_err_t err = chip_setup(d);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s at 0x%02x (%s bus): setup failed: %s", sensor_dev_name(d->kind), d->addr,
                 d->bus == SENSORS_BUS_LP ? "LP" : "main", esp_err_to_name(err));
        return err;
    }
    d->ready = true;
    return ESP_OK;
}

static esp_err_t dht_measure(sensor_dev_t *d, floor_readings_t *out)
{
    esp_err_t err = dht_read(&d->dht, &out->temp_c, &out->rh);
    if (err != ESP_OK) {
        /* One retry: a DHT needs 1 s (DHT11) / 2 s (DHT22) between reads. */
        delay_at_least_ms(2100);
        err = dht_read(&d->dht, &out->temp_c, &out->rh);
    }
    out->temp_valid = out->rh_valid = err == ESP_OK;
    return err;
}

esp_err_t sensor_dev_read(sensor_dev_t *d, floor_readings_t *out)
{
    if (d->kind == SENSOR_DEV_DHT11 || d->kind == SENSOR_DEV_DHT22) {
        return dht_measure(d, out);
    }
    if (!sensor_dev_is_i2c(d->kind)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!d->ready) {
        esp_err_t err = sensor_dev_init(d);
        if (err != ESP_OK) {
            return err;
        }
    }
    floor_readings_t r = *out;
    esp_err_t err = chip_measure(d, &r);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s at 0x%02x: read failed: %s", sensor_dev_name(d->kind), d->addr, esp_err_to_name(err));
        d->ready = false; /* set up again next time */
        return err;
    }
    *out = r;
    return ESP_OK;
}
