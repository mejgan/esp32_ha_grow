#include "spi_bus.h"

#include <stdbool.h>

#include "esp_log.h"

#if defined(GB_SPI_SCLK)

static const char *TAG = "spi_bus";
static bool s_ready;

esp_err_t spi_bus_setup(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    const spi_bus_config_t bus = {
        .mosi_io_num = GB_SPI_MOSI,
        .miso_io_num = GB_SPI_MISO,
        .sclk_io_num = GB_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = GB_SPI_MAX_TRANSFER + 16,
    };
    esp_err_t err = spi_bus_initialize(GB_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    return ESP_OK;
}

#else

esp_err_t spi_bus_setup(void)
{
    return ESP_OK; /* nothing on SPI */
}

#endif
