/*
 * DHT11 read with the RMT receiver: the transfer is timed by hardware, so no interrupts are disabled (the old
 * bit-banged read blocked every interrupt, including the Zigbee radio, for ~5 ms).
 *
 * The receiver is armed before the start pulse and captures the whole exchange: host start (~20 ms low),
 * sensor response (80 us low + 80 us high) and 40 bits (50 us low + 26/70 us high). It stops after the line
 * has been idle for DHT_IDLE_US.
 */
#include "dht11.h"

#include "delay.h"
#include "driver/rmt_rx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "growbox_logic.h"

static const char *TAG = "dht11";

#define DHT_RMT_RES_HZ    1000000 /* 1 tick = 1 us */
#define DHT_START_LOW_MS  20      /* datasheet: at least 18 ms */
#define DHT_IDLE_US       32000   /* must exceed the start pulse; RMT idle limit is 32767 ticks */
#define DHT_GLITCH_NS     1000
#define DHT_MEM_SYMBOLS   48      /* one RMT memory block on the ESP32-C6, enough for the 43 symbols of a read */
#define DHT_TIMEOUT_MS    (DHT_START_LOW_MS + 15 + DHT_IDLE_US / 1000 + 50)

static gpio_num_t s_pin = GPIO_NUM_NC;
static rmt_channel_handle_t s_chan;
static QueueHandle_t s_done;
static rmt_symbol_word_t s_symbols[64];

static bool rx_done_cb(rmt_channel_handle_t chan, const rmt_rx_done_event_data_t *edata, void *ctx)
{
    (void)chan;
    BaseType_t woken = pdFALSE;
    size_t n = edata->num_symbols;
    xQueueSendFromISR((QueueHandle_t)ctx, &n, &woken);
    return woken == pdTRUE;
}

static esp_err_t dht_init(gpio_num_t pin)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(pin, 1);

    if (s_done == NULL) {
        s_done = xQueueCreate(1, sizeof(size_t));
        if (s_done == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    const rmt_rx_channel_config_t cfg = {
        .gpio_num = pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = DHT_RMT_RES_HZ,
        .mem_block_symbols = DHT_MEM_SYMBOLS,
    };
    err = rmt_new_rx_channel(&cfg, &s_chan);
    if (err != ESP_OK) {
        return err;
    }
    /* The RX channel routes the pad to its input; keep driving it as an open-drain output for the start pulse. */
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_level(pin, 1);

    const rmt_rx_event_callbacks_t cbs = {.on_recv_done = rx_done_cb};
    err = rmt_rx_register_event_callbacks(s_chan, &cbs, s_done);
    if (err == ESP_OK) {
        err = rmt_enable(s_chan);
    }
    if (err != ESP_OK) {
        rmt_del_channel(s_chan);
        s_chan = NULL;
        return err;
    }
    s_pin = pin;
    return ESP_OK;
}

esp_err_t dht11_read(gpio_num_t pin, float *temp_c, float *rh)
{
    if (s_pin != pin) {
        esp_err_t err = dht_init(pin);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "RMT setup on GPIO%d failed: %s", pin, esp_err_to_name(err));
            return err;
        }
    }

    xQueueReset(s_done);
    const rmt_receive_config_t rx = {
        .signal_range_min_ns = DHT_GLITCH_NS,
        .signal_range_max_ns = DHT_IDLE_US * 1000u,
    };
    esp_err_t err = rmt_receive(s_chan, s_symbols, sizeof(s_symbols), &rx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rmt_receive failed: %s", esp_err_to_name(err));
        return err;
    }
    gpio_set_level(pin, 0);
    delay_at_least_ms(DHT_START_LOW_MS);
    gpio_set_level(pin, 1);

    size_t n = 0;
    if (xQueueReceive(s_done, &n, pdMS_TO_TICKS(DHT_TIMEOUT_MS) + 1) != pdTRUE) {
        /* Abort the pending receive so the next read starts clean. */
        rmt_disable(s_chan);
        rmt_enable(s_chan);
        ESP_LOGW(TAG, "no transfer captured");
        return ESP_ERR_TIMEOUT;
    }

    gb_pulse_t pulses[2 * 64];
    size_t np = 0;
    for (size_t i = 0; i < n && i < 64; i++) {
        if (s_symbols[i].duration0) {
            pulses[np++] = (gb_pulse_t){(uint8_t)s_symbols[i].level0, (uint16_t)s_symbols[i].duration0};
        }
        if (s_symbols[i].duration1) {
            pulses[np++] = (gb_pulse_t){(uint8_t)s_symbols[i].level1, (uint16_t)s_symbols[i].duration1};
        }
    }
    uint8_t frame[5];
    if (!gb_dht11_frame_from_pulses(pulses, np, frame)) {
        ESP_LOGW(TAG, "incomplete transfer (%u symbols)", (unsigned)n);
        return ESP_ERR_TIMEOUT;
    }
    if (!gb_dht11_decode(frame, temp_c, rh)) {
        ESP_LOGW(TAG, "bad frame %02x %02x %02x %02x %02x", frame[0], frame[1], frame[2], frame[3], frame[4]);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}
