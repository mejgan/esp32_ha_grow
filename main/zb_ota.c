#include "zb_ota.h"

#include "app.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "ezbee/zha.h"
#include "growbox_logic.h"
#include "sdkconfig.h"

static const char *TAG = "zb_ota";

#ifndef GROWBOX_OTA_FILE_VERSION
#error "GROWBOX_OTA_FILE_VERSION is set by main/CMakeLists.txt from PROJECT_VER"
#endif

#define OTA_BLOCK_SIZE 223

static const esp_partition_t *s_partition;
static esp_ota_handle_t s_handle;
static bool s_active;
static gb_ota_parser_t s_parser;
static uint32_t s_file_size;
static uint8_t s_last_pct;

esp_err_t zb_ota_add_client_cluster(ezb_af_ep_desc_t ep)
{
    static ezb_zcl_ota_upgrade_cluster_client_config_t cfg = {
        .upgrade_server_id = EZB_ZCL_OTA_UPGRADE_UPGRADE_SERVER_ID_DEFAULT_VALUE,
        .file_offset = 0,
        .image_upgrade_status = EZB_ZCL_OTA_UPGRADE_IMAGE_UPGRADE_STATUS_DEFAULT_VALUE,
        .manufacturer_id = CONFIG_GROWBOX_OTA_MANUF_CODE,
        .image_type_id = CONFIG_GROWBOX_OTA_IMAGE_TYPE,
    };
    static uint32_t current_version = GROWBOX_OTA_FILE_VERSION;
    ezb_zcl_cluster_desc_t desc = ezb_zcl_ota_upgrade_create_cluster_desc(&cfg, EZB_ZCL_CLUSTER_CLIENT);
    ESP_RETURN_ON_ERROR(esp_zigbee_err_to_esp(ezb_zcl_ota_upgrade_cluster_desc_add_attr(
                            desc, EZB_ZCL_ATTR_OTA_UPGRADE_CURRENT_FILE_VERSION_ID, &current_version)),
                        TAG, "file version attribute");
    ESP_LOGI(TAG, "firmware file version 0x%08lx (manufacturer 0x%04x, image type 0x%04x)",
             (unsigned long)current_version, CONFIG_GROWBOX_OTA_MANUF_CODE, CONFIG_GROWBOX_OTA_IMAGE_TYPE);
    return esp_zigbee_err_to_esp(ezb_af_endpoint_add_cluster_desc(ep, desc));
}

void zb_ota_after_register(uint8_t ep_id)
{
    ezb_zcl_ota_upgrade_set_download_block_size(ep_id, OTA_BLOCK_SIZE);
}

static int ota_sink(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    return esp_ota_write(s_handle, data, len) == ESP_OK ? 0 : -1;
}

static void abort_update(void)
{
    if (s_active) {
        esp_ota_abort(s_handle);
        s_active = false;
    }
}

static esp_err_t on_start(const ezb_zcl_ota_upgrade_client_progress_start_t *st)
{
    ESP_LOGI(TAG, "update start: manufacturer 0x%04x, type 0x%04x, version 0x%08lx, %lu bytes", st->manuf_code,
             st->image_type, (unsigned long)st->file_version, (unsigned long)st->image_size);
    abort_update();
    if (st->manuf_code != CONFIG_GROWBOX_OTA_MANUF_CODE || st->image_type != CONFIG_GROWBOX_OTA_IMAGE_TYPE) {
        ESP_LOGW(TAG, "image is not for this device");
        return ESP_ERR_INVALID_ARG;
    }
    s_partition = esp_ota_get_next_update_partition(NULL);
    ESP_RETURN_ON_FALSE(s_partition != NULL, ESP_ERR_NOT_FOUND, TAG,
                        "no OTA partition: flash the current partition table over USB once");
    ESP_RETURN_ON_FALSE(st->image_size <= s_partition->size + 1024, ESP_ERR_INVALID_SIZE, TAG,
                        "image larger than the partition");
    ESP_RETURN_ON_ERROR(esp_ota_begin(s_partition, OTA_WITH_SEQUENTIAL_WRITES, &s_handle), TAG, "esp_ota_begin");
    gb_ota_parser_init(&s_parser);
    s_file_size = st->image_size;
    s_last_pct = 0;
    s_active = true;
    return ESP_OK;
}

