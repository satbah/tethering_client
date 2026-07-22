#include "ssd1309.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SSD1309";
static i2c_master_dev_handle_t oled;

/* 5x7 ASCII font, columns LSB first; unsupported characters are rendered as ?. */
static const uint8_t font[][5] = {
    [' ']={0,0,0,0,0}, ['!']={0,0,0x5f,0,0}, ['.']={0x60,0x60,0,0,0}, ['-']={0x08,0x08,0x08,0x08,0x08},
    ['/']={0x20,0x10,0x08,0x04,0x02}, [':']={0,0x36,0x36,0,0}, ['0']={0x3e,0x51,0x49,0x45,0x3e}, ['1']={0,0x42,0x7f,0x40,0},
    ['2']={0x42,0x61,0x51,0x49,0x46}, ['3']={0x21,0x41,0x45,0x4b,0x31}, ['4']={0x18,0x14,0x12,0x7f,0x10}, ['5']={0x27,0x45,0x45,0x45,0x39},
    ['6']={0x3c,0x4a,0x49,0x49,0x30}, ['7']={0x01,0x71,0x09,0x05,0x03}, ['8']={0x36,0x49,0x49,0x49,0x36}, ['9']={0x06,0x49,0x49,0x29,0x1e},
    ['A']={0x7e,0x11,0x11,0x11,0x7e}, ['B']={0x7f,0x49,0x49,0x49,0x36}, ['C']={0x3e,0x41,0x41,0x41,0x22}, ['D']={0x7f,0x41,0x41,0x22,0x1c},
    ['E']={0x7f,0x49,0x49,0x49,0x41}, ['F']={0x7f,0x09,0x09,0x09,0x01}, ['G']={0x3e,0x41,0x49,0x49,0x7a}, ['H']={0x7f,0x08,0x08,0x08,0x7f},
    ['I']={0,0x41,0x7f,0x41,0}, ['J']={0x20,0x40,0x41,0x3f,0x01}, ['K']={0x7f,0x08,0x14,0x22,0x41}, ['L']={0x7f,0x40,0x40,0x40,0x40},
    ['M']={0x7f,0x02,0x0c,0x02,0x7f}, ['N']={0x7f,0x04,0x08,0x10,0x7f}, ['O']={0x3e,0x41,0x41,0x41,0x3e}, ['P']={0x7f,0x09,0x09,0x09,0x06},
    ['Q']={0x3e,0x41,0x51,0x21,0x5e}, ['R']={0x7f,0x09,0x19,0x29,0x46}, ['S']={0x46,0x49,0x49,0x49,0x31}, ['T']={0x01,0x01,0x7f,0x01,0x01},
    ['U']={0x3f,0x40,0x40,0x40,0x3f}, ['V']={0x1f,0x20,0x40,0x20,0x1f}, ['W']={0x7f,0x20,0x18,0x20,0x7f}, ['X']={0x63,0x14,0x08,0x14,0x63},
    ['Y']={0x03,0x04,0x78,0x04,0x03}, ['Z']={0x61,0x51,0x49,0x45,0x43}, ['?']={0x02,0x01,0x51,0x09,0x06},
};

static esp_err_t command(const uint8_t *data, size_t len) { return i2c_master_transmit(oled, data, len, 1000); }

esp_err_t ssd1309_init(const ssd1309_config_t *cfg) {
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bus_cfg = {.i2c_port = cfg->i2c_port, .sda_io_num = cfg->sda_gpio, .scl_io_num = cfg->scl_gpio, .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true};
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), "SSD1309", "I2C bus");

    const uint8_t candidates[] = {
        cfg->address,
        (uint8_t)(cfg->address == 0x3C ? 0x3D : 0x3C),
    };
    uint8_t found = 0;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        esp_err_t probe = i2c_master_probe(bus, candidates[i], 50);
        if (probe == ESP_OK) {
            found = candidates[i];
            ESP_LOGI(TAG, "OLED responding at 0x%02X", found);
            break;
        }
        ESP_LOGW(TAG, "No OLED response at 0x%02X (%s)", candidates[i], esp_err_to_name(probe));
    }
    if (found == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dev_cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = found, .scl_speed_hz = 400000};
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &oled), TAG, "I2C device");
    if (cfg->reset_gpio >= 0) {
        gpio_config_t reset_cfg = {.pin_bit_mask = 1ULL << cfg->reset_gpio, .mode = GPIO_MODE_OUTPUT};
        ESP_RETURN_ON_ERROR(gpio_config(&reset_cfg), TAG, "reset GPIO");
        ESP_RETURN_ON_ERROR(gpio_set_level(cfg->reset_gpio, 0), TAG, "reset low");
        vTaskDelay(pdMS_TO_TICKS(50));
        ESP_RETURN_ON_ERROR(gpio_set_level(cfg->reset_gpio, 1), TAG, "reset high");
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    const uint8_t init[] = {0x00,0xae,0xd5,0x80,0xa8,0x3f,0xd3,0x00,0x40,0x8d,0x14,0x20,0x02,0xa1,0xc8,0xda,0x12,0x81,0x7f,0xd9,0xf1,0xdb,0x40,0xa4,0xa6,0xaf};
    ESP_RETURN_ON_ERROR(command(init, sizeof(init)), TAG, "init");
    return ssd1309_clear();
}

esp_err_t ssd1309_clear(void) {
    uint8_t buf[129] = {0x40};
    for (unsigned page = 0; page < 8; ++page) {
        uint8_t pos[] = {0x00, (uint8_t)(0xb0 | page), 0x00, 0x10};
        ESP_RETURN_ON_ERROR(command(pos, sizeof(pos)), TAG, "position");
        ESP_RETURN_ON_ERROR(command(buf, sizeof(buf)), TAG, "clear");
    }
    return ESP_OK;
}

esp_err_t ssd1309_write_line(unsigned line, const char *text) {
    if (line >= 8) return ESP_ERR_INVALID_ARG;
    uint8_t out[129] = {0x40}; size_t n = 1;
    while (*text && n <= 122) {
        unsigned char c = *text++; if (c >= 'a' && c <= 'z') c -= 32;
        const uint8_t *glyph = (c < sizeof(font)/sizeof(font[0]) && font[c][0]) ? font[c] : font['?'];
        memcpy(&out[n], glyph, 5); n += 5; out[n++] = 0;
    }
    uint8_t pos[] = {0x00, (uint8_t)(0xb0 | line), 0x00, 0x10};
    ESP_RETURN_ON_ERROR(command(pos, sizeof(pos)), TAG, "position");
    return command(out, sizeof(out));
}
