#include "display_ui.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_random.h"
#include "esp_lcd_nv3041a.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "lvgl.h"

static const char *TAG = "ui";

#if CONFIG_TETHER_BOARD_PROFILE_JC4827W543
static const bool s_board_has_lcd = true;
#else
static const bool s_board_has_lcd = false;
#endif

#define UI_LOG_LINES 96
#define UI_LOG_LINE_LEN 80
#define UI_TEXT_MAX (UI_LOG_LINES * (UI_LOG_LINE_LEN + 1) + 1)
#define UI_SCREEN_W 480
#define UI_SCREEN_H 272
#define UI_DIRECT_TEST_HOLD_MS 2000
#define UI_TOUCH_MARKER_SIZE 10
static int s_lcd_raw_x_offset = 0;
static int s_lcd_raw_y_offset = 0;
static uint8_t s_lcd_madctl = 0xC0;

typedef struct {
    uint32_t magic;
    uint16_t head;
    uint16_t count;
    char lines[UI_LOG_LINES][UI_LOG_LINE_LEN];
} ui_log_ring_t;

static const uint32_t UI_LOG_MAGIC = 0x55494C47; /* UILG */
RTC_DATA_ATTR static ui_log_ring_t s_ring;

static bool s_enabled;
static lv_disp_t *s_disp;
static lv_indev_t *s_touch;
static lv_obj_t *s_label;
static lv_obj_t *s_log_title_label;
static lv_obj_t *s_log_status_label;
static lv_obj_t *s_scroll;
static lv_obj_t *s_log_screen;
static lv_obj_t *s_wifi_screen;
static lv_obj_t *s_wifi_list;
static lv_obj_t *s_wifi_ssid_label;
static lv_obj_t *s_wifi_password_ta;
static lv_obj_t *s_wifi_keyboard;
static lv_obj_t *s_wifi_status_label;
static lv_obj_t *s_select_btn;
static bool s_select_requested;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_touch_handle_t s_touch_handle;
static i2c_master_bus_handle_t s_i2c_bus;
static spi_device_handle_t s_lcd_spi;
static SemaphoreHandle_t s_wifi_done_sem;

static display_ui_wifi_credentials_t s_wifi_result;
static bool s_wifi_result_cancelled;
static display_ui_wifi_ap_t s_wifi_ap_list[12];
static size_t s_wifi_ap_count;
static int s_wifi_selected_index = -1;
static char s_log_connected_ssid[33];
static bool s_have_usd_jpy_rate;
static float s_usd_jpy_rate;
static lv_obj_t *s_ap_qr_panel;
static lv_obj_t *s_ap_qr_canvas;
static uint8_t *s_ap_qr_buf;

#define AP_QR_MODULES 29
#define AP_QR_SCALE 6
#define AP_QR_SIZE (AP_QR_MODULES * AP_QR_SCALE)

static const char *s_ap_url_qr_rows[AP_QR_MODULES] = {
    "00000000000000000000000000000",
    "00000000000000000000000000000",
    "00111111100000010010111111100",
    "00100000101111111000100000100",
    "00101110100110111000101110100",
    "00101110101001111110101110100",
    "00101110101010111000101110100",
    "00100000100101111010100000100",
    "00111111101010101010111111100",
    "00000000001111110010000000000",
    "00010111101111010101101101000",
    "00011100001101001111001111000",
    "00100111101111101101111100100",
    "00011110000010100101001111100",
    "00011010111111011011100000100",
    "00101111010000101100011001000",
    "00110010100100000111000111100",
    "00101011011111110010001010100",
    "00101000110100010011111011000",
    "00000000001101100110001001000",
    "00111111100001110010101100100",
    "00100000101011010010001001000",
    "00101110101011001111111100100",
    "00101110101100101000110101100",
    "00101110100111110000001011100",
    "00100000101011011101111011100",
    "00111111100111111110100100100",
    "00000000000000000000000000000",
    "00000000000000000000000000000",
};

