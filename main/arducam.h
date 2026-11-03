/*
 * Arducam Mega (3 MP / 5 MP) SPI camera: takes a JPEG into the camera's own frame buffer, which is then read out
 * in chunks, so a photo never has to fit into the ESP32-C6's RAM. Protocol from Arducam's MIT-licensed SDK
 * (github.com/ArduCAM/Arducam_Mega).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"

typedef enum {
    ARDUCAM_RES_HD,   /* 1280x720 */
    ARDUCAM_RES_UXGA, /* 1600x1200 */
    ARDUCAM_RES_FHD,  /* 1920x1080 */
    ARDUCAM_RES_QXGA, /* 2048x1536 */
} arducam_res_t;

typedef struct {
    int cs_gpio;
    spi_device_handle_t dev;
    uint8_t sensor_id; /* 0x81..0x87, 0 = not found */
    uint32_t left;     /* bytes of the current photo not read yet */
    bool first_burst;
} arducam_t;

/* Adds the camera on the shared SPI bus (spi_bus_setup() first) and resets it. */
esp_err_t arducam_init(arducam_t *cam, int cs_gpio, uint32_t spi_hz);

/* Takes a JPEG photo; on success *len is its size in bytes. */
esp_err_t arducam_capture(arducam_t *cam, arducam_res_t res, uint32_t *len);

/* Reads the next up to `n` bytes of the photo into `buf` (DMA-capable, n a multiple of 4); returns bytes read. */
esp_err_t arducam_read(arducam_t *cam, uint8_t *buf, size_t n, size_t *got);
