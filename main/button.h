#pragma once

#include <stdint.h>

typedef void (*button_cb_t)(void);

/*
 * Polls the button (CONFIG_GROWBOX_BUTTON_GPIO, active low; no-op when -1). Released within 3 s: `short_press`.
 * Held for CONFIG_GROWBOX_BUTTON_RESET_S: `long_press` (released earlier: nothing). Callbacks run in the
 * esp_timer task.
 */
void button_start(button_cb_t short_press, button_cb_t long_press);

/* Seconds left until the long press fires while the button is held past 3 s, else 0. */
uint8_t button_reset_countdown_s(void);
