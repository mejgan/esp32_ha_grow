#include "display.h"

#include "sdkconfig.h"

#if CONFIG_GROWBOX_DISPLAY_NONE

void display_start(void)
{
}

void display_next_page(void)
{
}

#else

#include "app.h"
#include "button.h"
#include "display_drv.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "growbox_ui.h"

static const char *TAG = "display";

#define US_PER_S          1000000LL
#define REFRESH_MS        250
#define PAGE_US           ((int64_t)CONFIG_GROWBOX_DISPLAY_PAGE_S * US_PER_S)
#define TIMEOUT_US        ((int64_t)CONFIG_GROWBOX_DISPLAY_TIMEOUT_S * US_PER_S)
#define INIT_RETRY_US     (30 * US_PER_S)
#define REINIT_AFTER_ERRS 3
#define NOTIFY_NEXT_PAGE  0x1

static TaskHandle_t s_task;

void display_next_page(void)
{
    if (s_task != NULL) {
        xTaskNotify(s_task, NOTIFY_NEXT_PAGE, eSetBits);
    }
}

static void display_task(void *arg)
{
    (void)arg;
    static gb_ui_status_t st;
    static gb_ui_screen_t screen;
    uint8_t cols = 0, rows = 0;
    bool ready = false;
    bool power_on = true;
    int errors = 0;
    int64_t next_init_us = 0;
    uint8_t page = 0;
    int64_t page_since_us = esp_timer_get_time();
    int64_t last_activity_us = page_since_us;

    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(REFRESH_MS));
        int64_t now = esp_timer_get_time();

        if (!ready) {
            if (now < next_init_us) {
                continue;
            }
            esp_err_t err = disp_drv_init(&cols, &rows);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "init failed (%s), retrying in %llds", esp_err_to_name(err), INIT_RETRY_US / US_PER_S);
                next_init_us = now + INIT_RETRY_US;
                continue;
            }
            ESP_LOGI(TAG, "ready: %ux%u characters", cols, rows);
            ready = true;
            power_on = true;
            errors = 0;
            last_activity_us = now;
        }

        app_get_ui_status(&st);
        st.reset_countdown_s = button_reset_countdown_s();

        bool wake = st.reset_countdown_s > 0;
        if (bits & NOTIFY_NEXT_PAGE) {
            if (power_on) {
                page++;
                page_since_us = now;
            }
            wake = true;
        }
        if (wake) {
            last_activity_us = now;
            if (!power_on) {
                disp_drv_power(true);
                power_on = true;
            }
        }
        if (TIMEOUT_US > 0 && power_on && now - last_activity_us > TIMEOUT_US) {
            disp_drv_power(false);
            power_on = false;
        }
        if (!power_on) {
            continue;
        }

        if (now - page_since_us >= PAGE_US) {
            page++;
            page_since_us = now;
        }
        uint8_t pages = gb_ui_page_count(&st, cols, rows);
        page %= pages;
        gb_ui_render(&st, page, cols, rows, &screen);
        if (disp_drv_draw(&screen) != ESP_OK) {
            if (++errors >= REINIT_AFTER_ERRS) {
                ESP_LOGW(TAG, "display not responding, re-initializing");
                ready = false;
                next_init_us = now + 5 * US_PER_S;
            }
        } else {
            errors = 0;
        }
    }
}

void display_start(void)
{
    BaseType_t ok = xTaskCreate(display_task, "display", 6144, NULL, 2, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to start the display task");
        s_task = NULL;
    }
}

#endif
