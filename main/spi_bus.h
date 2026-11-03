/*
 * The SPI bus (SPI2) shared by a TFT display and the cameras. The TFT's SCLK/MOSI pins are the bus pins when there
 * is a TFT; the cameras add MISO.
 */
#pragma once

#include "driver/spi_master.h"
#include "esp_err.h"
#include "sdkconfig.h"

#define GB_SPI_HOST SPI2_HOST

#ifndef CONFIG_GROWBOX_CAMERA_COUNT
#define CONFIG_GROWBOX_CAMERA_COUNT 0
#endif

#if CONFIG_GROWBOX_DISPLAY_TFT
#define GB_SPI_SCLK CONFIG_GROWBOX_DISPLAY_SPI_SCLK_GPIO
#define GB_SPI_MOSI CONFIG_GROWBOX_DISPLAY_SPI_MOSI_GPIO
#elif CONFIG_GROWBOX_CAMERA_COUNT > 0
#define GB_SPI_SCLK CONFIG_GROWBOX_CAM_SPI_SCLK_GPIO
#define GB_SPI_MOSI CONFIG_GROWBOX_CAM_SPI_MOSI_GPIO
#endif

#if CONFIG_GROWBOX_CAMERA_COUNT > 0
#define GB_SPI_MISO CONFIG_GROWBOX_CAM_SPI_MISO_GPIO
#else
#define GB_SPI_MISO (-1)
#endif

/* Largest single SPI transfer: a camera read chunk (TFT cells are smaller). */
#define GB_SPI_MAX_TRANSFER 4096

/* Initializes the bus once; later calls return ESP_OK. Call from app_main before the display/camera tasks start. */
esp_err_t spi_bus_setup(void);
