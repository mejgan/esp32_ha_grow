#include "arducam.h"

#include <string.h>

#include "delay.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "spi_bus.h"

static const char *TAG = "arducam";

/* Registers (Arducam Mega SDK, ArducamCamera.c). */
#define REG_FIFO          0x04
#define FIFO_CLEAR_ID     0x01
#define FIFO_START        0x02
#define REG_SENSOR_RESET  0x07
#define SENSOR_RESET      0x40
#define REG_FORMAT        0x20
#define FORMAT_JPEG       0x01
#define REG_RESOLUTION    0x21
#define REG_QUALITY       0x2A
#define REG_SENSOR_ID     0x40
#define REG_STATE         0x44 /* bits 0-1: 2 = sensor bus idle; bit 2: capture done */
#define STATE_IDLE        0x02
#define STATE_CAP_DONE    0x04
#define REG_FIFO_SIZE1    0x45
#define REG_FIFO_SIZE2    0x46
#define REG_FIFO_SIZE3    0x47
#define CMD_BURST_READ    0x3C

#define SENSOR_FIRST_NEW  0x85 /* 0x85.. take resolution codes directly, older ones need the legacy table */
#define IDLE_TIMEOUT_US   (1000 * 1000)
#define CAPTURE_TIMEOUT_US (5 * 1000 * 1000)

static esp_err_t write_reg(arducam_t *cam, uint8_t reg, uint8_t val)
{
    spi_transaction_t t = {
        .cmd = (uint16_t)(reg | 0x80),
        .length = 8,
        .flags = SPI_TRANS_USE_TXDATA,
        .tx_data = {val},
    };
    return spi_device_polling_transmit(cam->dev, &t);
}

static esp_err_t read_reg(arducam_t *cam, uint8_t reg, uint8_t *val)
{
    /* Address byte, one dummy byte, then the value. */
    spi_transaction_ext_t t = {
        .base =
            {
                .cmd = (uint16_t)(reg & 0x7F),
                .rxlength = 8,
                .flags = SPI_TRANS_USE_RXDATA | SPI_TRANS_VARIABLE_DUMMY,
            },
        .dummy_bits = 8,
    };
    esp_err_t err = spi_device_polling_transmit(cam->dev, &t.base);
    if (err == ESP_OK) {
        *val = t.base.rx_data[0];
    }
    return err;
}

static esp_err_t wait_idle(arducam_t *cam)
{
    int64_t start = esp_timer_get_time();
    for (;;) {
        uint8_t st = 0;
        esp_err_t err = read_reg(cam, REG_STATE, &st);
        if (err != ESP_OK) {
            return err;
        }
        if ((st & 0x03) == STATE_IDLE) {
            return ESP_OK;
        }
        if (esp_timer_get_time() - start > IDLE_TIMEOUT_US) {
            return ESP_ERR_TIMEOUT;
        }
        delay_at_least_ms(2);
    }
}

static esp_err_t write_reg_idle(arducam_t *cam, uint8_t reg, uint8_t val)
{
    esp_err_t err = write_reg(cam, reg, val);
    return err == ESP_OK ? wait_idle(cam) : err;
}

esp_err_t arducam_init(arducam_t *cam, int cs_gpio, uint32_t spi_hz)
{
    cam->cs_gpio = cs_gpio;
    cam->sensor_id = 0;
    cam->left = 0;
    if (cam->dev == NULL) {
        const spi_device_interface_config_t dev = {
            .command_bits = 8,
            .mode = 0,
            .clock_speed_hz = (int)spi_hz,
            .spics_io_num = cs_gpio,
            .queue_size = 1,
            .flags = SPI_DEVICE_HALFDUPLEX,
        };
        esp_err_t err = spi_bus_add_device(GB_SPI_HOST, &dev, &cam->dev);
        if (err != ESP_OK) {
            cam->dev = NULL;
            return err;
        }
    }
    esp_err_t err = write_reg_idle(cam, REG_SENSOR_RESET, SENSOR_RESET);
    uint8_t id = 0;
    if (err == ESP_OK) {
        err = read_reg(cam, REG_SENSOR_ID, &id);
    }
    if (err == ESP_OK && (id < 0x81 || id > 0x87)) {
        ESP_LOGW(TAG, "CS GPIO%d: no Arducam Mega (sensor id 0x%02x)", cs_gpio, id);
        return ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_OK) {
        err = wait_idle(cam);
    }
    if (err == ESP_OK) {
        cam->sensor_id = id;
        ESP_LOGI(TAG, "CS GPIO%d: Arducam Mega, sensor id 0x%02x", cs_gpio, id);
    }
    return err;
}

