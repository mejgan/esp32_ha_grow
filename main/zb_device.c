#include "zb_device.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_zigbee.h"
#include "ezbee/zha.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "zb_device";

/* Endpoint N+1 is floor N: dimmable light + that floor's sensors + the grow box cluster. */
#define FLOOR_EP(floor) ((uint8_t)((floor) + 1))
#define EP_MAIN         FLOOR_EP(0) /* Basic strings, Time client */

#define GROWBOX_CLUSTER_ID 0xFC00

enum {
    ATTR_AUTO_MODE = 0x0000,
    ATTR_ON_HOUR = 0x0001,
    ATTR_OFF_HOUR = 0x0002,
    ATTR_AUTO_BRIGHTNESS = 0x0003,
    ATTR_MAX_TEMP = 0x0004,
    ATTR_HEAT_LOCK = 0x0005,
    ATTR_SOIL_MOISTURE = 0x0006, /* only on floors with a soil sensor */
    ATTR_FLOOR_ENABLED = 0x0007,
    ATTR_TIME_SYNCED = 0x0008,
};

/* ZCL character strings: first byte is the length. */
static const char s_manufacturer[] = "\x0A" "GrowBoxDIY";
static const char s_model[] = "\x0A" "GrowBox-C6";

#define ZB_CONFIG()                                                                              \
    {                                                                                            \
        .device_config =                                                                         \
            {                                                                                    \
                .device_type = EZB_NWK_DEVICE_TYPE_ROUTER,                                       \
                .install_code_policy = false,                                                    \
                .zczr_config = {.max_children = 10},                                             \
            },                                                                                   \
        .platform_config =                                                                       \
            {                                                                                    \
                .storage_partition_name = "nvs",                                                 \
                .radio_config = {.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE},                    \
            },                                                                                   \
    }

/* Per-floor attribute values last written by this module; remote writes equal to these are echoes, not commands.
 * Also the initial attribute storage handed to the data model (static, outlives creation). */
typedef struct {
    uint8_t on;
    uint8_t level;
    uint8_t enabled;
    uint8_t auto_mode;
    uint8_t on_hour;
    uint8_t off_hour;
    uint8_t auto_brightness;
    uint8_t max_temp;
    uint8_t heat_lock;
    uint8_t time_synced;
} published_t;

static gb_settings_t s_initial[GB_FLOOR_COUNT];
static published_t s_pub[GB_FLOOR_COUNT];
static published_t s_attr_init[GB_FLOOR_COUNT];
static uint16_t s_attr_soil_init[GB_FLOOR_COUNT];
static bool s_internal_write;
static volatile bool s_zb_ready;
static volatile bool s_joined;

static uint16_t s_time_attrs[] = {EZB_ZCL_ATTR_TIME_LOCAL_TIME_ID};

static portMUX_TYPE s_time_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_time_synced;
static uint32_t s_local_secs;
static int64_t s_sync_us;

/* ---------- one-shot retry timer (from esp-zigbee-sdk examples/utils/example_common/src/alarm_timer.c) ---------- */

typedef void (*alarm_cb_t)(uint8_t arg);

typedef struct {
    esp_timer_handle_t timer;
    alarm_cb_t cb;
    uint8_t arg;
} alarm_t;

static void alarm_timer_cb(void *arg)
{
    alarm_t *alarm = arg;
    esp_timer_delete(alarm->timer);
    alarm->cb(alarm->arg);
    free(alarm);
}

static esp_err_t alarm_timer_schedule(alarm_cb_t cb, uint8_t arg, uint32_t time_ms)
{
    alarm_t *alarm = malloc(sizeof(*alarm));
    if (alarm == NULL) {
        return ESP_ERR_NO_MEM;
    }
    alarm->cb = cb;
    alarm->arg = arg;
    alarm->timer = NULL;
    const esp_timer_create_args_t args = {.callback = alarm_timer_cb, .arg = alarm, .name = "zb_retry"};
    esp_err_t err = esp_timer_create(&args, &alarm->timer);
    if (err != ESP_OK) {
        free(alarm);
        return err;
    }
    err = esp_timer_start_once(alarm->timer, (uint64_t)time_ms * 1000);
    if (err != ESP_OK) {
        esp_timer_delete(alarm->timer);
        free(alarm);
    }
    return err;
}