static void ui_draw_ap_qr_locked(void)
{
    if (!s_log_screen) {
        return;
    }

    if (!s_ap_qr_panel) {
        s_ap_qr_panel = lv_obj_create(s_log_screen);
        lv_obj_set_size(s_ap_qr_panel, 300, 252);
        lv_obj_align(s_ap_qr_panel, LV_ALIGN_CENTER, 0, 14);
        lv_obj_set_style_bg_color(s_ap_qr_panel, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(s_ap_qr_panel, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_ap_qr_panel, lv_color_hex(0x20343A), 0);
        lv_obj_set_style_border_width(s_ap_qr_panel, 2, 0);
        lv_obj_set_style_radius(s_ap_qr_panel, 6, 0);
        lv_obj_clear_flag(s_ap_qr_panel, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *guide = lv_label_create(s_ap_qr_panel);
        lv_label_set_text(guide, "Open your phone's Wi-Fi settings\nand connect to TickerPoc.");
        lv_obj_set_style_text_color(guide, lv_color_hex(0x101010), 0);
        lv_obj_set_style_text_font(guide, &lv_font_montserrat_14, 0);
        lv_obj_set_width(guide, 280);
        lv_obj_set_style_text_align(guide, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(guide, LV_ALIGN_TOP_MID, 0, 8);

        s_ap_qr_canvas = lv_canvas_create(s_ap_qr_panel);
        lv_obj_set_size(s_ap_qr_canvas, AP_QR_SIZE, AP_QR_SIZE);
        lv_obj_align(s_ap_qr_canvas, LV_ALIGN_TOP_MID, 0, 46);

        uint32_t buf_size = LV_CANVAS_BUF_SIZE_INDEXED_1BIT(AP_QR_SIZE, AP_QR_SIZE);
        s_ap_qr_buf = lv_mem_alloc(buf_size);
        if (!s_ap_qr_buf) {
            return;
        }
        memset(s_ap_qr_buf, 0xFF, buf_size);
        lv_canvas_set_buffer(s_ap_qr_canvas, s_ap_qr_buf, AP_QR_SIZE, AP_QR_SIZE, LV_IMG_CF_INDEXED_1BIT);
        lv_canvas_set_palette(s_ap_qr_canvas, 0, lv_color_hex(0x000000));
        lv_canvas_set_palette(s_ap_qr_canvas, 1, lv_color_hex(0xFFFFFF));

        lv_obj_t *caption = lv_label_create(s_ap_qr_panel);
        lv_label_set_text(caption, "Scan for setup: http://192.168.4.1/");
        lv_obj_set_style_text_color(caption, lv_color_hex(0x101010), 0);
        lv_obj_set_style_text_font(caption, &lv_font_montserrat_14, 0);
        lv_obj_set_width(caption, 280);
        lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, -8);
    }

    if (!s_ap_qr_canvas) {
        return;
    }

    lv_canvas_fill_bg(s_ap_qr_canvas, lv_color_hex(0xFFFFFF), LV_OPA_COVER);
    for (int y = 0; y < AP_QR_MODULES; ++y) {
        const char *row = s_ap_url_qr_rows[y];
        for (int x = 0; x < AP_QR_MODULES; ++x) {
            if (row[x] != '1') {
                continue;
            }
            for (int dy = 0; dy < AP_QR_SCALE; ++dy) {
                for (int dx = 0; dx < AP_QR_SCALE; ++dx) {
                    lv_canvas_set_px_color(s_ap_qr_canvas,
                                           (x * AP_QR_SCALE) + dx,
                                           (y * AP_QR_SCALE) + dy,
                                           lv_color_hex(0x000000));
                }
            }
        }
    }
}

static void ui_update_log_title_locked(void)
{
    if (!s_log_title_label) {
        return;
    }

    char title[96];
    const char *ap_ssid = (s_log_connected_ssid[0] != '\0') ? s_log_connected_ssid : "-";
    if (s_have_usd_jpy_rate) {
        snprintf(title, sizeof(title), "USD/JPY:%.2f [AP: %s]", (double)s_usd_jpy_rate, ap_ssid);
    } else {
        snprintf(title, sizeof(title), "USD/JPY:--.-- [AP: %s]", ap_ssid);
    }
    lv_label_set_text(s_log_title_label, title);
}

static void ui_gt911_reset_sequence(void)
{
#if CONFIG_TETHER_BOARD_PROFILE_JC4827W543
    if (CONFIG_TETHER_JC_TOUCH_RST_GPIO >= 0) {
        const gpio_num_t rst = CONFIG_TETHER_JC_TOUCH_RST_GPIO;
        gpio_reset_pin(rst);
        gpio_set_direction(rst, GPIO_MODE_OUTPUT);
        gpio_set_level(rst, 0);
    }
    if (CONFIG_TETHER_JC_TOUCH_INT_GPIO >= 0) {
        const gpio_num_t int_gpio = CONFIG_TETHER_JC_TOUCH_INT_GPIO;
        gpio_reset_pin(int_gpio);
        gpio_set_direction(int_gpio, GPIO_MODE_OUTPUT);
        gpio_set_level(int_gpio, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    if (CONFIG_TETHER_JC_TOUCH_INT_GPIO >= 0) {
        gpio_set_level(CONFIG_TETHER_JC_TOUCH_INT_GPIO, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(1));
    if (CONFIG_TETHER_JC_TOUCH_RST_GPIO >= 0) {
        gpio_set_level(CONFIG_TETHER_JC_TOUCH_RST_GPIO, 1);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    vTaskDelay(pdMS_TO_TICKS(50));
#endif
}

#if CONFIG_TETHER_BOARD_PROFILE_JC4827W543
static const nv3041a_lcd_init_cmd_t s_jc4827_nv3041_init_cmds[] = {
    {0xFF, (uint8_t[]){0xA5}, 1, 0},
    {0x36, (uint8_t[]){0xC0}, 1, 0},
    {0x3A, (uint8_t[]){0x01}, 1, 0},
    {0x41, (uint8_t[]){0x03}, 1, 0},
    {0x44, (uint8_t[]){0x15}, 1, 0},
    {0x45, (uint8_t[]){0x15}, 1, 0},
    {0x7D, (uint8_t[]){0x03}, 1, 0},
    {0xC1, (uint8_t[]){0xBB}, 1, 0},
    {0xC2, (uint8_t[]){0x05}, 1, 0},
    {0xC3, (uint8_t[]){0x10}, 1, 0},
    {0xC6, (uint8_t[]){0x3E}, 1, 0},
    {0xC7, (uint8_t[]){0x25}, 1, 0},
    {0xC8, (uint8_t[]){0x11}, 1, 0},
    {0x7A, (uint8_t[]){0x5F}, 1, 0},
    {0x67, (uint8_t[]){0x21}, 1, 0},
    {0x6F, (uint8_t[]){0x44}, 1, 0},
    {0x78, (uint8_t[]){0x70}, 1, 0},
    {0xC9, (uint8_t[]){0x00}, 1, 0},
    {0x51, (uint8_t[]){0x0A}, 1, 0},
    {0x52, (uint8_t[]){0x76}, 1, 0},
    {0x53, (uint8_t[]){0x0A}, 1, 0},
    {0x54, (uint8_t[]){0x76}, 1, 0},
    {0x46, (uint8_t[]){0x0A}, 1, 0},
    {0x47, (uint8_t[]){0x2A}, 1, 0},
    {0x48, (uint8_t[]){0x0A}, 1, 0},
    {0x49, (uint8_t[]){0x1A}, 1, 0},
    {0x56, (uint8_t[]){0x43}, 1, 0},
    {0x57, (uint8_t[]){0x42}, 1, 0},
    {0x58, (uint8_t[]){0x3C}, 1, 0},
    {0x59, (uint8_t[]){0x64}, 1, 0},
    {0x5A, (uint8_t[]){0x41}, 1, 0},
    {0x5B, (uint8_t[]){0x3C}, 1, 0},
    {0x5C, (uint8_t[]){0x02}, 1, 0},
    {0x5D, (uint8_t[]){0x3C}, 1, 0},
    {0x5E, (uint8_t[]){0x1F}, 1, 0},
    {0x60, (uint8_t[]){0x80}, 1, 0},
    {0x61, (uint8_t[]){0x3F}, 1, 0},
    {0x62, (uint8_t[]){0x21}, 1, 0},
    {0x63, (uint8_t[]){0x07}, 1, 0},
    {0x64, (uint8_t[]){0xE0}, 1, 0},
    {0x65, (uint8_t[]){0x02}, 1, 0},
    {0xCA, (uint8_t[]){0x20}, 1, 0},
    {0xCB, (uint8_t[]){0x52}, 1, 0},
    {0xCC, (uint8_t[]){0x10}, 1, 0},
    {0xCD, (uint8_t[]){0x42}, 1, 0},
    {0xD0, (uint8_t[]){0x20}, 1, 0},
    {0xD1, (uint8_t[]){0x52}, 1, 0},
    {0xD2, (uint8_t[]){0x10}, 1, 0},
    {0xD3, (uint8_t[]){0x42}, 1, 0},
    {0xD4, (uint8_t[]){0x0A}, 1, 0},
    {0xD5, (uint8_t[]){0x32}, 1, 0},
    {0x80, (uint8_t[]){0x00, 0x07, 0x02, 0x37, 0x35, 0x3F, 0x11, 0x27, 0x0B, 0x14, 0x1A, 0x0A, 0x14, 0x17, 0x16, 0x1B, 0x04, 0x0A, 0x16}, 19, 0},
    {0xA0, (uint8_t[]){0x00, 0x06, 0x01, 0x37, 0x35, 0x3F, 0x10, 0x27, 0x0B, 0x14, 0x1A, 0x0A, 0x08, 0x07, 0x06, 0x07, 0x04, 0x0A, 0x15}, 19, 0},
    {0x81, (uint8_t[]){0x07}, 1, 0},
    {0xA1, (uint8_t[]){0x06}, 1, 0},
    {0x82, (uint8_t[]){0x02}, 1, 0},
    {0xA2, (uint8_t[]){0x01}, 1, 0},
    {0x86, (uint8_t[]){0x11}, 1, 0},
    {0xA6, (uint8_t[]){0x10}, 1, 0},
    {0x87, (uint8_t[]){0x27}, 1, 0},
    {0xA7, (uint8_t[]){0x27}, 1, 0},
    {0x83, (uint8_t[]){0x37}, 1, 0},
    {0xA3, (uint8_t[]){0x37}, 1, 0},
    {0x84, (uint8_t[]){0x35}, 1, 0},
    {0xA4, (uint8_t[]){0x35}, 1, 0},
    {0x85, (uint8_t[]){0x3F}, 1, 0},
    {0xA5, (uint8_t[]){0x3F}, 1, 0},
    {0x88, (uint8_t[]){0x0B}, 1, 0},
    {0xA8, (uint8_t[]){0x0B}, 1, 0},
    {0x89, (uint8_t[]){0x14}, 1, 0},
    {0xA9, (uint8_t[]){0x14}, 1, 0},
    {0x8A, (uint8_t[]){0x1A}, 1, 0},
    {0xAA, (uint8_t[]){0x1A}, 1, 0},
    {0x8B, (uint8_t[]){0x0A}, 1, 0},
    {0xAB, (uint8_t[]){0x0A}, 1, 0},
    {0x8C, (uint8_t[]){0x14}, 1, 0},
    {0xAC, (uint8_t[]){0x08}, 1, 0},
    {0x8D, (uint8_t[]){0x17}, 1, 0},
    {0xAD, (uint8_t[]){0x07}, 1, 0},
    {0x8E, (uint8_t[]){0x16}, 1, 0},
    {0xAE, (uint8_t[]){0x06}, 1, 0},
    {0x8F, (uint8_t[]){0x1B}, 1, 0},
    {0xAF, (uint8_t[]){0x07}, 1, 0},
    {0x90, (uint8_t[]){0x04}, 1, 0},
    {0xB0, (uint8_t[]){0x04}, 1, 0},
    {0x91, (uint8_t[]){0x0A}, 1, 0},
    {0xB1, (uint8_t[]){0x0A}, 1, 0},
    {0x92, (uint8_t[]){0x16}, 1, 0},
    {0xB2, (uint8_t[]){0x15}, 1, 0},
    {0xFF, (uint8_t[]){0x00}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 100},
};

static const nv3041a_vendor_config_t s_jc4827_nv3041_vendor_cfg __attribute__((unused)) = {
    .init_cmds = s_jc4827_nv3041_init_cmds,
    .init_cmds_size = sizeof(s_jc4827_nv3041_init_cmds) / sizeof(s_jc4827_nv3041_init_cmds[0]),
    .flags = {
        .use_qspi_interface = 1,
    },
};

#endif

static void ui_log_nv3041_init_sequence(void)
{
#if CONFIG_TETHER_BOARD_PROFILE_JC4827W543
    ESP_LOGI(TAG, "LCD NV3041 init sequence (%u cmds)", (unsigned) (sizeof(s_jc4827_nv3041_init_cmds) / sizeof(s_jc4827_nv3041_init_cmds[0])));
    for (size_t i = 0; i < sizeof(s_jc4827_nv3041_init_cmds) / sizeof(s_jc4827_nv3041_init_cmds[0]); ++i) {
        const nv3041a_lcd_init_cmd_t *cmd = &s_jc4827_nv3041_init_cmds[i];
        char data_buf[48];
        size_t pos = 0;
        for (size_t j = 0; j < cmd->data_bytes && pos + 4 < sizeof(data_buf); ++j) {
            pos += snprintf(&data_buf[pos], sizeof(data_buf) - pos, "%s%02X", (j == 0) ? "" : " ", ((const uint8_t *)cmd->data)[j]);
        }
        data_buf[pos] = '\0';
        ESP_LOGI(TAG, "  cmd[%02u] 0x%02X data=[%s] delay=%u ms",
                 (unsigned)i, cmd->cmd, data_buf, cmd->delay_ms);
    }
#endif
}

#if CONFIG_TETHER_BOARD_PROFILE_JC4827W543
static esp_err_t ui_lcd_qspi_tx(uint8_t prefix, uint16_t addr_word, const void *data, size_t data_len, bool quad_data)
{
    if (!s_lcd_spi) {
        return ESP_FAIL;
    }

    spi_transaction_ext_t t = {0};
    t.base.flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR;
    if (quad_data) {
        t.base.flags |= SPI_TRANS_MODE_QIO;
    }
    t.base.cmd = prefix;
    t.base.addr = ((uint32_t)addr_word) << 8;
    t.base.length = data_len * 8;
    t.base.tx_buffer = data;
    t.command_bits = 8;
    t.address_bits = 24;
    return spi_device_polling_transmit(s_lcd_spi, &t.base);
}

static esp_err_t ui_lcd_write_reg(uint8_t reg, const void *data, size_t data_len)
{
    return ui_lcd_qspi_tx(0x02, reg, data, data_len, true);
}

static esp_err_t ui_lcd_write_color(const void *data, size_t data_len)
{
    return ui_lcd_qspi_tx(0x32, 0x2C, data, data_len, true);
}

static esp_err_t ui_lcd_set_window(int x0, int y0, int x1, int y1)
{
    x0 += s_lcd_raw_x_offset;
    x1 += s_lcd_raw_x_offset;
    y0 += s_lcd_raw_y_offset;
    y1 += s_lcd_raw_y_offset;
    uint8_t col[4] = {
        (uint8_t)((x0 >> 8) & 0xFF), (uint8_t)(x0 & 0xFF),
        (uint8_t)((x1 >> 8) & 0xFF), (uint8_t)(x1 & 0xFF),
    };
    uint8_t row[4] = {
        (uint8_t)((y0 >> 8) & 0xFF), (uint8_t)(y0 & 0xFF),
        (uint8_t)((y1 >> 8) & 0xFF), (uint8_t)(y1 & 0xFF),
    };
    ESP_RETURN_ON_ERROR(ui_lcd_write_reg(0x2A, col, sizeof(col)), TAG, "set column failed");
    ESP_RETURN_ON_ERROR(ui_lcd_write_reg(0x2B, row, sizeof(row)), TAG, "set row failed");
    return ESP_OK;
}

static esp_err_t ui_lcd_set_madctl(uint8_t value)
{
    s_lcd_madctl = value;
    return ui_lcd_write_reg(0x36, &s_lcd_madctl, 1);
}
#endif

static void __attribute__((unused)) ui_dump_panel_registers(void)
{
    if (!s_panel_io) {
        return;
    }

    uint8_t buf[4] = {0};
    esp_err_t ret;

#define DUMP_REG(cmd, size) do { \
        memset(buf, 0, sizeof(buf)); \
        ret = esp_lcd_panel_io_rx_param(s_panel_io, (cmd), buf, (size)); \
        if (ret == ESP_OK) { \
            ESP_LOGI(TAG, "LCD reg 0x%02X = %02X %02X %02X %02X", (cmd), buf[0], buf[1], buf[2], buf[3]); \
        } else { \
            ESP_LOGW(TAG, "LCD reg 0x%02X read failed: %s", (cmd), esp_err_to_name(ret)); \
        } \
    } while (0)

    DUMP_REG(0x04, 4); /* ID */
    DUMP_REG(0x09, 4); /* status */
    DUMP_REG(0x0A, 1); /* power mode */
    DUMP_REG(0x0B, 1); /* MADCTL? board-specific, sometimes works */
    DUMP_REG(0x0C, 1); /* pixel format */
    DUMP_REG(0x35, 1); /* TE */
    DUMP_REG(0x36, 1); /* MADCTL */
    DUMP_REG(0x3A, 1); /* COLMOD */
    DUMP_REG(0x40, 1);
    DUMP_REG(0x41, 1);
    DUMP_REG(0x44, 1);
    DUMP_REG(0x45, 1);

#undef DUMP_REG
}

static void __attribute__((unused)) ui_panel_show_direct_test(void)
{
    if (!s_panel) {
        ESP_LOGW(TAG, "LCD direct test skipped: panel handle missing");
        return;
    }

    static uint16_t block[20][UI_SCREEN_W];
    const uint16_t colors[] = { 0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F };

    ESP_LOGI(TAG, "LCD direct test pattern");
    for (size_t c = 0; c < sizeof(colors) / sizeof(colors[0]); ++c) {
        const uint16_t color = colors[c];
        for (int y = 0; y < 20; ++y) {
            for (int i = 0; i < UI_SCREEN_W; ++i) {
                block[y][i] = color;
            }
        }
        for (int y = 0; y < UI_SCREEN_H; y += 20) {
            const int y_end = (y + 20 < UI_SCREEN_H) ? (y + 20) : UI_SCREEN_H;
            if (esp_lcd_panel_draw_bitmap(s_panel, 0, y, UI_SCREEN_W, y_end, block) != ESP_OK) {
                ESP_LOGW(TAG, "LCD direct test draw failed at y=%d", y);
                return;
            }
        }
        ESP_LOGI(TAG, "LCD direct test color[%u]=0x%04X hold=%d ms",
                 (unsigned)c, colors[c], UI_DIRECT_TEST_HOLD_MS);
        vTaskDelay(pdMS_TO_TICKS(UI_DIRECT_TEST_HOLD_MS));
    }
}

static esp_err_t ui_touch_read(uint16_t *x, uint16_t *y)
{
    if (!s_touch_handle) {
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_touch_read_data(s_touch_handle), TAG, "touch read failed");

    esp_lcd_touch_point_data_t points[1] = {0};
    uint8_t point_num = 0;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_get_data(s_touch_handle, points, &point_num, 1), TAG, "touch data parse failed");
    if (point_num == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (x) {
        *x = points[0].x;
    }
    if (y) {
        *y = points[0].y;
    }
    return ESP_OK;
}

static void ui_touch_marker(int x, int y, uint16_t color)
{
    if (!s_panel) {
        return;
    }

    static uint16_t block[UI_TOUCH_MARKER_SIZE][UI_TOUCH_MARKER_SIZE];
    for (int row = 0; row < UI_TOUCH_MARKER_SIZE; ++row) {
        for (int col = 0; col < UI_TOUCH_MARKER_SIZE; ++col) {
            block[row][col] = color;
        }
    }

    const int half = UI_TOUCH_MARKER_SIZE / 2;
    const int x0 = (x > half) ? (x - half) : 0;
    const int y0 = (y > half) ? (y - half) : 0;
    const int x1 = (x0 + UI_TOUCH_MARKER_SIZE < UI_SCREEN_W) ? (x0 + UI_TOUCH_MARKER_SIZE) : UI_SCREEN_W;
    const int y1 = (y0 + UI_TOUCH_MARKER_SIZE < UI_SCREEN_H) ? (y0 + UI_TOUCH_MARKER_SIZE) : UI_SCREEN_H;
    if (x1 > x0 && y1 > y0) {
        esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, block);
    }
}

static uint16_t ui_random_rgb565(void)
{
    return 0xFFFF;
}

bool display_ui_poll_touch_random_dot(void)
{
    static bool s_touch_logged_once;
    if (!s_touch_handle) {
        if (!s_touch_logged_once) {
            ESP_LOGW(TAG, "GT911 touch not ready");
            s_touch_logged_once = true;
        }
        return false;
    }

    uint16_t x = 0, y = 0;
    if (ui_touch_read(&x, &y) != ESP_OK) {
        return false;
    }

    const uint16_t color = ui_random_rgb565();
    ESP_LOGI(TAG, "GT911 touch event x=%u y=%u color=0x%04X", (unsigned)x, (unsigned)y, color);
    ui_touch_marker((int)x, (int)y, color);
    return true;
}

static void ui_panel_fill_solid(uint16_t color_be)
{
#if CONFIG_TETHER_BOARD_PROFILE_JC4827W543
    if (!s_panel) {
        return;
    }

    static uint16_t block[20][UI_SCREEN_W];
    for (int y = 0; y < 20; ++y) {
        for (int i = 0; i < UI_SCREEN_W; ++i) {
            block[y][i] = color_be;
        }
    }
    for (int y = 0; y < UI_SCREEN_H; y += 20) {
        const int y_end = (y + 20 < UI_SCREEN_H) ? (y + 20) : UI_SCREEN_H;
        if (esp_lcd_panel_draw_bitmap(s_panel, 0, y, UI_SCREEN_W, y_end, block) != ESP_OK) {
            ESP_LOGW(TAG, "LCD solid fill draw failed at y=%d", y);
            return;
        }
    }
#else
    if (!s_panel) {
        return;
    }

    static uint16_t block[20][UI_SCREEN_W];
    for (int y = 0; y < 20; ++y) {
        for (int i = 0; i < UI_SCREEN_W; ++i) {
            block[y][i] = color_be;
        }
    }
    for (int y = 0; y < UI_SCREEN_H; y += 20) {
        const int y_end = (y + 20 < UI_SCREEN_H) ? (y + 20) : UI_SCREEN_H;
        if (esp_lcd_panel_draw_bitmap(s_panel, 0, y, UI_SCREEN_W, y_end, block) != ESP_OK) {
            ESP_LOGW(TAG, "LCD solid fill draw failed at y=%d", y);
            return;
        }
    }
#endif
}

#if CONFIG_TETHER_BOARD_PROFILE_JC4827W543
static void __attribute__((unused)) ui_panel_origin_probe(void)
{
    ESP_LOGI(TAG, "LCD origin probe skipped in panel-driver mode");
}

static void __attribute__((unused)) ui_panel_calibration_sweep(void)
{
    static const struct {
        uint8_t madctl;
        int x_off;
        int y_off;
    } cases[] = {
        { 0x00, 0, 0 },
        { 0x00, 0, 1 },
        { 0x00, 0, 2 },
        { 0x00, 0, 4 },
    };
    static uint16_t block[32][UI_SCREEN_W];
    const uint16_t colors[] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };

    ESP_LOGI(TAG, "LCD calibration sweep");
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        s_lcd_raw_x_offset = cases[c].x_off;
        s_lcd_raw_y_offset = cases[c].y_off;
        if (ui_lcd_set_madctl(cases[c].madctl) != ESP_OK) {
            ESP_LOGW(TAG, "MADCTL set failed at case %u", (unsigned)c);
            return;
        }
        ESP_LOGI(TAG, "LCD cal case[%u] madctl=0x%02X x_off=%d y_off=%d",
                 (unsigned)c, cases[c].madctl, s_lcd_raw_x_offset, s_lcd_raw_y_offset);
        for (int y = 0; y < 32; ++y) {
            for (int x = 0; x < UI_SCREEN_W; ++x) {
                const size_t idx = (x < UI_SCREEN_W / 4) ? 0 :
                                   (x < UI_SCREEN_W / 2) ? 1 :
                                   (x < (UI_SCREEN_W * 3) / 4) ? 2 : 3;
                block[y][x] = __builtin_bswap16(colors[idx]);
            }
        }
        if (ui_lcd_set_window(0, 0, UI_SCREEN_W - 1, 31) != ESP_OK ||
            ui_lcd_write_color(block, sizeof(block)) != ESP_OK) {
            ESP_LOGW(TAG, "LCD cal draw failed at case %u", (unsigned)c);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1300));
    }
}
#endif

static void ui_ring_init(void)
{
    if (s_ring.magic != UI_LOG_MAGIC) {
        memset(&s_ring, 0, sizeof(s_ring));
        s_ring.magic = UI_LOG_MAGIC;
    }
}

static void ui_ring_push(const char *line)
{
    ui_ring_init();
    const size_t len = strnlen(line, UI_LOG_LINE_LEN - 1);
    char *slot = s_ring.lines[s_ring.head];
    memset(slot, 0, UI_LOG_LINE_LEN);
    memcpy(slot, line, len);
    s_ring.head = (s_ring.head + 1) % UI_LOG_LINES;
    if (s_ring.count < UI_LOG_LINES) {
        s_ring.count++;
    }
}

static void ui_ring_replace_last(const char *line)
{
    ui_ring_init();
    if (s_ring.count == 0) {
        ui_ring_push(line);
        return;
    }

    const size_t len = strnlen(line, UI_LOG_LINE_LEN - 1);
    const uint16_t idx = (s_ring.head + UI_LOG_LINES - 1) % UI_LOG_LINES;
    char *slot = s_ring.lines[idx];
    memset(slot, 0, UI_LOG_LINE_LEN);
    memcpy(slot, line, len);
}

static void ui_ring_build_text(char *buf, size_t buf_len)
{
    ui_ring_init();
    buf[0] = '\0';
    size_t pos = 0;
    for (uint16_t i = 0; i < s_ring.count; ++i) {
        const uint16_t idx = (s_ring.head + UI_LOG_LINES - s_ring.count + i) % UI_LOG_LINES;
        const char *line = s_ring.lines[idx];
        const size_t line_len = strnlen(line, UI_LOG_LINE_LEN);
        if (line_len == 0) {
            continue;
        }
        if (pos + line_len + 2 >= buf_len) {
            break;
        }
        memcpy(&buf[pos], line, line_len);
        pos += line_len;
        buf[pos++] = '\n';
        buf[pos] = '\0';
    }
}

static void ui_scroll_log_to_bottom_locked(void)
{
    if (!s_scroll) {
        return;
    }
    if (s_log_screen) {
        lv_obj_update_layout(s_log_screen);
    }
    lv_obj_update_layout(s_scroll);
    lv_obj_scroll_to_y(s_scroll, LV_COORD_MAX, LV_ANIM_OFF);
}

static void ui_refresh_label_locked(void)
{
    if (!s_label) {
        return;
    }
    static char text[UI_TEXT_MAX];
    ui_ring_build_text(text, sizeof(text));
    lv_label_set_text(s_label, text);
    ui_scroll_log_to_bottom_locked();
}

static void ui_select_btn_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_CLICKED) {
        s_select_requested = true;
        if (s_wifi_status_label) {
            lv_label_set_text(s_wifi_status_label, "scanning ...");
        }
        ESP_LOGI(TAG, "log screen select requested");
    }
}

