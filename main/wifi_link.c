#include "wifi_link.h"

#include "camera.h"

#if CONFIG_GROWBOX_CAMERA_COUNT > 0 /* Wi-Fi is only used for camera uploads */

#include <string.h>

#include "esp_coexist.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";

#define BIT_GOT_IP BIT0
#define BIT_FAILED BIT1

static EventGroupHandle_t s_events;
static bool s_started;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "disconnected (reason %d)", d ? d->reason : -1);
        xEventGroupClearBits(s_events, BIT_GOT_IP);
        xEventGroupSetBits(s_events, BIT_FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

esp_err_t wifi_link_init(void)
{
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* may already exist */
        return err;
    }
    esp_netif_create_default_wifi_sta();
    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, CONFIG_GROWBOX_WIFI_SSID, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, CONFIG_GROWBOX_WIFI_PASSWORD, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = strlen(CONFIG_GROWBOX_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    err = esp_wifi_set_config(WIFI_IF_STA, &sta);
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    /* Share the radio with Zigbee (Espressif's zigbee_gateway example does the same). */
    esp_coex_wifi_i154_enable();
#endif
    return err;
}

esp_err_t wifi_link_up(uint32_t timeout_ms)
{
    if (strlen(CONFIG_GROWBOX_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "no Wi-Fi network configured (menuconfig: Grow Box -> Cameras)");
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_FAILED);
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }
    s_started = true;
    /* Modem power save while Zigbee shares the radio. */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) {
            ESP_LOGW(TAG, "no connection to \"%s\"", CONFIG_GROWBOX_WIFI_SSID);
            return ESP_ERR_TIMEOUT;
        }
        EventBits_t bits =
            xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_FAILED, pdTRUE, pdFALSE, deadline - now);
        if (bits & BIT_GOT_IP) {
            return ESP_OK;
        }
        if (bits & BIT_FAILED) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect(); /* retry until the deadline */
        }
    }
}

void wifi_link_down(void)
{
    if (s_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_started = false;
    }
}

#endif
