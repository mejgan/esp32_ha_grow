/*
 * DHT11 / DHT22 read with the RMT receiver: the transfer is timed by hardware, so no interrupts are disabled (a
 * bit-banged read blocks every interrupt, including the Zigbee radio, for ~5 ms).
 *
 * The receiver is armed before the start pulse and captures the whole exchange: host start (DHT11 ~20 ms,
 * DHT22 ~2 ms low), sensor response (80 us low + 80 us high) and 40 bits (50 us low + 26/70 us high). It stops
 * after the line has been idle for DHT_IDLE_US.
 *
 * The RMT channel is only held during a read, so any number of DHT sensors work although the ESP32-C6 has two
 * receive channels (reads run one after another from the sensor task).
 */
#include "dht.h"

#include "delay.h"
#include "esp_log.h"
#include "growbox_logic.h"

static const char *TAG = "dht";

#define DHT_RMT_RES_HZ   1000000 /* 1 tick = 1 us */
#define DHT11_START_MS   20      /* datasheet: at least 18 ms */
#define DHT22_START_MS   2       /* datasheet: 1..20 ms */
#define DHT_IDLE_US      32000   /* must exceed the start pulse; RMT idle limit is 32767 ticks */
#define DHT_GLITCH_NS    1000
#define DHT_MEM_SYMBOLS  48      /* one RMT memory block on the ESP32-C6, enough for the 43 symbols of a read */
#define DHT_TIMEOUT_MS   (DHT11_START_MS + 15 + DHT_IDLE_US / 1000 + 50)

static bool rx_done_cb(rmt_channel_handle_t chan, const rmt_rx_done_event_data_t *edata, void *ctx)
{
    (void)chan;
    BaseType_t woken = pdFALSE;
    size_t n = edata->num_symbols;
    xQueueSendFromISR((QueueHandle_t)ctx, &n, &woken);
    return woken == pdTRUE;
}

static esp_err_t channel_open(dht_t *dev)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << dev->pin,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(dev->pin, 1);

    if (dev->done == NULL) {
        dev->done = xQueueCreate(1, sizeof(size_t));
        if (dev->done == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    const rmt_rx_channel_config_t cfg = {
        .gpio_num = dev->pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = DHT_RMT_RES_HZ,
        .mem_block_symbols = DHT_MEM_SYMBOLS,
    };
    err = rmt_new_rx_channel(&cfg, &dev->chan);
    if (err != ESP_OK) {
        dev->chan = NULL;
        return err;
    }
    /* The RX channel routes the pad to its input; keep driving it as an open-drain output for the start pulse. */
    gpio_set_direction(dev->pin, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_level(dev->pin, 1);

    const rmt_rx_event_callbacks_t cbs = {.on_recv_done = rx_done_cb};
    err = rmt_rx_register_event_callbacks(dev->chan, &cbs, dev->done);
    if (err == ESP_OK) {
        err = rmt_enable(dev->chan);
    }
    if (err != ESP_OK) {
        rmt_del_channel(dev->chan);
        dev->chan = NULL;
    }
    return err;
}

static void channel_close(dht_t *dev)
{
    if (dev->chan != NULL) {
        rmt_disable(dev->chan);
        rmt_del_channel(dev->chan);
        dev->chan = NULL;
    }
    /* Leave the line released (pulled up) between reads. */
    gpio_set_direction(dev->pin, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_level(dev->pin, 1);
}

static esp_err_t read_frame(dht_t *dev, float *temp_c, float *rh);

esp_err_t dht_read(dht_t *dev, float *temp_c, float *rh)
{
    esp_err_t err = channel_open(dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RMT setup on GPIO%d failed: %s", dev->pin, esp_err_to_name(err));
        return err;
    }
    err = read_frame(dev, temp_c, rh);
    channel_close(dev);
    return err;
}

static esp_err_t read_frame(dht_t *dev, float *temp_c, float *rh)
{
    xQueueReset(dev->done);
    const rmt_receive_config_t rx = {
        .signal_range_min_ns = DHT_GLITCH_NS,
        .signal_range_max_ns = DHT_IDLE_US * 1000u,
    };
    esp_err_t err = rmt_receive(dev->chan, dev->symbols, sizeof(dev->symbols), &rx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d: rmt_receive failed: %s", dev->pin, esp_err_to_name(err));
        return err;
    }
    gpio_set_level(dev->pin, 0);
    delay_at_least_ms(dev->type == DHT_TYPE_DHT22 ? DHT22_START_MS : DHT11_START_MS);
    gpio_set_level(dev->pin, 1);

    size_t n = 0;
    if (xQueueReceive(dev->done, &n, pdMS_TO_TICKS(DHT_TIMEOUT_MS) + 1) != pdTRUE) {
        ESP_LOGW(TAG, "GPIO%d: no transfer captured", dev->pin);
        return ESP_ERR_TIMEOUT;
    }

    gb_pulse_t pulses[2 * 64];
    size_t np = 0;
    for (size_t i = 0; i < n && i < 64; i++) {
        if (dev->symbols[i].duration0) {
            pulses[np++] = (gb_pulse_t){(uint8_t)dev->symbols[i].level0, (uint16_t)dev->symbols[i].duration0};
        }
        if (dev->symbols[i].duration1) {
            pulses[np++] = (gb_pulse_t){(uint8_t)dev->symbols[i].level1, (uint16_t)dev->symbols[i].duration1};
        }
    }
    uint8_t frame[5];
    if (!gb_dht11_frame_from_pulses(pulses, np, frame)) {
        ESP_LOGW(TAG, "GPIO%d: incomplete transfer (%u symbols)", dev->pin, (unsigned)n);
        return ESP_ERR_TIMEOUT;
    }
    bool ok = dev->type == DHT_TYPE_DHT22 ? gb_dht22_decode(frame, temp_c, rh) : gb_dht11_decode(frame, temp_c, rh);
    if (!ok) {
        ESP_LOGW(TAG, "GPIO%d: bad frame %02x %02x %02x %02x %02x", dev->pin, frame[0], frame[1], frame[2], frame[3],
                 frame[4]);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}