static void bdb_commissioning_cb(uint8_t mode)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_start_top_level_commissioning(mode);
    esp_zigbee_lock_release();
}

static void schedule_commissioning(uint8_t mode)
{
    esp_err_t err = alarm_timer_schedule(bdb_commissioning_cb, mode, 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to schedule commissioning retry: %s", esp_err_to_name(err));
    }
}

/* ---------- signals ---------- */

static bool signal_handler(const ezb_app_signal_t *sig)
{
    ezb_app_signal_type_t type = ezb_app_signal_get_type(sig);

    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status = *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(sig);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            if (ezb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Factory new, start network steering (enable Permit join in Zigbee2MQTT)");
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Rejoined network: PAN ID 0x%04hx, channel %d, short address 0x%04hx",
                         ezb_nwk_get_panid(), ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
                s_joined = true;
            }
        } else {
            ESP_LOGW(TAG, "%s failed with status 0x%02x, retrying", ezb_app_signal_to_string(type), status);
            schedule_commissioning(EZB_BDB_MODE_INITIALIZATION);
        }
    } break;
    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(sig);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ezb_extpanid_t ext;
            ezb_nwk_get_extended_panid(&ext);
            ESP_LOGI(TAG, "Joined network successfully: PAN ID 0x%04hx (ext 0x%llx), channel %d, short address 0x%04hx",
                     ezb_nwk_get_panid(), ext.u64, ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            s_joined = true;
        } else {
            ESP_LOGI(TAG, "Network steering failed (status 0x%02x), retrying", status);
            schedule_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        }
    } break;
    case EZB_ZDO_SIGNAL_LEAVE:
        ESP_LOGW(TAG, "Left the network, back to pairing mode");
        s_joined = false;
        portENTER_CRITICAL(&s_time_mux);
        s_time_synced = false;
        portEXIT_CRITICAL(&s_time_mux);
        schedule_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        break;
    default:
        ESP_LOGI(TAG, "Zigbee signal %s (0x%02lx)", ezb_app_signal_to_string(type), (unsigned long)type);
        break;
    }
    return true;
}

/* ---------- remote writes ---------- */

static void handle_remote(uint8_t ep, uint16_t cluster, uint16_t attr, const void *val)
{
    if (ep < FLOOR_EP(0) || ep > FLOOR_EP(GB_FLOOR_COUNT - 1) || val == NULL || s_internal_write) {
        return;
    }
    uint8_t floor = ep - FLOOR_EP(0);
    const published_t *pub = &s_pub[floor];
    uint8_t v = *(const uint8_t *)val;
    app_field_t field;
    uint8_t published;

    if (cluster == EZB_ZCL_CLUSTER_ID_ON_OFF && attr == EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
        v = v != 0;
        field = APP_FIELD_LIGHT_ON;
        published = pub->on;
    } else if (cluster == EZB_ZCL_CLUSTER_ID_LEVEL && attr == EZB_ZCL_ATTR_LEVEL_CURRENT_LEVEL_ID) {
        field = APP_FIELD_LIGHT_LEVEL;
        published = pub->level;
    } else if (cluster == GROWBOX_CLUSTER_ID) {
        switch (attr) {
        case ATTR_FLOOR_ENABLED:
            v = v != 0;
            field = APP_FIELD_ENABLED;
            published = pub->enabled;
            break;
        case ATTR_AUTO_MODE:
            v = v != 0;
            field = APP_FIELD_AUTO_MODE;
            published = pub->auto_mode;
            break;
        case ATTR_ON_HOUR:
            field = APP_FIELD_ON_HOUR;
            published = pub->on_hour;
            break;
        case ATTR_OFF_HOUR:
            field = APP_FIELD_OFF_HOUR;
            published = pub->off_hour;
            break;
        case ATTR_AUTO_BRIGHTNESS:
            field = APP_FIELD_AUTO_BRIGHTNESS;
            published = pub->auto_brightness;
            break;
        case ATTR_MAX_TEMP:
            field = APP_FIELD_MAX_TEMP;
            published = pub->max_temp;
            break;
        default:
            return;
        }
    } else {
        return;
    }

    if (v == published) {
        return;
    }
    ESP_LOGI(TAG, "floor %u: remote write cluster 0x%04x attr 0x%04x = %u", floor + 1, cluster, attr, v);
    app_remote_set(floor, field, v);
}