static esp_err_t on_receive(const ezb_zcl_ota_upgrade_client_progress_receiving_t *rx)
{
    ESP_RETURN_ON_FALSE(s_active, ESP_ERR_INVALID_STATE, TAG, "block without start");
    gb_ota_err_t err = gb_ota_parser_feed(&s_parser, rx->block, rx->block_size, ota_sink, NULL);
    ESP_RETURN_ON_FALSE(err == GB_OTA_OK, ESP_FAIL, TAG, "bad OTA file or flash write failed (%d)", err);
    uint32_t done = rx->file_offset + rx->block_size;
    uint8_t pct = s_file_size ? (uint8_t)((uint64_t)done * 100 / s_file_size) : 0;
    if (pct >= s_last_pct + 10) {
        s_last_pct = pct;
        ESP_LOGI(TAG, "received %u%% (%lu / %lu)", pct, (unsigned long)done, (unsigned long)s_file_size);
    }
    return ESP_OK;
}

static esp_err_t on_check(void)
{
    ESP_RETURN_ON_FALSE(s_active, ESP_ERR_INVALID_STATE, TAG, "check without start");
    ESP_RETURN_ON_FALSE(gb_ota_parser_done(&s_parser), ESP_ERR_INVALID_SIZE, TAG, "OTA file incomplete");
    ESP_RETURN_ON_FALSE(s_parser.image_bytes > 0, ESP_ERR_INVALID_SIZE, TAG, "OTA file has no upgrade image");
    /* esp_ota_end validates the app image (header, segments, checksum/hash). */
    esp_err_t err = esp_ota_end(s_handle);
    s_active = false;
    ESP_RETURN_ON_ERROR(err, TAG, "image validation failed");
    return ESP_OK;
}

static void handle_progress(ezb_zcl_ota_upgrade_client_progress_message_t *msg)
{
    esp_err_t err = ESP_OK;
    switch (msg->in.progress) {
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_START:
        err = on_start(&msg->in.start);
        break;
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_RECEIVING:
        err = on_receive(&msg->in.receiving);
        break;
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_CHECK:
        err = on_check();
        break;
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_APPLY:
        ESP_LOGI(TAG, "applying version 0x%08lx", (unsigned long)msg->in.apply.file_version);
        err = s_partition ? esp_ota_set_boot_partition(s_partition) : ESP_ERR_INVALID_STATE;
        break;
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_FINISH:
        ESP_LOGW(TAG, "update finished, restarting");
        app_save_settings_now();
        esp_restart();
        break;
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_ABORT:
        ESP_LOGW(TAG, "update aborted");
        abort_update();
        break;
    default:
        break;
    }
    if (err != ESP_OK) {
        abort_update();
    }
    msg->out.result = err == ESP_OK ? EZB_ZCL_STATUS_SUCCESS : EZB_ZCL_STATUS_ABORT;
}

bool zb_ota_handle_action(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_OTA_UPGRADE_CLIENT_PROGRESS_CB_ID:
        handle_progress(message);
        return true;
    case EZB_ZCL_CORE_OTA_UPGRADE_QUERY_NEXT_IMAGE_RSP_CB_ID: {
        ezb_zcl_ota_upgrade_query_next_image_rsp_message_t *msg = message;
        if (msg->in.image.status == EZB_ZCL_OTA_UPGRADE_STATUS_CODE_SUCCESS) {
            ESP_LOGI(TAG, "server offers version 0x%08lx (%lu bytes)", (unsigned long)msg->in.image.file_version,
                     (unsigned long)msg->in.image.size);
        } else {
            ESP_LOGI(TAG, "no new image (status 0x%02x)", msg->in.image.status);
        }
        msg->out.result = EZB_ZCL_STATUS_SUCCESS;
        return true;
    }
    default:
        return false;
    }
}

void zb_ota_confirm_running_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed (%s)", esp_err_to_name(err));
    }
}
