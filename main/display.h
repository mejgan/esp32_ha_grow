#pragma once

/* Starts the display task when a display is configured (no-op otherwise). Call after sensors_init(). */
void display_start(void);

/* Next page, or wakes the display if it timed out. Safe from any task. */
void display_next_page(void);
