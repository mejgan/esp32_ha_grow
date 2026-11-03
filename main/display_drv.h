/*
 * Display driver interface. Exactly one disp_*.c implements it, picked by the display type in menuconfig
 * (Grow Box -> Display). Drivers show a character grid rendered by growbox_ui.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "growbox_ui.h"

/* Brings the display up and reports its character grid. May be called again after a failure. */
esp_err_t disp_drv_init(uint8_t *cols, uint8_t *rows);

/* Shows `s`; only what changed since the previous successful draw is sent. */
esp_err_t disp_drv_draw(const gb_ui_screen_t *s);

/* Panel (and backlight) on/off; the content is kept. */
esp_err_t disp_drv_power(bool on);
