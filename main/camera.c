#include "camera.h"

#if CONFIG_GROWBOX_CAMERA_COUNT == 0

void camera_start(void)
{
}

void camera_request(uint8_t floor)
{
    (void)floor;
}

#else

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app.h"
#include "arducam.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "growbox_ui.h"
#include "spi_bus.h"
#include "wifi_link.h"

static const char *TAG = "camera";

#define N_CAM           CONFIG_GROWBOX_CAMERA_COUNT
#define US_PER_S        1000000LL
#define INTERVAL_US     ((int64_t)CONFIG_GROWBOX_PHOTO_INTERVAL_MIN * 60 * US_PER_S)
#define CHECK_MS        30000
#define WIFI_TIMEOUT_MS 30000
#define RETRY_US        (5 * 60 * US_PER_S) /* after a failed Wi-Fi connection */
#define CHUNK           4096

#if CONFIG_GROWBOX_PHOTO_HD
#define PHOTO_RES ARDUCAM_RES_HD
#elif CONFIG_GROWBOX_PHOTO_FHD
#define PHOTO_RES ARDUCAM_RES_FHD
#elif CONFIG_GROWBOX_PHOTO_QXGA
#define PHOTO_RES ARDUCAM_RES_QXGA
#else
#define PHOTO_RES ARDUCAM_RES_UXGA
#endif

static const int k_cs[3] = {
    CONFIG_GROWBOX_CAM1_CS_GPIO,
#if N_CAM >= 2
    CONFIG_GROWBOX_CAM2_CS_GPIO,
#else
    -1,
#endif
#if N_CAM >= 3
    CONFIG_GROWBOX_CAM3_CS_GPIO,
#else
    -1,
#endif
};

/* GPIO16/17 carry the UART0 console unless it is moved to USB. */
#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) &&                                                                    \
    (CONFIG_GROWBOX_CAM_SPI_MISO_GPIO == 16 || CONFIG_GROWBOX_CAM_SPI_MISO_GPIO == 17 ||                           \
     CONFIG_GROWBOX_CAM1_CS_GPIO == 16 || CONFIG_GROWBOX_CAM1_CS_GPIO == 17 ||                                     \
     (N_CAM >= 2 && (CONFIG_GROWBOX_CAM2_CS_GPIO == 16 || CONFIG_GROWBOX_CAM2_CS_GPIO == 17)) ||                    \
     (N_CAM >= 3 && (CONFIG_GROWBOX_CAM3_CS_GPIO == 16 || CONFIG_GROWBOX_CAM3_CS_GPIO == 17)))
#error "A camera uses GPIO16/17 (UART0): set Component config -> ESP System Settings -> Channel for console output -> USB Serial/JTAG"
#endif

static arducam_t s_cam[N_CAM];
static bool s_cam_ok[N_CAM];
static int64_t s_last_us[N_CAM];
static volatile uint32_t s_requests;
static TaskHandle_t s_task;
static uint8_t *s_buf;

void camera_request(uint8_t floor)
{
    if (floor >= N_CAM || s_task == NULL) {
        return;
    }
    __atomic_fetch_or(&s_requests, 1u << floor, __ATOMIC_SEQ_CST);
    xTaskNotifyGive(s_task);
}

/* Every pin some feature uses; a camera pin that appears twice disables the cameras. */
static bool pins_ok(void)
{
    int pins[40];
    const char *what[40];
    int n = 0;
#define ADD(p, w)            \
    do {                     \
        if ((p) >= 0) {      \
            pins[n] = (p);   \
            what[n++] = (w); \
        }                    \
    } while (0)
    ADD(CONFIG_GROWBOX_F1_DHT11_GPIO, "floor 1 DHT11");
    ADD(CONFIG_GROWBOX_F1_SOIL_CAP_GPIO, "floor 1 soil");
    ADD(CONFIG_GROWBOX_F1_LIGHT_PWM_GPIO, "floor 1 light");
    ADD(CONFIG_GROWBOX_F2_SOIL_RES_GPIO, "floor 2 soil");
    ADD(CONFIG_GROWBOX_F2_SOIL_RES_PWR_GPIO, "floor 2 soil power");
    ADD(CONFIG_GROWBOX_F2_LIGHT_PWM_GPIO, "floor 2 light");
    ADD(CONFIG_GROWBOX_F3_I2C_SDA_GPIO, "I2C SDA");
    ADD(CONFIG_GROWBOX_F3_I2C_SCL_GPIO, "I2C SCL");
    ADD(CONFIG_GROWBOX_F3_LIGHT_SENSOR_GPIO, "floor 3 light sensor");
    ADD(CONFIG_GROWBOX_F3_LIGHT_PWM_GPIO, "floor 3 light");
    ADD(6, "LP I2C SDA");
    ADD(7, "LP I2C SCL");
    ADD(12, "USB D-");
    ADD(13, "USB D+");
    ADD(CONFIG_GROWBOX_BUTTON_GPIO, "button");
#if CONFIG_GROWBOX_ROOM_DHT
    ADD(CONFIG_GROWBOX_ROOM_GPIO, "room sensor");
#endif
#if CONFIG_GROWBOX_DISPLAY_TFT
    ADD(CONFIG_GROWBOX_DISPLAY_SPI_CS_GPIO, "TFT CS");
    ADD(CONFIG_GROWBOX_DISPLAY_SPI_DC_GPIO, "TFT DC");
    ADD(CONFIG_GROWBOX_DISPLAY_SPI_RST_GPIO, "TFT RST");
    ADD(CONFIG_GROWBOX_DISPLAY_BL_GPIO, "TFT backlight");
#endif
    int first_cam = n;
    ADD(GB_SPI_SCLK, "SPI SCLK");
    ADD(GB_SPI_MOSI, "SPI MOSI");
    ADD(GB_SPI_MISO, "camera SPI MISO");
    for (int i = 0; i < N_CAM; i++) {
        ADD(k_cs[i], "camera CS");
    }
#undef ADD
    bool ok = true;
    for (int i = first_cam; i < n; i++) {
        for (int j = 0; j < i; j++) {
            if (pins[i] == pins[j]) {
                ESP_LOGE(TAG, "GPIO%d is used by both %s and %s", pins[i], what[j], what[i]);
                ok = false;
            }
        }
    }
    return ok;
}

