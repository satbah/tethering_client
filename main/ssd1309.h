#pragma once

#include "esp_err.h"

typedef struct {
    int i2c_port;
    int sda_gpio;
    int scl_gpio;
    int reset_gpio;
    uint8_t address;
} ssd1309_config_t;

esp_err_t ssd1309_init(const ssd1309_config_t *config);
esp_err_t ssd1309_clear(void);
esp_err_t ssd1309_write_line(unsigned line, const char *text);
