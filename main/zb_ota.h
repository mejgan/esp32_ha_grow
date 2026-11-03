/*
 * Zigbee OTA Upgrade client: Zigbee2MQTT (or any OTA server) pushes build/growbox.ota, which is written to the
 * inactive app partition and booted. The new image must confirm itself (zb_ota_confirm_running_image) or the
 * bootloader rolls back to the previous one on the next reset.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_zigbee.h"

/* Adds the OTA Upgrade client cluster to `ep` (called while building the data model). */
esp_err_t zb_ota_add_client_cluster(ezb_af_ep_desc_t ep);

/* Settings that need the registered data model (block size). */
void zb_ota_after_register(uint8_t ep_id);

/* Handles OTA-related ZCL core actions; false for anything else. Zigbee context. */
bool zb_ota_handle_action(ezb_zcl_core_action_callback_id_t callback_id, void *message);

/* Marks a freshly updated image as good, cancelling the rollback. No-op otherwise. */
void zb_ota_confirm_running_image(void);