static void timestamp(char *buf, size_t n)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    time_t t = tv.tv_sec;
    gmtime_r(&t, &tm); /* the system clock holds local time (see zb_device.c) */
    if (tv.tv_sec < 1704067200) {
        snprintf(buf, n, "unknown");
    } else {
        strftime(buf, n, "%Y-%m-%dT%H:%M:%S", &tm);
    }
}

/* Takes a photo with camera `i` and streams it to the server. */
static esp_err_t upload_photo(int i)
{
    if (!s_cam_ok[i]) {
        s_cam_ok[i] = arducam_init(&s_cam[i], k_cs[i], CONFIG_GROWBOX_CAM_SPI_HZ) == ESP_OK;
        if (!s_cam_ok[i]) {
            return ESP_ERR_NOT_FOUND;
        }
    }
    uint32_t len = 0;
    esp_err_t err = arducam_capture(&s_cam[i], PHOTO_RES, &len);
    if (err != ESP_OK) {
        s_cam_ok[i] = false; /* set the camera up again next time */
        return err;
    }

    const esp_http_client_config_t cfg = {
        .url = CONFIG_GROWBOX_PHOTO_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach, /* https:// with a public CA certificate */
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    char value[48];
    esp_http_client_set_header(client, "Content-Type", "image/jpeg");
    snprintf(value, sizeof(value), "%d", i + 1);
    esp_http_client_set_header(client, "X-GrowBox-Floor", value);
    timestamp(value, sizeof(value));
    esp_http_client_set_header(client, "X-GrowBox-Time", value);
    if (strlen(CONFIG_GROWBOX_PHOTO_TOKEN) > 0) {
        esp_http_client_set_header(client, "Authorization", "Bearer " CONFIG_GROWBOX_PHOTO_TOKEN);
    }

    err = esp_http_client_open(client, (int)len);
    uint32_t sent = 0;
    while (err == ESP_OK && sent < len) {
        size_t got = 0;
        err = arducam_read(&s_cam[i], s_buf, CHUNK, &got);
        if (err != ESP_OK || got == 0) {
            err = err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
            break;
        }
        if (sent == 0 && !(s_buf[0] == 0xFF && s_buf[1] == 0xD8)) {
            ESP_LOGW(TAG, "camera %d: data is not a JPEG (%02x %02x)", i + 1, s_buf[0], s_buf[1]);
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        size_t off = 0;
        while (off < got) {
            int w = esp_http_client_write(client, (const char *)s_buf + off, (int)(got - off));
            if (w <= 0) {
                err = ESP_FAIL;
                break;
            }
            off += (size_t)w;
        }
        sent += got;
    }
    int status = 0;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (status < 200 || status > 299) {
            err = ESP_FAIL;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "floor %d: photo uploaded (%lu bytes)", i + 1, (unsigned long)len);
    } else {
        ESP_LOGW(TAG, "floor %d: upload failed (%s, HTTP %d)", i + 1, esp_err_to_name(err), status);
    }
    return err;
}

static void camera_task(void *arg)
{
    (void)arg;
    static gb_ui_status_t st;
    int64_t retry_after = 0;

    for (int i = 0; i < N_CAM; i++) {
        s_cam_ok[i] = arducam_init(&s_cam[i], k_cs[i], CONFIG_GROWBOX_CAM_SPI_HZ) == ESP_OK;
    }
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CHECK_MS));
        int64_t now = esp_timer_get_time();
        app_get_ui_status(&st);
        uint32_t requested = __atomic_exchange_n(&s_requests, 0, __ATOMIC_SEQ_CST);
        uint32_t due = requested;
        for (int i = 0; i < N_CAM; i++) {
            bool lit = st.floor[i].enabled && st.floor[i].light_on;
            if (lit && (s_last_us[i] == 0 || now - s_last_us[i] >= INTERVAL_US)) {
                due |= 1u << i;
            }
        }
        if (due == 0 || (requested == 0 && now < retry_after)) {
            continue;
        }
        if (wifi_link_up(WIFI_TIMEOUT_MS) != ESP_OK) {
            wifi_link_down();
            retry_after = now + RETRY_US;
            continue;
        }
        for (int i = 0; i < N_CAM; i++) {
            if ((due & (1u << i)) && upload_photo(i) == ESP_OK) {
                s_last_us[i] = esp_timer_get_time();
            }
        }
        wifi_link_down();
    }
}

void camera_start(void)
{
    if (!pins_ok()) {
        ESP_LOGE(TAG, "cameras disabled: fix the GPIO conflict in menuconfig (Grow Box -> Cameras)");
        return;
    }
    if (spi_bus_setup() != ESP_OK || wifi_link_init() != ESP_OK) {
        ESP_LOGE(TAG, "cameras disabled: SPI or Wi-Fi setup failed");
        return;
    }
    s_buf = heap_caps_malloc(CHUNK, MALLOC_CAP_DMA);
    if (s_buf == NULL) {
        ESP_LOGE(TAG, "cameras disabled: no memory");
        return;
    }
    if (xTaskCreate(camera_task, "camera", 6144, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "cameras disabled: no memory for the task");
    }
}

#endif