static void custom_write_cb(uint8_t ep_id, uint16_t attr_id, void *new_value, uint16_t manuf_code)
{
    (void)manuf_code;
    handle_remote(ep_id, GROWBOX_CLUSTER_ID, attr_id, new_value);
}

static void handle_time_rsp(const ezb_zcl_cmd_read_attr_rsp_message_t *msg)
{
    for (const ezb_zcl_read_attr_rsp_variable_t *var = msg->in.variables; var != NULL; var = var->next) {
        if (var->attr_id != EZB_ZCL_ATTR_TIME_LOCAL_TIME_ID || var->status != EZB_ZCL_STATUS_SUCCESS ||
            var->attr_value == NULL) {
            continue;
        }
        uint32_t secs;
        memcpy(&secs, var->attr_value, sizeof(secs));
        if (secs == 0 || secs == 0xFFFFFFFFu) {
            ESP_LOGW(TAG, "coordinator LocalTime not set (0x%08lx)", (unsigned long)secs);
            continue;
        }
        portENTER_CRITICAL(&s_time_mux);
        s_local_secs = secs;
        s_sync_us = esp_timer_get_time();
        s_time_synced = true;
        portEXIT_CRITICAL(&s_time_mux);
        ESP_LOGI(TAG, "time synced: local %02lu:%02lu:%02lu", (unsigned long)(secs / 3600 % 24),
                 (unsigned long)(secs / 60 % 60), (unsigned long)(secs % 60));
    }
}

static void action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID: {
        const ezb_zcl_set_attr_value_message_t *msg = message;
        if (msg->info.cluster_role == EZB_ZCL_CLUSTER_SERVER) {
            handle_remote(msg->info.dst_ep, msg->info.cluster_id, msg->in.attribute.id, msg->in.attribute.data.value);
        }
    } break;
    case EZB_ZCL_CORE_READ_ATTR_RSP_CB_ID: {
        const ezb_zcl_cmd_read_attr_rsp_message_t *msg = message;
        if (msg->info.cluster_id == EZB_ZCL_CLUSTER_ID_TIME) {
            handle_time_rsp(msg);
        }
    } break;
    default:
        ESP_LOGD(TAG, "ZCL core action 0x%04lx", (unsigned long)callback_id);
        break;
    }
}

/* ---------- data model ---------- */

static esp_err_t add_cluster(ezb_af_ep_desc_t ep, ezb_zcl_cluster_desc_t cluster)
{
    return esp_zigbee_err_to_esp(ezb_af_endpoint_add_cluster_desc(ep, cluster));
}

static esp_err_t add_custom_attr(ezb_zcl_cluster_desc_t desc, uint16_t id, uint8_t type, bool writable,
                                 const void *value)
{
    uint8_t access = writable ? (EZB_ZCL_ATTR_ACCESS_READ_WRITE | EZB_ZCL_ATTR_ACCESS_REPORTING)
                              : (EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING);
    return esp_zigbee_err_to_esp(ezb_zcl_custom_cluster_desc_add_attr(desc, id, type, access, value));
}