static void ui_build_log_screen(void)
{
    s_select_requested = false;
    s_log_screen = lv_scr_act();
    lv_obj_set_style_bg_color(s_log_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_log_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_log_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_log_title_label = lv_label_create(s_log_screen);
    lv_obj_set_style_text_color(s_log_title_label, lv_color_hex(0x9FFFE3), 0);
    lv_obj_set_style_text_font(s_log_title_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_log_title_label, LV_ALIGN_TOP_LEFT, 8, 6);
    ui_update_log_title_locked();

    s_log_status_label = lv_label_create(s_log_screen);
    lv_label_set_text(s_log_status_label, "ready");
    lv_obj_set_style_text_color(s_log_status_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_log_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_log_status_label, UI_SCREEN_W - 124);
    lv_obj_align(s_log_status_label, LV_ALIGN_TOP_LEFT, 8, 26);

    s_scroll = lv_obj_create(s_log_screen);
    lv_obj_set_size(s_scroll, UI_SCREEN_W - 16, UI_SCREEN_H - 56);
    lv_obj_align(s_scroll, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_scroll, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_style_bg_color(s_scroll, lv_color_hex(0x030303), 0);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scroll, 1, 0);
    lv_obj_set_style_border_color(s_scroll, lv_color_hex(0x4FA7B3), 0);
    lv_obj_set_style_pad_all(s_scroll, 6, 0);

    s_label = lv_label_create(s_scroll);
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_label, UI_SCREEN_W - 28);
    lv_obj_align(s_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_color(s_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_opa(s_label, LV_OPA_COVER, 0);
    ui_refresh_label_locked();
    ui_scroll_log_to_bottom_locked();

    s_select_btn = lv_btn_create(s_log_screen);
    lv_obj_set_size(s_select_btn, 92, 30);
    lv_obj_align(s_select_btn, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_bg_color(s_select_btn, lv_color_hex(0xA8FFF0), 0);
    lv_obj_set_style_bg_opa(s_select_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_select_btn, 1, 0);
    lv_obj_set_style_border_color(s_select_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_event_cb(s_select_btn, ui_select_btn_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *btn_label = lv_label_create(s_select_btn);
    lv_label_set_text(btn_label, "Select");
    lv_obj_set_style_text_color(btn_label, lv_color_hex(0x000000), 0);
    lv_obj_center(btn_label);
}

static void ui_wifi_prompt_finish(bool cancelled)
{
    s_wifi_result_cancelled = cancelled;
    if (s_wifi_done_sem) {
        xSemaphoreGive(s_wifi_done_sem);
    }
}

static void ui_wifi_connect_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    if (s_wifi_selected_index >= 0 && s_wifi_selected_index < (int)s_wifi_ap_count) {
        strlcpy(s_wifi_result.ssid, s_wifi_ap_list[s_wifi_selected_index].ssid, sizeof(s_wifi_result.ssid));
    }
    if (s_wifi_result.ssid[0] == '\0') {
        if (s_wifi_status_label) {
            lv_label_set_text(s_wifi_status_label, "select AP first");
        }
        return;
    }
    if (s_wifi_password_ta) {
        strlcpy(s_wifi_result.password, lv_textarea_get_text(s_wifi_password_ta), sizeof(s_wifi_result.password));
    }
    if (s_wifi_password_ta && s_wifi_result.password[0] == '\0') {
        if (s_wifi_status_label) {
            lv_label_set_text(s_wifi_status_label, "enter password");
        }
        return;
    }
    if (s_wifi_status_label) {
        lv_label_set_text(s_wifi_status_label, "connecting ...");
    }
    ui_wifi_prompt_finish(false);
}

static void ui_wifi_cancel_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_wifi_status_label) {
        lv_label_set_text(s_wifi_status_label, "cancelled");
    }
    ui_wifi_prompt_finish(true);
}

static void ui_wifi_password_event_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        if (s_wifi_selected_index >= 0 && s_wifi_selected_index < (int)s_wifi_ap_count) {
            strlcpy(s_wifi_result.ssid, s_wifi_ap_list[s_wifi_selected_index].ssid, sizeof(s_wifi_result.ssid));
        }
        if (s_wifi_password_ta) {
            strlcpy(s_wifi_result.password, lv_textarea_get_text(s_wifi_password_ta), sizeof(s_wifi_result.password));
        }
        ui_wifi_prompt_finish(false);
    } else if (code == LV_EVENT_CANCEL) {
        ui_wifi_prompt_finish(true);
    }
}

