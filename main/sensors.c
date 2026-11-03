#include "sensors.h"

#include <stdarg.h>
#include <stdio.h>

#include "analog.h"
#include "delay.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "growbox_logic.h"
#include "room_sensor.h"
#include "sdkconfig.h"
#include "sensor_dev.h"

static const char *TAG = "sensors";

/* LP I2C on the ESP32-C6 is hard-wired to these pins. */
#define LP_I2C_SDA_GPIO 6
#define LP_I2C_SCL_GPIO 7

/* Numbers match the *_SOIL_TYPE / *_LIGHT_TYPE options in main/Kconfig.projbuild. */
enum { SOIL_NONE = 0, SOIL_CAPACITIVE = 1, SOIL_RESISTIVE = 2 };
enum { LIGHT_NONE = 0, LIGHT_TEMT6000 = 1, LIGHT_BH1750 = 2 };

/* Settling time after switching a GPIO-powered soil sensor on. The capacitive sensor's output is filtered. */
#define SOIL_RES_SETTLE_MS 100
#define SOIL_CAP_SETTLE_MS 1000

typedef struct {
    sensor_dev_t climate;  /* temperature / humidity (BME280: + pressure) */
    sensor_dev_t pressure; /* BMP180 / BMP280: pressure, backup temperature */
    sensor_dev_t light;    /* BH1750 */
    int light_type;
    int light_gpio; /* TEMT6000 */
    int soil_type;
    int soil_gpio;
    int soil_pwr_gpio;
    int soil_dry_mv;
    int soil_wet_mv;
} floor_hw_t;

#define BH1750_IF(light_type) ((light_type) == LIGHT_BH1750 ? SENSOR_DEV_BH1750 : SENSOR_DEV_NONE)

