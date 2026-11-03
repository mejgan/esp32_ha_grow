#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Sleeps for at least `ms`. pdMS_TO_TICKS() rounds down (5 ms is 0 ticks at the 100 Hz default) and vTaskDelay(n)
 * may return up to one tick early, so round up and add a tick. Use this wherever a sensor needs a minimum wait.
 */
static inline void delay_at_least_ms(uint32_t ms)
{
    TickType_t ticks = (TickType_t)(((uint64_t)ms * configTICK_RATE_HZ + 999u) / 1000u) + 1u;
    vTaskDelay(ticks);
}
