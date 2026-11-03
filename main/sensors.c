#include "sensors.h"

#include "aht10.h"
#include "analog.h"
#include "bmp180.h"
#include "delay.h"
#include "dht11.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "growbox_logic.h"
#include "sdkconfig.h"

static const char *TAG = "sensors";

/* LP I2C on the ESP32-C6 is hard-wired to these pins. */
#define LP_I2C_SDA_GPIO 6
#define LP_I2C_SCL_GPIO 7

static i2c_master_bus_handle_t s_bus_f2; /* LP I2C: AHT10 #1 */
static i2c_master_bus_handle_t s_bus_f3; /* HP I2C: AHT10 #2, BMP180 */
static aht10_t s_aht_f2;
static aht10_t s_aht_f3;

static const uint8_t s_caps[GB_FLOOR_COUNT] = {
    SENSOR_CAP_HUMIDITY | SENSOR_CAP_SOIL,
    SENSOR_CAP_HUMIDITY | SENSOR_CAP_SOIL,
    SENSOR_CAP_HUMIDITY | SENSOR_CAP_PRESSURE | SENSOR_CAP_LUX,
};

uint8_t sensors_floor_caps(uint8_t floor)
{
    return floor < GB_FLOOR_COUNT ? s_caps[floor] : 0;
}

bool sensors_floor_fault(uint8_t floor, const floor_readings_t *r)
{
    uint8_t caps = sensors_floor_caps(floor);
    return !r->temp_valid || ((caps & SENSOR_CAP_HUMIDITY) && !r->rh_valid) ||
           ((caps & SENSOR_CAP_SOIL) && !r->soil_valid) || ((caps & SENSOR_CAP_PRESSURE) && !r->pressure_valid) ||
           ((caps & SENSOR_CAP_LUX) && !r->lux_valid);
}

i2c_master_bus_handle_t sensors_i2c_bus(sensors_bus_t bus)
{
    return bus == SENSORS_BUS_LP ? s_bus_f2 : s_bus_f3;
}

static esp_err_t new_bus(i2c_port_num_t port, int sda, int scl, bool lp, i2c_master_bus_handle_t *out)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = port,
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
    esp_err_t err = i2c_new_master_bus(&cfg, out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus SDA=%d SCL=%d init failed: %s", sda, scl, esp_err_to_name(err));
        *out = NULL;
    }
    return err;
}

esp_err_t sensors_init(void)
{
    esp_err_t result = ESP_OK;
    esp_err_t err;

    const gpio_config_t pwr = {
        .pin_bit_mask = 1ULL << CONFIG_GROWBOX_F2_SOIL_RES_PWR_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if ((err = gpio_config(&pwr)) == ESP_OK) {
        gpio_set_level(CONFIG_GROWBOX_F2_SOIL_RES_PWR_GPIO, 0);
    } else {
        ESP_LOGW(TAG, "soil power GPIO setup failed: %s", esp_err_to_name(err));
        result = err;
    }

    if ((err = new_bus(LP_I2C_NUM_0, LP_I2C_SDA_GPIO, LP_I2C_SCL_GPIO, true, &s_bus_f2)) != ESP_OK) {
        result = err;
    }
    if ((err = new_bus(I2C_NUM_0, CONFIG_GROWBOX_F3_I2C_SDA_GPIO, CONFIG_GROWBOX_F3_I2C_SCL_GPIO, false, &s_bus_f3)) !=
        ESP_OK) {
        result = err;
    }
    if ((err = aht10_init(&s_aht_f2, s_bus_f2)) != ESP_OK) {
        result = err;
    }
    if ((err = aht10_init(&s_aht_f3, s_bus_f3)) != ESP_OK) {
        result = err;
    }
    if ((err = bmp180_init(s_bus_f3)) != ESP_OK) {
        result = err;
    }
    if ((err = analog_init()) != ESP_OK) {
        result = err;
    }
    return result;
}

static void read_soil(analog_ch_t ch, int dry_mv, int wet_mv, floor_readings_t *out)
{
    out->soil_valid = analog_read_mv(ch, &out->soil_mv) == ESP_OK;
    if (out->soil_valid) {
        out->soil_pct_x100 = gb_soil_pct_x100(out->soil_mv, dry_mv, wet_mv);
    }
}

static void read_floor1(floor_readings_t *out)
{
    out->temp_valid = dht11_read(CONFIG_GROWBOX_F1_DHT11_GPIO, &out->temp_c, &out->rh) == ESP_OK;
    if (!out->temp_valid) {
        delay_at_least_ms(2000); /* DHT11 needs >= 1 s between reads */
        out->temp_valid = dht11_read(CONFIG_GROWBOX_F1_DHT11_GPIO, &out->temp_c, &out->rh) == ESP_OK;
    }
    out->rh_valid = out->temp_valid;
    read_soil(ANALOG_SOIL_CAP, CONFIG_GROWBOX_SOIL_CAP_DRY_MV, CONFIG_GROWBOX_SOIL_CAP_WET_MV, out);
}

static void read_floor2(floor_readings_t *out)
{
    out->temp_valid = aht10_read(&s_aht_f2, &out->temp_c, &out->rh) == ESP_OK;
    out->rh_valid = out->temp_valid;
    gpio_set_level(CONFIG_GROWBOX_F2_SOIL_RES_PWR_GPIO, 1);
    delay_at_least_ms(100);
    read_soil(ANALOG_SOIL_RES, CONFIG_GROWBOX_SOIL_RES_DRY_MV, CONFIG_GROWBOX_SOIL_RES_WET_MV, out);
    gpio_set_level(CONFIG_GROWBOX_F2_SOIL_RES_PWR_GPIO, 0);
}

static void read_floor3(floor_readings_t *out)
{
    out->temp_valid = aht10_read(&s_aht_f3, &out->temp_c, &out->rh) == ESP_OK;
    out->rh_valid = out->temp_valid;
    float bmp_temp_c;
    out->pressure_valid = bmp180_read(&bmp_temp_c, &out->pressure_hpa) == ESP_OK;
    if (!out->temp_valid && out->pressure_valid) {
        out->temp_c = bmp_temp_c; /* BMP180 backs up the floor temperature, humidity stays unknown */
        out->temp_valid = true;
    }
    int light_mv;
    out->lux_valid = analog_read_mv(ANALOG_LIGHT, &light_mv) == ESP_OK;
    if (out->lux_valid) {
        out->lux = gb_temt6000_lux(light_mv);
    }
}

void sensors_read_floor(uint8_t floor, floor_readings_t *out)
{
    *out = (floor_readings_t){0};
    switch (floor) {
    case 0:
        read_floor1(out);
        break;
    case 1:
        read_floor2(out);
        break;
    case 2:
        read_floor3(out);
        break;
    default:
        return;
    }

    char extra[96] = "";
    uint8_t caps = s_caps[floor];
    if (caps & SENSOR_CAP_SOIL) {
        snprintf(extra, sizeof(extra), " | soil %s %dmV (%.1f%%)", out->soil_valid ? "ok" : "ERR", out->soil_mv,
                 out->soil_pct_x100 / 100.0f);
    } else {
        snprintf(extra, sizeof(extra), " | pressure %s %.1fhPa | light %s %.0flx", out->pressure_valid ? "ok" : "ERR",
                 out->pressure_hpa, out->lux_valid ? "ok" : "ERR", out->lux);
    }
    ESP_LOGI(TAG, "floor %u: temp %s %.1fC | hum %s %.1f%%%s", floor + 1, out->temp_valid ? "ok" : "ERR", out->temp_c,
             out->rh_valid ? "ok" : "ERR", out->rh, extra);
}