static ezb_af_ep_desc_t create_floor_endpoint(uint8_t floor)
{
    /* Config storage is static and per floor so it stays valid whether or not the SDK copies it. */
    static ezb_zha_dimmable_light_config_t light_cfg[GB_FLOOR_COUNT];
    static ezb_zcl_temperature_measurement_cluster_server_config_t temp_cfg[GB_FLOOR_COUNT];
    static ezb_zcl_rel_humidity_measurement_cluster_server_config_t hum_cfg[GB_FLOOR_COUNT];
    static ezb_zcl_pressure_measurement_cluster_server_config_t press_cfg[GB_FLOOR_COUNT];
    static ezb_zcl_illuminance_measurement_cluster_server_config_t illum_cfg[GB_FLOOR_COUNT];
    static ezb_zcl_custom_cluster_config_t custom_cfg[GB_FLOOR_COUNT];

    const gb_settings_t *init = &s_initial[floor];
    uint8_t caps = sensors_floor_caps(floor);
    uint8_t ep_id = FLOOR_EP(floor);

    light_cfg[floor] = (ezb_zha_dimmable_light_config_t)EZB_ZHA_DIMMABLE_LIGHT_CONFIG();
    light_cfg[floor].on_off_cfg.on_off = false;
    light_cfg[floor].level_cfg.current_level = init->manual_level;
    ezb_af_ep_desc_t ep = ezb_zha_create_dimmable_light(ep_id, &light_cfg[floor]);

    if (ep_id == EP_MAIN) {
        ezb_zcl_cluster_desc_t basic =
            ezb_af_endpoint_get_cluster_desc(ep, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
        ESP_ERROR_CHECK(esp_zigbee_err_to_esp(
            ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, s_manufacturer)));
        ESP_ERROR_CHECK(esp_zigbee_err_to_esp(
            ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, s_model)));
        /* Time client: lets us read LocalTime from the coordinator. */
        ESP_ERROR_CHECK(add_cluster(ep, ezb_zcl_time_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)));
    }

    temp_cfg[floor] = (ezb_zcl_temperature_measurement_cluster_server_config_t){0, -4000, 8500};
    ESP_ERROR_CHECK(
        add_cluster(ep, ezb_zcl_temperature_measurement_create_cluster_desc(&temp_cfg[floor], EZB_ZCL_CLUSTER_SERVER)));
    if (caps & SENSOR_CAP_HUMIDITY) {
        hum_cfg[floor] = (ezb_zcl_rel_humidity_measurement_cluster_server_config_t){0, 0, 10000};
        ESP_ERROR_CHECK(add_cluster(
            ep, ezb_zcl_rel_humidity_measurement_create_cluster_desc(&hum_cfg[floor], EZB_ZCL_CLUSTER_SERVER)));
    }
    if (caps & SENSOR_CAP_PRESSURE) {
        press_cfg[floor] = (ezb_zcl_pressure_measurement_cluster_server_config_t){0, 300, 1100};
        ESP_ERROR_CHECK(add_cluster(
            ep, ezb_zcl_pressure_measurement_create_cluster_desc(&press_cfg[floor], EZB_ZCL_CLUSTER_SERVER)));
    }
    if (caps & SENSOR_CAP_LUX) {
        illum_cfg[floor] = (ezb_zcl_illuminance_measurement_cluster_server_config_t){0, 1, 40001};
        ESP_ERROR_CHECK(add_cluster(
            ep, ezb_zcl_illuminance_measurement_create_cluster_desc(&illum_cfg[floor], EZB_ZCL_CLUSTER_SERVER)));
    }

    published_t *a = &s_attr_init[floor];
    *a = (published_t){
        .on = light_cfg[floor].on_off_cfg.on_off,
        .level = light_cfg[floor].level_cfg.current_level,
        .enabled = init->enabled,
        .auto_mode = init->auto_mode,
        .on_hour = init->on_hour,
        .off_hour = init->off_hour,
        .auto_brightness = init->auto_brightness_pct,
        .max_temp = init->max_temp_c,
    };
    custom_cfg[floor] = (ezb_zcl_custom_cluster_config_t){.cluster_id = GROWBOX_CLUSTER_ID};
    ezb_zcl_cluster_desc_t custom = ezb_zcl_custom_create_cluster_desc(&custom_cfg[floor], EZB_ZCL_CLUSTER_SERVER);
    ESP_ERROR_CHECK(add_custom_attr(custom, ATTR_AUTO_MODE, EZB_ZCL_ATTR_TYPE_BOOL, true, &a->auto_mode));
    ESP_ERROR_CHECK(add_custom_attr(custom, ATTR_ON_HOUR, EZB_ZCL_ATTR_TYPE_UINT8, true, &a->on_hour));
    ESP_ERROR_CHECK(add_custom_attr(custom, ATTR_OFF_HOUR, EZB_ZCL_ATTR_TYPE_UINT8, true, &a->off_hour));
    ESP_ERROR_CHECK(add_custom_attr(custom, ATTR_AUTO_BRIGHTNESS, EZB_ZCL_ATTR_TYPE_UINT8, true, &a->auto_brightness));
    ESP_ERROR_CHECK(add_custom_attr(custom, ATTR_MAX_TEMP, EZB_ZCL_ATTR_TYPE_UINT8, true, &a->max_temp));
    ESP_ERROR_CHECK(add_custom_attr(custom, ATTR_HEAT_LOCK, EZB_ZCL_ATTR_TYPE_BOOL, false, &a->heat_lock));
    if (caps & SENSOR_CAP_SOIL) {
        ESP_ERROR_CHECK(
            add_custom_attr(custom, ATTR_SOIL_MOISTURE, EZB_ZCL_ATTR_TYPE_UINT16, false, &s_attr_soil_init[floor]));
    }
    ESP_ERROR_CHECK(add_custom_attr(custom, ATTR_FLOOR_ENABLED, EZB_ZCL_ATTR_TYPE_BOOL, true, &a->enabled));
    ESP_ERROR_CHECK(add_custom_attr(custom, ATTR_TIME_SYNCED, EZB_ZCL_ATTR_TYPE_BOOL, false, &a->time_synced));
    ESP_ERROR_CHECK(add_cluster(ep, custom));

    s_pub[floor] = *a;
    return ep;
}