static uint8_t resolution_code(const arducam_t *cam, arducam_res_t res)
{
    static const uint8_t current[] = {[ARDUCAM_RES_HD] = 0x08, [ARDUCAM_RES_UXGA] = 0x0a, [ARDUCAM_RES_FHD] = 0x0b,
                                      [ARDUCAM_RES_QXGA] = 0x0c};
    static const uint8_t legacy[] = {[ARDUCAM_RES_HD] = 0x04, [ARDUCAM_RES_UXGA] = 0x06, [ARDUCAM_RES_FHD] = 0x07,
                                     [ARDUCAM_RES_QXGA] = 0x08};
    return cam->sensor_id >= SENSOR_FIRST_NEW ? current[res] : legacy[res];
}

esp_err_t arducam_capture(arducam_t *cam, arducam_res_t res, uint32_t *len)
{
    if (cam->sensor_id == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = write_reg_idle(cam, REG_FORMAT, FORMAT_JPEG);
    if (err == ESP_OK) {
        err = write_reg_idle(cam, REG_RESOLUTION, resolution_code(cam, res)); /* bit 7 = 0: still capture */
    }
    if (err == ESP_OK) {
        err = write_reg_idle(cam, REG_QUALITY, 0); /* high quality */
    }
    if (err == ESP_OK) {
        err = write_reg(cam, REG_FIFO, FIFO_CLEAR_ID);
    }
    if (err == ESP_OK) {
        err = write_reg(cam, REG_FIFO, FIFO_START);
    }
    int64_t start = esp_timer_get_time();
    while (err == ESP_OK) {
        uint8_t st = 0;
        err = read_reg(cam, REG_STATE, &st);
        if (err == ESP_OK && (st & STATE_CAP_DONE)) {
            break;
        }
        if (esp_timer_get_time() - start > CAPTURE_TIMEOUT_US) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        delay_at_least_ms(10);
    }
    uint8_t l1 = 0, l2 = 0, l3 = 0;
    if (err == ESP_OK) {
        err = read_reg(cam, REG_FIFO_SIZE1, &l1);
    }
    if (err == ESP_OK) {
        err = read_reg(cam, REG_FIFO_SIZE2, &l2);
    }
    if (err == ESP_OK) {
        err = read_reg(cam, REG_FIFO_SIZE3, &l3);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CS GPIO%d: capture failed: %s", cam->cs_gpio, esp_err_to_name(err));
        return err;
    }
    cam->left = ((uint32_t)l3 << 16 | (uint32_t)l2 << 8 | l1) & 0xFFFFFF;
    cam->first_burst = true;
    *len = cam->left;
    return cam->left > 0 ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t arducam_read(arducam_t *cam, uint8_t *buf, size_t n, size_t *got)
{
    *got = 0;
    if (cam->left == 0) {
        return ESP_OK;
    }
    size_t want = n < cam->left ? n : cam->left;
    size_t rx = (want + 3) & ~(size_t)3; /* DMA reads whole words; extra bytes are ignored */
    if (rx > n) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* Burst read command; the very first burst of a photo starts with one dummy byte. */
    spi_transaction_ext_t t = {
        .base =
            {
                .cmd = CMD_BURST_READ,
                .rxlength = rx * 8,
                .rx_buffer = buf,
                .flags = SPI_TRANS_VARIABLE_DUMMY,
            },
        .dummy_bits = cam->first_burst ? 8 : 0,
    };
    esp_err_t err = spi_device_transmit(cam->dev, &t.base);
    if (err != ESP_OK) {
        return err;
    }
    cam->first_burst = false;
    cam->left -= want;
    *got = want;
    return ESP_OK;
}