#define FLOOR_HW(n)                                                                                              \
    {                                                                                                            \
        .climate = SENSOR_DEV(CONFIG_GROWBOX_F##n##_CLIMATE_TYPE, CONFIG_GROWBOX_F##n##_BUS,                     \
                              CONFIG_GROWBOX_F##n##_CLIMATE_ADDR, CONFIG_GROWBOX_F##n##_CLIMATE_GPIO),           \
        .pressure = SENSOR_DEV(CONFIG_GROWBOX_F##n##_PRESSURE_TYPE, CONFIG_GROWBOX_F##n##_BUS,                   \
                               CONFIG_GROWBOX_F##n##_PRESSURE_ADDR, -1),                                         \
        .light = SENSOR_DEV(BH1750_IF(CONFIG_GROWBOX_F##n##_LIGHT_TYPE), CONFIG_GROWBOX_F##n##_BUS,              \
                            CONFIG_GROWBOX_F##n##_LIGHT_ADDR, -1),                                               \
        .light_type = CONFIG_GROWBOX_F##n##_LIGHT_TYPE, .light_gpio = CONFIG_GROWBOX_F##n##_LIGHT_GPIO,          \
        .soil_type = CONFIG_GROWBOX_F##n##_SOIL_TYPE, .soil_gpio = CONFIG_GROWBOX_F##n##_SOIL_GPIO,              \
        .soil_pwr_gpio = CONFIG_GROWBOX_F##n##_SOIL_PWR_GPIO, .soil_dry_mv = CONFIG_GROWBOX_F##n##_SOIL_DRY_MV,  \
        .soil_wet_mv = CONFIG_GROWBOX_F##n##_SOIL_WET_MV,                                                        \
    }

static floor_hw_t s_floor[GB_FLOOR_COUNT] = {FLOOR_HW(1), FLOOR_HW(2), FLOOR_HW(3)};
static i2c_master_bus_handle_t s_bus[2];

static uint8_t floor_caps(const floor_hw_t *h)
{
    uint8_t caps = sensor_dev_caps(h->climate.kind) | sensor_dev_caps(h->pressure.kind);
    if (h->light_type != LIGHT_NONE) {
        caps |= SENSOR_CAP_LUX;
    }
    if (h->soil_type != SOIL_NONE) {
        caps |= SENSOR_CAP_SOIL;
    }
    return caps;
}

uint8_t sensors_floor_caps(uint8_t floor)
{
    return floor < GB_FLOOR_COUNT ? floor_caps(&s_floor[floor]) : 0;
}

bool sensors_any_temp(void)
{
    for (int f = 0; f < GB_FLOOR_COUNT; f++) {
        if (sensors_floor_caps(f) & SENSOR_CAP_TEMP) {
            return true;
        }
    }
    return false;
}

bool sensors_floor_fault(uint8_t floor, const floor_readings_t *r)
{
    uint8_t caps = sensors_floor_caps(floor);
    return ((caps & SENSOR_CAP_TEMP) && !r->temp_valid) || ((caps & SENSOR_CAP_HUMIDITY) && !r->rh_valid) ||
           ((caps & SENSOR_CAP_SOIL) && !r->soil_valid) || ((caps & SENSOR_CAP_PRESSURE) && !r->pressure_valid) ||
           ((caps & SENSOR_CAP_LUX) && !r->lux_valid);
}

i2c_master_bus_handle_t sensors_i2c_bus(sensors_bus_t bus)
{
    return bus == SENSORS_BUS_LP ? s_bus[SENSORS_BUS_LP] : s_bus[SENSORS_BUS_MAIN];
}

static bool dev_at(const sensor_dev_t *d, sensors_bus_t bus, uint8_t addr)
{
    return sensor_dev_is_i2c(d->kind) && d->bus == bus && d->addr == addr;
}

bool sensors_i2c_addr_in_use(sensors_bus_t bus, uint8_t addr)
{
    for (int f = 0; f < GB_FLOOR_COUNT; f++) {
        const floor_hw_t *h = &s_floor[f];
        if (dev_at(&h->climate, bus, addr) || dev_at(&h->pressure, bus, addr) || dev_at(&h->light, bus, addr)) {
            return true;
        }
    }
    return room_sensor_uses_i2c(bus, addr);
}

static esp_err_t new_bus(sensors_bus_t which, int sda, int scl)
{
    bool lp = which == SENSORS_BUS_LP;
    i2c_master_bus_config_t cfg = {
        .i2c_port = lp ? LP_I2C_NUM_0 : I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    if (lp) {
        cfg.lp_source_clk = LP_I2C_SCLK_DEFAULT;
    } else {
        cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    }
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus[which]);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s I2C bus SDA=%d SCL=%d init failed: %s", lp ? "LP" : "main", sda, scl,
                 esp_err_to_name(err));
        s_bus[which] = NULL;
    }
    return err;
}

static void keep_first_error(esp_err_t *result, esp_err_t err)
{
    if (*result == ESP_OK && err != ESP_OK) {
        *result = err;
    }
}

esp_err_t sensors_init(void)
{
    esp_err_t result = ESP_OK;
#if CONFIG_GROWBOX_I2C_MAIN_USED
    keep_first_error(&result, new_bus(SENSORS_BUS_MAIN, CONFIG_GROWBOX_I2C_SDA_GPIO, CONFIG_GROWBOX_I2C_SCL_GPIO));
#endif
#if CONFIG_GROWBOX_I2C_LP_USED
    keep_first_error(&result, new_bus(SENSORS_BUS_LP, LP_I2C_SDA_GPIO, LP_I2C_SCL_GPIO));
#endif
    keep_first_error(&result, analog_init());

    for (int f = 0; f < GB_FLOOR_COUNT; f++) {
        floor_hw_t *h = &s_floor[f];
        if (h->soil_type != SOIL_NONE && h->soil_pwr_gpio >= 0) {
            const gpio_config_t pwr = {
                .pin_bit_mask = 1ULL << h->soil_pwr_gpio,
                .mode = GPIO_MODE_OUTPUT,
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE,
            };
            esp_err_t err = gpio_config(&pwr);
            if (err == ESP_OK) {
                gpio_set_level(h->soil_pwr_gpio, 0);
            } else {
                ESP_LOGW(TAG, "floor %d: soil power GPIO%d setup failed: %s", f + 1, h->soil_pwr_gpio,
                         esp_err_to_name(err));
            }
            keep_first_error(&result, err);
        }
        keep_first_error(&result, sensor_dev_init(&h->climate));
        keep_first_error(&result, sensor_dev_init(&h->pressure));
        keep_first_error(&result, sensor_dev_init(&h->light));
        ESP_LOGI(TAG, "floor %d: climate %s, pressure %s, soil %s, light %s", f + 1, sensor_dev_name(h->climate.kind),
                 sensor_dev_name(h->pressure.kind),
                 h->soil_type == SOIL_CAPACITIVE ? "capacitive" : h->soil_type == SOIL_RESISTIVE ? "resistive" : "none",
                 h->light_type == LIGHT_TEMT6000 ? "TEMT6000" : h->light_type == LIGHT_BH1750 ? "BH1750" : "none");
    }
    keep_first_error(&result, room_sensor_init());
    return result;
}

static void read_soil(const floor_hw_t *h, floor_readings_t *out)
{
    if (h->soil_pwr_gpio >= 0) {
        gpio_set_level(h->soil_pwr_gpio, 1);
        delay_at_least_ms(h->soil_type == SOIL_RESISTIVE ? SOIL_RES_SETTLE_MS : SOIL_CAP_SETTLE_MS);
    }
    out->soil_valid = analog_read_mv(h->soil_gpio, &out->soil_mv) == ESP_OK;
    if (h->soil_pwr_gpio >= 0) {
        gpio_set_level(h->soil_pwr_gpio, 0);
    }
    if (out->soil_valid) {
        out->soil_pct_x100 = gb_soil_pct_x100(out->soil_mv, h->soil_dry_mv, h->soil_wet_mv);
    }
}

static void read_light(floor_hw_t *h, floor_readings_t *out)
{
    if (h->light_type == LIGHT_BH1750) {
        sensor_dev_read(&h->light, out);
    } else if (h->light_type == LIGHT_TEMT6000) {
        int mv;
        out->lux_valid = analog_read_mv(h->light_gpio, &mv) == ESP_OK;
        if (out->lux_valid) {
            out->lux = gb_temt6000_lux(mv);
        }
    }
}

/* One log line built from parts: "floor 1: temp ok 24.0C | hum ok 55.0% | soil ok 1980mV (51.4%)". */
typedef struct {
    char text[192];
    size_t len;
    const char *sep;
} log_line_t;

static void log_add(log_line_t *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void log_add(log_line_t *l, const char *fmt, ...)
{
    char part[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(part, sizeof(part), fmt, ap);
    va_end(ap);
    if (l->len < sizeof(l->text)) {
        int n = snprintf(l->text + l->len, sizeof(l->text) - l->len, "%s%s", l->sep, part);
        if (n > 0) {
            l->len += (size_t)n;
        }
    }
    l->sep = " | ";
}

void sensors_read_floor(uint8_t floor, floor_readings_t *out)
{
    *out = (floor_readings_t){0};
    if (floor >= GB_FLOOR_COUNT) {
        return;
    }
    floor_hw_t *h = &s_floor[floor];
    if (h->climate.kind != SENSOR_DEV_NONE) {
        sensor_dev_read(&h->climate, out);
    }
    if (h->pressure.kind != SENSOR_DEV_NONE) {
        floor_readings_t p = {0};
        sensor_dev_read(&h->pressure, &p);
        if (!out->pressure_valid && p.pressure_valid) {
            out->pressure_valid = true;
            out->pressure_hpa = p.pressure_hpa;
        }
        if (!out->temp_valid && p.temp_valid) {
            out->temp_valid = true; /* backup temperature; humidity stays unknown */
            out->temp_c = p.temp_c;
        }
    }
    read_light(h, out);
    if (h->soil_type != SOIL_NONE) {
        read_soil(h, out);
    }

    log_line_t l = {.sep = ": "};
    uint8_t caps = floor_caps(h);
    if (caps & SENSOR_CAP_TEMP) {
        log_add(&l, "temp %s %.1fC", out->temp_valid ? "ok" : "ERR", out->temp_c);
    }
    if (caps & SENSOR_CAP_HUMIDITY) {
        log_add(&l, "hum %s %.1f%%", out->rh_valid ? "ok" : "ERR", out->rh);
    }
    if (caps & SENSOR_CAP_PRESSURE) {
        log_add(&l, "pressure %s %.1fhPa", out->pressure_valid ? "ok" : "ERR", out->pressure_hpa);
    }
    if (caps & SENSOR_CAP_LUX) {
        log_add(&l, "light %s %.0flx", out->lux_valid ? "ok" : "ERR", out->lux);
    }
    if (caps & SENSOR_CAP_SOIL) {
        log_add(&l, "soil %s %dmV (%.1f%%)", out->soil_valid ? "ok" : "ERR", out->soil_mv,
                out->soil_pct_x100 / 100.0f);
    }
    if (caps == 0) {
        log_add(&l, "no sensors");
    }
    ESP_LOGI(TAG, "floor %u%s", floor + 1, l.text);
}