static void create_data_model(void)
{
    ezb_af_device_desc_t dev = ezb_af_create_device_desc();
    for (uint8_t f = 0; f < GB_FLOOR_COUNT; f++) {
        ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_af_device_add_endpoint_desc(dev, create_floor_endpoint(f))));
    }
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_af_device_desc_register(dev)));

    ezb_zcl_core_action_handler_register(action_handler);
    static const ezb_zcl_custom_cluster_handlers_t custom_handlers = {
        .cluster_id = GROWBOX_CLUSTER_ID,
        .cluster_role = EZB_ZCL_CLUSTER_SERVER,
        .write_attr_cb = custom_write_cb,
    };
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_zcl_custom_cluster_handlers_register(&custom_handlers)));
}

/* ---------- stack task ---------- */

static void zb_main_task(void *arg)
{
    (void)arg;
    esp_zigbee_config_t config = ZB_CONFIG();
    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_bdb_set_primary_channel_set(CONFIG_GROWBOX_ZB_CHANNEL_MASK)));
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_bdb_set_secondary_channel_set(CONFIG_GROWBOX_ZB_CHANNEL_MASK)));
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_app_signal_add_handler(signal_handler)));

    create_data_model();

    ESP_ERROR_CHECK(esp_zigbee_start(false));
    s_zb_ready = true;
    esp_zigbee_launch_mainloop();

    s_zb_ready = false;
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

esp_err_t zb_device_start(const gb_settings_t initial[GB_FLOOR_COUNT])
{
    memcpy(s_initial, initial, sizeof(s_initial));
    if (xTaskCreate(zb_main_task, "zb_main", 8192, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---------- public API used by the app task ---------- */

bool zb_device_is_joined(void)
{
    return s_joined;
}

void zb_device_request_time(void)
{
    if (!s_zb_ready) {
        return;
    }
    ezb_zcl_read_attr_cmd_t cmd = {
        .cmd_ctrl =
            {
                .dst_ep = 1,
                .src_ep = EP_MAIN,
                .cluster_id = EZB_ZCL_CLUSTER_ID_TIME,
                .manuf_code = EZB_ZCL_STD_MANUF_CODE,
                .fc.direction = EZB_ZCL_CMD_DIRECTION_TO_SRV,
            },
        .payload =
            {
                .attr_number = 1,
                .attr_field = s_time_attrs,
            },
    };
    ezb_address_set_short(&cmd.cmd_ctrl.dst_addr, 0x0000);

    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_err_t err = ezb_zcl_read_attr_cmd_req(&cmd);
    esp_zigbee_lock_release();
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "time read request failed (0x%04x)", (unsigned)err);
    } else {
        ESP_LOGI(TAG, "requested LocalTime from coordinator");
    }
}

bool zb_device_get_local_hour(uint8_t *hour)
{
    portENTER_CRITICAL(&s_time_mux);
    bool synced = s_time_synced;
    uint32_t secs = s_local_secs;
    int64_t sync_us = s_sync_us;
    portEXIT_CRITICAL(&s_time_mux);
    if (!synced) {
        return false;
    }
    *hour = gb_hour_from_zcl_local(secs + (uint32_t)((esp_timer_get_time() - sync_us) / 1000000));
    return true;
}

/* Caller holds the Zigbee lock. */
static void set_attr(uint8_t ep, uint16_t cluster, uint16_t attr, void *value)
{
    s_internal_write = true;
    ezb_zcl_status_t st = ezb_zcl_set_attr_value(ep, cluster, EZB_ZCL_CLUSTER_SERVER, attr, EZB_ZCL_STD_MANUF_CODE, value,
                                                 false);
    s_internal_write = false;
    if (st != EZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "set attr ep %u cluster 0x%04x attr 0x%04x failed (0x%02x)", ep, cluster, attr, (unsigned)st);
    }
}

