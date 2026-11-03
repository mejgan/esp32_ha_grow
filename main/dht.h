#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    DHT_TYPE_DHT11,
    DHT_TYPE_DHT22, /* also AM2302 */
} dht_type_t;

/* One DHT sensor on its own GPIO. An RMT receive channel is taken only for the duration of a read. */
typedef struct {
    gpio_num_t pin;
    dht_type_t type;
    rmt_channel_handle_t chan; /* only during a read */
    QueueHandle_t done;
    rmt_symbol_word_t symbols[64];
} dht_t;

#define DHT_INIT(gpio, dht_type) {.pin = (gpio), .type = (dht_type)}

/* Reads one measurement. Allow at least 1 s (DHT11) / 2 s (DHT22) between reads. */
esp_err_t dht_read(dht_t *dev, float *temp_c, float *rh);
