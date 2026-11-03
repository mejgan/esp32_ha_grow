#include "room_sensor.h"

#include "esp_log.h"
#include "sdkconfig.h"
#include "sensor_dev.h"

static const char *TAG = "room";

static sensor_dev_t s_dev = SENSOR_DEV(CONFIG_GROWBOX_ROOM_TYPE, CONFIG_GROWBOX_ROOM_BUS, CONFIG_GROWBOX_ROOM_I2C_ADDR,
                                       CONFIG_GROWBOX_ROOM_GPIO);

esp_err_t room_sensor_init(void)
{
    if (s_dev.kind == SENSOR_DEV_NONE) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "room sensor: %s", sensor_dev_name(s_dev.kind));
    return sensor_dev_init(&s_dev);
}

uint8_t room_sensor_caps(void)
{
    return sensor_dev_caps(s_dev.kind);
}

bool room_sensor_uses_i2c(sensors_bus_t bus, uint8_t addr)
{
    return sensor_dev_is_i2c(s_dev.kind) && s_dev.bus == bus && s_dev.addr == addr;
}

void room_sensor_read(floor_readings_t *out)
{
    *out = (floor_readings_t){0};
    if (s_dev.kind == SENSOR_DEV_NONE) {
        return;
    }
    sensor_dev_read(&s_dev, out);
    ESP_LOGI(TAG, "temp %s %.1fC | hum %s %.1f%% | pressure %s %.1fhPa", out->temp_valid ? "ok" : "ERR", out->temp_c,
             out->rh_valid ? "ok" : "ERR", out->rh, out->pressure_valid ? "ok" : "--", out->pressure_hpa);
}