static void ui_wifi_select_ap_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)s_wifi_ap_count) {
        return;
    }

    s_wifi_selected_index = idx;
    const char *ssid = s_wifi_ap_list[idx].ssid;

    if (s_wifi_ssid_label) {
        char line[64];
        snprintf(line, sizeof(line), "SSID: %s", ssid);
        lv_label_set_text(s_wifi_ssid_label, line);
    }

    if (s_wifi_status_label) {
        lv_label_set_text(s_wifi_status_label, "Enter password");
    }
    if (s_wifi_password_ta) {
        lv_obj_clear_flag(s_wifi_password_ta, LV_OBJ_FLAG_HIDDEN);
        lv_textarea_set_text(s_wifi_password_ta, "");
        lv_obj_add_state(s_wifi_password_ta, LV_STATE_FOCUSED);
        lv_obj_scroll_to_view(s_wifi_password_ta, LV_ANIM_OFF);
    }
    if (s_wifi_keyboard) {
        lv_obj_clear_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_wifi_keyboard, s_wifi_password_ta);
    }
}

static lv_obj_t *ui_wifi_create_button(lv_obj_t *parent, const display_ui_wifi_ap_t *ap, size_t index)
{
    char text[64];
    snprintf(text, sizeof(text), "%s  (%d dBm)%s", ap->ssid, (int)ap->rssi, ap->secure ? " *" : "");
    lv_obj_t *btn = lv_list_add_btn(parent, NULL, text);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x0D0D0D), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x315D66), 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_event_cb(btn, ui_wifi_select_ap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
    return btn;
}