void zb_device_publish_floor_sensors(uint8_t floor, const floor_readings_t *r)
{
    if (!s_zb_ready || floor >= GB_FLOOR_COUNT) {
        return;
    }
    uint8_t ep = FLOOR_EP(floor);
    uint8_t caps = sensors_floor_caps(floor);
    esp_zigbee_lock_acquire(portMAX_DELAY);
    if (r->temp_valid) {
        int16_t v = (int16_t)lroundf(r->temp_c * 100);
        set_attr(ep, EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT, EZB_ZCL_ATTR_TEMPERATURE_MEASUREMENT_MEASURED_VALUE_ID,
                 &v);
    }
    if ((caps & SENSOR_CAP_HUMIDITY) && r->rh_valid) {
        uint16_t v = (uint16_t)lroundf(r->rh * 100);
        set_attr(ep, EZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                 EZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_MEASURED_VALUE_ID, &v);
    }
    if ((caps & SENSOR_CAP_PRESSURE) && r->pressure_valid) {
        int16_t hpa = (int16_t)lroundf(r->pressure_hpa); /* ZCL unit 0.1 kPa == 1 hPa */
        set_attr(ep, EZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT, EZB_ZCL_ATTR_PRESSURE_MEASUREMENT_MEASURED_VALUE_ID, &hpa);
    }
    if ((caps & SENSOR_CAP_LUX) && r->lux_valid) {
        uint16_t lux = gb_lux_to_zcl(r->lux);
        set_attr(ep, EZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT, EZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID,
                 &lux);
    }
    if ((caps & SENSOR_CAP_SOIL) && r->soil_valid) {
        uint16_t pct = r->soil_pct_x100;
        set_attr(ep, GROWBOX_CLUSTER_ID, ATTR_SOIL_MOISTURE, &pct);
    }
    esp_zigbee_lock_release();
}

/* Caller holds the Zigbee lock. Writes `value` only if it differs from the last published one. */
static void publish_u8(uint8_t ep, uint8_t *last, uint8_t value, uint16_t cluster, uint16_t attr)
{
    if (*last == value) {
        return;
    }
    set_attr(ep, cluster, attr, &value);
    *last = value;
}

void zb_device_publish_floor_state(uint8_t floor, const gb_settings_t *s, gb_light_output_t out, bool heat_lock)
{
    if (!s_zb_ready || floor >= GB_FLOOR_COUNT) {
        return;
    }
    uint8_t ep = FLOOR_EP(floor);
    published_t *p = &s_pub[floor];
    esp_zigbee_lock_acquire(portMAX_DELAY);
    publish_u8(ep, &p->on, out.on, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
    publish_u8(ep, &p->level, out.level, EZB_ZCL_CLUSTER_ID_LEVEL, EZB_ZCL_ATTR_LEVEL_CURRENT_LEVEL_ID);
    publish_u8(ep, &p->enabled, s->enabled, GROWBOX_CLUSTER_ID, ATTR_FLOOR_ENABLED);
    publish_u8(ep, &p->auto_mode, s->auto_mode, GROWBOX_CLUSTER_ID, ATTR_AUTO_MODE);
    publish_u8(ep, &p->on_hour, s->on_hour, GROWBOX_CLUSTER_ID, ATTR_ON_HOUR);
    publish_u8(ep, &p->off_hour, s->off_hour, GROWBOX_CLUSTER_ID, ATTR_OFF_HOUR);
    publish_u8(ep, &p->auto_brightness, s->auto_brightness_pct, GROWBOX_CLUSTER_ID, ATTR_AUTO_BRIGHTNESS);
    publish_u8(ep, &p->max_temp, s->max_temp_c, GROWBOX_CLUSTER_ID, ATTR_MAX_TEMP);
    publish_u8(ep, &p->heat_lock, heat_lock, GROWBOX_CLUSTER_ID, ATTR_HEAT_LOCK);
    esp_zigbee_lock_release();
}

void zb_device_publish_time_synced(bool synced)
{
    if (!s_zb_ready) {
        return;
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    for (uint8_t f = 0; f < GB_FLOOR_COUNT; f++) {
        publish_u8(FLOOR_EP(f), &s_pub[f].time_synced, synced, GROWBOX_CLUSTER_ID, ATTR_TIME_SYNCED);
    }
    esp_zigbee_lock_release();
}