static void ui_build_wifi_screen(void)
{
    if (s_wifi_screen) {
        lv_obj_del(s_wifi_screen);
        s_wifi_screen = NULL;
    }

    s_wifi_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_wifi_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_wifi_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_wifi_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_wifi_screen);
    lv_label_set_text(title, "Wi-Fi setup");
    lv_obj_set_style_text_color(title, lv_color_hex(0x9FFFE3), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 4);

    s_wifi_status_label = lv_label_create(s_wifi_screen);
    lv_label_set_text(s_wifi_status_label, "Select AP");
    lv_obj_set_style_text_color(s_wifi_status_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_wifi_status_label, LV_ALIGN_TOP_LEFT, 8, 28);

    s_wifi_ssid_label = lv_label_create(s_wifi_screen);
    lv_label_set_text(s_wifi_ssid_label, "SSID: -");
    lv_obj_set_style_text_color(s_wifi_ssid_label, lv_color_hex(0xDCECEC), 0);
    lv_obj_align(s_wifi_ssid_label, LV_ALIGN_TOP_LEFT, 8, 48);

    s_wifi_list = lv_list_create(s_wifi_screen);
    lv_obj_set_size(s_wifi_list, UI_SCREEN_W - 16, 120);
    lv_obj_align(s_wifi_list, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_set_scrollbar_mode(s_wifi_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(s_wifi_list, lv_color_hex(0x030303), 0);
    lv_obj_set_style_bg_opa(s_wifi_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wifi_list, 1, 0);
    lv_obj_set_style_border_color(s_wifi_list, lv_color_hex(0x4FA7B3), 0);

    for (size_t i = 0; i < s_wifi_ap_count; ++i) {
        ui_wifi_create_button(s_wifi_list, &s_wifi_ap_list[i], i);
    }

    s_wifi_password_ta = lv_textarea_create(s_wifi_screen);
    lv_obj_set_size(s_wifi_password_ta, UI_SCREEN_W - 220, 34);
    lv_obj_align(s_wifi_password_ta, LV_ALIGN_BOTTOM_LEFT, 110, -8);
    lv_textarea_set_one_line(s_wifi_password_ta, true);
    lv_textarea_set_password_mode(s_wifi_password_ta, true);
    lv_textarea_set_placeholder_text(s_wifi_password_ta, "Password");
    lv_obj_set_style_bg_color(s_wifi_password_ta, lv_color_hex(0x0D0D0D), 0);
    lv_obj_set_style_bg_opa(s_wifi_password_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wifi_password_ta, 1, 0);
    lv_obj_set_style_border_color(s_wifi_password_ta, lv_color_hex(0x4FA7B3), 0);
    lv_obj_set_style_text_color(s_wifi_password_ta, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_flag(s_wifi_password_ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_wifi_password_ta, ui_wifi_password_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_wifi_password_ta, ui_wifi_password_event_cb, LV_EVENT_CANCEL, NULL);

    s_wifi_keyboard = lv_keyboard_create(s_wifi_screen);
    lv_obj_set_size(s_wifi_keyboard, UI_SCREEN_W - 16, 120);
    lv_obj_align(s_wifi_keyboard, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_set_style_bg_color(s_wifi_keyboard, lv_color_hex(0x050505), 0);
    lv_obj_set_style_bg_opa(s_wifi_keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_wifi_keyboard, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(s_wifi_keyboard, lv_color_hex(0x161616), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_wifi_keyboard, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_wifi_keyboard, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_wifi_keyboard, lv_color_hex(0x5FBCCA), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_wifi_keyboard, lv_color_hex(0xA8FFF0), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(s_wifi_keyboard, lv_color_hex(0x000000), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_wifi_keyboard, s_wifi_password_ta);

    lv_obj_t *connect_btn = lv_btn_create(s_wifi_screen);
    lv_obj_set_size(connect_btn, 96, 34);
    lv_obj_align(connect_btn, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    lv_obj_set_style_bg_color(connect_btn, lv_color_hex(0x0B4E56), 0);
    lv_obj_set_style_bg_opa(connect_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(connect_btn, 1, 0);
    lv_obj_set_style_border_color(connect_btn, lv_color_hex(0x8FEFFF), 0);
    lv_obj_add_event_cb(connect_btn, ui_wifi_connect_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *connect_label = lv_label_create(connect_btn);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_set_style_text_color(connect_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(connect_label);

    lv_obj_t *cancel_btn = lv_btn_create(s_wifi_screen);
    lv_obj_set_size(cancel_btn, 96, 34);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cancel_btn, 1, 0);
    lv_obj_set_style_border_color(cancel_btn, lv_color_hex(0x8E8E8E), 0);
    lv_obj_add_event_cb(cancel_btn, ui_wifi_cancel_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(cancel_label);
}

static void ui_clear_wifi_screen_refs(void)
{
    s_wifi_screen = NULL;
    s_wifi_list = NULL;
    s_wifi_ssid_label = NULL;
    s_wifi_password_ta = NULL;
    s_wifi_keyboard = NULL;
    s_wifi_status_label = NULL;
}

bool display_ui_has_gui(void)
{
    return s_board_has_lcd;
}

bool display_ui_take_select_request(void)
{
    const bool requested = s_select_requested;
    s_select_requested = false;
    return requested;
}

void display_ui_set_status_text(const char *text)
{
    if (!text || !s_wifi_status_label) {
        return;
    }
    if (s_enabled && lvgl_port_lock(1000)) {
        lv_label_set_text(s_wifi_status_label, text);
        lv_timer_handler();
        lvgl_port_unlock();
    } else {
        lv_label_set_text(s_wifi_status_label, text);
    }
}

void display_ui_set_log_status_text(const char *text)
{
    if (!text || !s_log_status_label) {
        return;
    }
    if (s_enabled && lvgl_port_lock(1000)) {
        lv_label_set_text(s_log_status_label, text);
        lv_timer_handler();
        lvgl_port_unlock();
    } else {
        lv_label_set_text(s_log_status_label, text);
    }
}

void display_ui_set_log_connected_ssid(const char *ssid)
{
    if (!ssid || ssid[0] == '\0') {
        s_log_connected_ssid[0] = '\0';
    } else {
        strlcpy(s_log_connected_ssid, ssid, sizeof(s_log_connected_ssid));
    }

    if (!s_log_title_label) {
        return;
    }

    if (s_enabled && lvgl_port_lock(1000)) {
        ui_update_log_title_locked();
        lv_timer_handler();
        lvgl_port_unlock();
    } else {
        ui_update_log_title_locked();
    }
}

void display_ui_set_usd_jpy_rate(float rate, bool valid)
{
    s_have_usd_jpy_rate = valid;
    if (valid) {
        s_usd_jpy_rate = rate;
    }

    if (!s_log_title_label) {
        return;
    }

    if (s_enabled && lvgl_port_lock(1000)) {
        ui_update_log_title_locked();
        lv_timer_handler();
        lvgl_port_unlock();
    } else {
        ui_update_log_title_locked();
    }
}

void display_ui_show_log_screen(void)
{
    if (!s_board_has_lcd || !s_enabled || !s_log_screen) {
        return;
    }

    if (lvgl_port_lock(1000)) {
        lv_scr_load(s_log_screen);
        if (s_wifi_screen) {
            lv_obj_del(s_wifi_screen);
            ui_clear_wifi_screen_refs();
        }
        ui_scroll_log_to_bottom_locked();
        lv_timer_handler();
        lvgl_port_unlock();
    }

    s_select_requested = false;
}

void display_ui_show_ap_url_qr(void)
{
    if (!s_board_has_lcd || !s_enabled || !s_log_screen) {
        return;
    }

    if (!lvgl_port_lock(1000)) {
        return;
    }

    lv_scr_load(s_log_screen);
    if (s_wifi_screen) {
        lv_obj_del(s_wifi_screen);
        ui_clear_wifi_screen_refs();
    }

    if (s_scroll) {
        lv_obj_add_flag(s_scroll, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_select_btn) {
        lv_obj_add_flag(s_select_btn, LV_OBJ_FLAG_HIDDEN);
    }

    ui_draw_ap_qr_locked();
    lv_obj_move_foreground(s_ap_qr_panel);
    lv_timer_handler();
    lvgl_port_unlock();
}

esp_err_t display_ui_wifi_prompt(const display_ui_wifi_ap_t *aps, size_t ap_count, display_ui_wifi_credentials_t *out_creds)
{
    if (!s_board_has_lcd || !s_enabled) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!aps || ap_count == 0 || !out_creds) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ap_count > (sizeof(s_wifi_ap_list) / sizeof(s_wifi_ap_list[0]))) {
        ap_count = sizeof(s_wifi_ap_list) / sizeof(s_wifi_ap_list[0]);
    }
    memcpy(s_wifi_ap_list, aps, ap_count * sizeof(s_wifi_ap_list[0]));
    s_wifi_ap_count = ap_count;
    s_wifi_selected_index = -1;
    memset(&s_wifi_result, 0, sizeof(s_wifi_result));
    s_wifi_result_cancelled = false;

    if (!s_wifi_done_sem) {
        s_wifi_done_sem = xSemaphoreCreateBinary();
        if (!s_wifi_done_sem) {
            return ESP_ERR_NO_MEM;
        }
    }
    while (xSemaphoreTake(s_wifi_done_sem, 0) == pdTRUE) {
    }

    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    ui_build_wifi_screen();
    lv_scr_load(s_wifi_screen);
    lv_timer_handler();
    lvgl_port_unlock();

    if (xSemaphoreTake(s_wifi_done_sem, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    if (s_wifi_result_cancelled) {
        return ESP_FAIL;
    }

    memcpy(out_creds, &s_wifi_result, sizeof(*out_creds));
    return ESP_OK;
}

bool display_ui_enabled(void)
{
    return s_enabled;
}

static esp_err_t ui_init_display(void)
{
    const gpio_num_t backlight = CONFIG_TETHER_JC_LCD_BL_GPIO;
    ESP_LOGI(TAG, "LCD init start");
    if (backlight >= 0) {
        ESP_LOGI(TAG, "LCD backlight GPIO=%d off", (int)backlight);
        gpio_reset_pin(backlight);
        if (gpio_set_direction(backlight, GPIO_MODE_OUTPUT) != ESP_OK) {
            return ESP_FAIL;
        }
        if (gpio_set_level(backlight, !CONFIG_TETHER_JC_LCD_BL_ACTIVE_LEVEL) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    ui_log_nv3041_init_sequence();

#if CONFIG_TETHER_BOARD_PROFILE_JC4827W543
    const spi_host_device_t host = SPI2_HOST;
    ESP_LOGI(TAG, "LCD QSPI bus init host=%d clk=%d cs=%d d0=%d d1=%d d2=%d d3=%d",
             (int)host,
             CONFIG_TETHER_JC_LCD_CLK_GPIO,
             CONFIG_TETHER_JC_LCD_CS_GPIO,
             CONFIG_TETHER_JC_LCD_DATA0_GPIO,
             CONFIG_TETHER_JC_LCD_DATA1_GPIO,
             CONFIG_TETHER_JC_LCD_DATA2_GPIO,
             CONFIG_TETHER_JC_LCD_DATA3_GPIO);
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_TETHER_JC_LCD_DATA0_GPIO,
        .miso_io_num = CONFIG_TETHER_JC_LCD_DATA1_GPIO,
        .quadwp_io_num = CONFIG_TETHER_JC_LCD_DATA2_GPIO,
        .quadhd_io_num = CONFIG_TETHER_JC_LCD_DATA3_GPIO,
        .sclk_io_num = CONFIG_TETHER_JC_LCD_CLK_GPIO,
        .max_transfer_sz = 480 * 40 * sizeof(uint16_t),
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    if (spi_bus_initialize(host, &bus_cfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "LCD QSPI bus init failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "LCD QSPI bus ready");

    const spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = 20 * 1000 * 1000,
        .spics_io_num = CONFIG_TETHER_JC_LCD_CS_GPIO,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    if (spi_bus_add_device(host, &devcfg, &s_lcd_spi) != ESP_OK) {
        ESP_LOGE(TAG, "LCD QSPI device add failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "LCD QSPI device ready");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = CONFIG_TETHER_JC_LCD_CS_GPIO,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = 20 * 1000 * 1000,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {
            .quad_mode = 1,
        },
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)host, &io_cfg, &s_panel_io) != ESP_OK) {
        ESP_LOGE(TAG, "LCD panel IO init failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "LCD panel IO ready");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CONFIG_TETHER_JC_LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&s_jc4827_nv3041_vendor_cfg,
    };
    if (esp_lcd_new_panel_nv3041a(s_panel_io, &panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "LCD panel create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "LCD panel created");
    if (esp_lcd_panel_reset(s_panel) != ESP_OK ||
        esp_lcd_panel_init(s_panel) != ESP_OK ||
        esp_lcd_panel_disp_on_off(s_panel, true) != ESP_OK) {
        ESP_LOGE(TAG, "LCD panel init failed");
        return ESP_FAIL;
    }
    if (esp_lcd_panel_invert_color(s_panel, true) != ESP_OK) {
        ESP_LOGE(TAG, "LCD invert color failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "LCD panel ready");

    if (backlight >= 0) {
        ESP_LOGI(TAG, "LCD pre-backlight white frame");
        ui_panel_fill_solid(0xFFFF);
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "LCD pre-backlight black frame");
        ui_panel_fill_solid(0x0000);
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "LCD backlight GPIO=%d on", (int)backlight);
        if (gpio_set_level(backlight, CONFIG_TETHER_JC_LCD_BL_ACTIVE_LEVEL) != ESP_OK) {
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI(TAG, "LCD panel driver path enabled; skipping diagnostics");
    return ESP_OK;
#else
    const spi_host_device_t host = SPI2_HOST;
    ESP_LOGI(TAG, "LCD QSPI bus init host=%d clk=%d cs=%d d0=%d d1=%d d2=%d d3=%d",
             (int)host,
             CONFIG_TETHER_JC_LCD_CLK_GPIO,
             CONFIG_TETHER_JC_LCD_CS_GPIO,
             CONFIG_TETHER_JC_LCD_DATA0_GPIO,
             CONFIG_TETHER_JC_LCD_DATA1_GPIO,
             CONFIG_TETHER_JC_LCD_DATA2_GPIO,
             CONFIG_TETHER_JC_LCD_DATA3_GPIO);
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_TETHER_JC_LCD_DATA0_GPIO,
        .miso_io_num = CONFIG_TETHER_JC_LCD_DATA1_GPIO,
        .quadwp_io_num = CONFIG_TETHER_JC_LCD_DATA2_GPIO,
        .quadhd_io_num = CONFIG_TETHER_JC_LCD_DATA3_GPIO,
        .sclk_io_num = CONFIG_TETHER_JC_LCD_CLK_GPIO,
        .max_transfer_sz = 480 * 40 * sizeof(uint16_t),
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    if (spi_bus_initialize(host, &bus_cfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "LCD SPI bus init failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "LCD SPI bus ready");

    ESP_LOGI(TAG, "LCD panel IO init");
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = CONFIG_TETHER_JC_LCD_CS_GPIO,
        .dc_gpio_num = CONFIG_TETHER_JC_LCD_DC_GPIO,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 1,
        .trans_queue_depth = 2,
        .cs_ena_pretrans = 2,
        .cs_ena_posttrans = 2,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .flags = {
            .quad_mode = 1,
        },
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)host, &io_cfg, &s_panel_io) != ESP_OK) {
        ESP_LOGE(TAG, "LCD panel IO init failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "LCD panel IO ready");

    ESP_LOGI(TAG, "LCD panel create");
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CONFIG_TETHER_JC_LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
#if CONFIG_TETHER_BOARD_PROFILE_JC4827W543
    panel_cfg.vendor_config = (void *)&s_jc4827_nv3041_vendor_cfg;
    ESP_LOGI(TAG, "LCD panel vendor_config set");
#endif
    if (esp_lcd_new_panel_nv3041a(s_panel_io, &panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "LCD panel create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "LCD panel created");
    ESP_LOGI(TAG, "LCD panel reset/init/on");
    if (esp_lcd_panel_reset(s_panel) != ESP_OK ||
        esp_lcd_panel_init(s_panel) != ESP_OK ||
        esp_lcd_panel_disp_on_off(s_panel, true) != ESP_OK) {
        ESP_LOGE(TAG, "LCD panel init failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "LCD panel reset/init/on done");

    ui_dump_panel_registers();

    if (backlight >= 0) {
        ESP_LOGI(TAG, "LCD pre-backlight white frame");
        ui_panel_fill_solid(__builtin_bswap16(0xFFFF));
        vTaskDelay(pdMS_TO_TICKS(40));
        ESP_LOGI(TAG, "LCD pre-backlight black frame");
        ui_panel_fill_solid(__builtin_bswap16(0x0000));
        vTaskDelay(pdMS_TO_TICKS(40));
        ESP_LOGI(TAG, "LCD backlight GPIO=%d on", (int)backlight);
        if (gpio_set_level(backlight, CONFIG_TETHER_JC_LCD_BL_ACTIVE_LEVEL) != ESP_OK) {
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    return ESP_OK;
#endif
}

static esp_err_t ui_init_touch(void)
{
    if (s_touch_handle) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "GT911 touch init start");
    ui_gt911_reset_sequence();

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_TETHER_JC_TOUCH_SDA_GPIO,
        .scl_io_num = CONFIG_TETHER_JC_TOUCH_SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_i2c_bus) != ESP_OK) {
        ESP_LOGE(TAG, "touch I2C bus init failed");
        return ESP_FAIL;
    }

    esp_lcd_panel_io_handle_t touch_io = NULL;
    const esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    if (esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_cfg, &touch_io) != ESP_OK) {
        ESP_LOGE(TAG, "touch IO init failed");
        return ESP_FAIL;
    }

    esp_lcd_touch_config_t touch_cfg = {
        .x_max = 480,
        .y_max = 272,
        .rst_gpio_num = CONFIG_TETHER_JC_TOUCH_RST_GPIO,
        .int_gpio_num = CONFIG_TETHER_JC_TOUCH_INT_GPIO,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = false,
            // Keep touch coordinates in the same orientation as the panel.
            .mirror_x = false,
            .mirror_y = false,
        },
    };
    if (esp_lcd_touch_new_i2c_gt911(touch_io, &touch_cfg, &s_touch_handle) != ESP_OK) {
        ESP_LOGE(TAG, "GT911 init failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "GT911 touch init ok");
    return ESP_OK;
}

static esp_err_t ui_init_lvgl(void)
{
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    if (lvgl_port_init(&lvgl_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "lvgl port init failed");
        return ESP_FAIL;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_panel_io,
        .panel_handle = s_panel,
        .buffer_size = 480 * 20,
        .double_buffer = 0,
        .hres = 480,
        .vres = 272,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            // Must match the panel's baseline MADCTL orientation (0xC0).
            // esp_lvgl_port reapplies these values and can override panel init.
            .mirror_x = true,
            .mirror_y = true,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        return ESP_FAIL;
    }

    if (s_touch_handle) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = s_disp,
            .handle = s_touch_handle,
        };
        s_touch = lvgl_port_add_touch(&touch_cfg);
        if (!s_touch) {
            ESP_LOGW(TAG, "lvgl touch add failed; continue without touch");
        }
    } else {
        ESP_LOGW(TAG, "touch handle missing; continue without touch");
    }

    return ESP_OK;
}

static void __attribute__((unused)) ui_build_screen(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x041014), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Tethering log");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FFAA), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 6);

    s_scroll = lv_obj_create(screen);
    lv_obj_set_size(s_scroll, UI_SCREEN_W - 16, UI_SCREEN_H - 32);
    lv_obj_align(s_scroll, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_scroll, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_style_bg_color(s_scroll, lv_color_hex(0x0B1D22), 0);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scroll, 1, 0);
    lv_obj_set_style_border_color(s_scroll, lv_color_hex(0x2B6F77), 0);
    lv_obj_set_style_pad_all(s_scroll, 6, 0);

    s_label = lv_label_create(s_scroll);
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_label, UI_SCREEN_W - 28);
    lv_obj_align(s_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_color(s_label, lv_color_hex(0xF2FFF8), 0);
    lv_obj_set_style_text_opa(s_label, LV_OPA_COVER, 0);
    ui_refresh_label_locked();
}

void display_ui_init(void)
{
    if (!s_board_has_lcd) {
        return;
    }
    ESP_LOGI(TAG, "display_ui_init begin");
    ui_ring_init();
    if (ui_init_display() != ESP_OK) {
        ESP_LOGW(TAG, "display init failed");
        return;
    }
#if CONFIG_TETHER_BOARD_PROFILE_JC4827W543
    if (ui_init_touch() != ESP_OK) {
        ESP_LOGW(TAG, "touch init failed");
    }
    if (ui_init_lvgl() != ESP_OK) {
        ESP_LOGW(TAG, "lvgl init failed");
        return;
    }
    if (lvgl_port_lock(0)) {
        ui_build_log_screen();
        lv_timer_handler();
        lvgl_port_unlock();
        s_enabled = true;
        ESP_LOGI(TAG, "display_ui_init done");
    } else {
        ESP_LOGW(TAG, "display_ui_init lock failed");
    }
#else
    if (ui_init_touch() != ESP_OK) {
        ESP_LOGW(TAG, "touch init failed");
    }
    if (ui_init_lvgl() != ESP_OK) {
        ESP_LOGW(TAG, "lvgl init failed");
        return;
    }
    if (lvgl_port_lock(0)) {
        ui_build_screen();
        lv_timer_handler();
        lvgl_port_unlock();
        s_enabled = true;
        ESP_LOGI(TAG, "display_ui_init done");
    } else {
        ESP_LOGW(TAG, "display_ui_init lock failed");
    }
#endif
}

void display_ui_append_line(const char *line)
{
    if (!s_board_has_lcd) {
        return;
    }
    ui_ring_push(line);
    if (s_enabled && lvgl_port_lock(0)) {
        ui_refresh_label_locked();
        lv_timer_handler();
        lvgl_port_unlock();
    }
}

void display_ui_begin_line(const char *line)
{
    if (!s_board_has_lcd) {
        return;
    }
    ui_ring_push(line);
    if (s_enabled && lvgl_port_lock(0)) {
        ui_refresh_label_locked();
        lv_timer_handler();
        lvgl_port_unlock();
    }
}

void display_ui_replace_last_line(const char *line)
{
    if (!s_board_has_lcd) {
        return;
    }
    ui_ring_replace_last(line);
    if (s_enabled && lvgl_port_lock(0)) {
        ui_refresh_label_locked();
        lv_timer_handler();
        lvgl_port_unlock();
    }
}

void display_ui_append_linef(const char *fmt, ...)
{
    char line[UI_LOG_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    display_ui_append_line(line);
}

void display_ui_update(void)
{
    if (!s_enabled) {
        return;
    }
    if (lvgl_port_lock(0)) {
        ui_refresh_label_locked();
        lv_timer_handler();
        lvgl_port_unlock();
    }
}
