/*
 * esp_lcd_nv3041a.c — QSPI panel driver for the NV3041A LCD controller.
 *
 * Target board: Guition JC4827W543C (ESP32-S3, 480x272 IPS).
 *
 * NV3041A quirks handled here:
 *   - Color inversion must be ON  -> init() issues INVON (0x21).
 *   - RGB565 must go out big-endian on the wire -> draw_bitmap() byte-swaps.
 *   - Only 0/180 rotation is valid -> swap_xy() is rejected.
 *
 * Wire protocol (QSPI, matches Arduino_GFX / Espressif SH8601 convention):
 *   - register writes  : cmd word = (0x02 << 24) | (reg << 8)   via tx_param
 *   - pixel/color write: cmd word = (0x32 << 24) | (0x2C << 8)  via tx_color
 * The panel-IO is created with lcd_cmd_bits = 32, so esp_lcd splits the word
 * into an 8-bit opcode phase (bits 31..24) and a 24-bit address phase.
 */

#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "esp_log.h"

#include "esp_lcd_nv3041a.h"

#define LCD_OPCODE_WRITE_CMD    (0x02ULL)
#define LCD_OPCODE_WRITE_COLOR  (0x32ULL)

// MADCTL bits for the NV3041A (RGB order, MX/MY select the 0/180 flip).
#define NV3041A_MADCTL_MY   0x80
#define NV3041A_MADCTL_MX   0x40
#define NV3041A_MADCTL_RGB  0x00
// Rotation 0 == MX | MY | RGB (native 480x272 landscape).
#define NV3041A_MADCTL_DEFAULT  (NV3041A_MADCTL_MX | NV3041A_MADCTL_MY | NV3041A_MADCTL_RGB)

static const char *TAG = "nv3041a";

static esp_err_t panel_nv3041a_del(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3041a_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3041a_init(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3041a_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_nv3041a_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_nv3041a_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_nv3041a_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_nv3041a_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_nv3041a_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;                     // hardware RST GPIO, -1 if not wired
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val;                     // current MADCTL (0x36) value
    const nv3041a_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    uint16_t *swap_buf;                     // DMA scratch for byte-swapped pixels
    size_t swap_buf_pixels;                 // capacity of swap_buf in pixels
    struct {
        unsigned int use_qspi_interface: 1;
        unsigned int reset_level: 1;
    } flags;
} nv3041a_panel_t;

// Built-in default init table (used when vendor_config->init_cmds is NULL).
// Transcribed verbatim from docs/nv3041a_reference.md: 90 config-register
// writes, then INVON (board-required color inversion), SLPOUT and DISPON.
static const nv3041a_lcd_init_cmd_t vendor_specific_init_default[] = {
//  {cmd, { data }, data_bytes, delay_ms}
    {0xFF, (uint8_t []){0xA5}, 1, 0},   // enter command page
    {0x36, (uint8_t []){0xC0}, 1, 0},   // MADCTL: MX|MY|RGB (rotation 0)
    {0x3A, (uint8_t []){0x01}, 1, 0},   // COLMOD: 0x01 = RGB565
    {0x41, (uint8_t []){0x03}, 1, 0},   // 0x03 = 16-bit interface
    {0x44, (uint8_t []){0x15}, 1, 0},   // VBP
    {0x45, (uint8_t []){0x15}, 1, 0},   // VFP
    {0x7D, (uint8_t []){0x03}, 1, 0},   // vdds_trim
    {0xC1, (uint8_t []){0xBB}, 1, 0},   // avdd/avcl clamp
    {0xC2, (uint8_t []){0x05}, 1, 0},
    {0xC3, (uint8_t []){0x10}, 1, 0},
    {0xC6, (uint8_t []){0x3E}, 1, 0},
    {0xC7, (uint8_t []){0x25}, 1, 0},
    {0xC8, (uint8_t []){0x11}, 1, 0},
    {0x7A, (uint8_t []){0x5F}, 1, 0},   // user_vgsp
    {0x6F, (uint8_t []){0x44}, 1, 0},   // user_gvdd
    {0x78, (uint8_t []){0x70}, 1, 0},   // user_gvcl
    {0xC9, (uint8_t []){0x00}, 1, 0},
    {0x67, (uint8_t []){0x21}, 1, 0},
    {0x51, (uint8_t []){0x0A}, 1, 0},   // gate_st_o
    {0x52, (uint8_t []){0x76}, 1, 0},   // gate_ed_o
    {0x53, (uint8_t []){0x0A}, 1, 0},   // gate_st_e
    {0x54, (uint8_t []){0x76}, 1, 0},   // gate_ed_e
    {0x46, (uint8_t []){0x0A}, 1, 0},   // source timing
    {0x47, (uint8_t []){0x2A}, 1, 0},
    {0x48, (uint8_t []){0x0A}, 1, 0},
    {0x49, (uint8_t []){0x1A}, 1, 0},
    {0x56, (uint8_t []){0x43}, 1, 0},
    {0x57, (uint8_t []){0x42}, 1, 0},
    {0x58, (uint8_t []){0x3C}, 1, 0},
    {0x59, (uint8_t []){0x64}, 1, 0},
    {0x5A, (uint8_t []){0x41}, 1, 0},
    {0x5B, (uint8_t []){0x3C}, 1, 0},
    {0x5C, (uint8_t []){0x02}, 1, 0},
    {0x5D, (uint8_t []){0x3C}, 1, 0},
    {0x5E, (uint8_t []){0x1F}, 1, 0},
    {0x60, (uint8_t []){0x80}, 1, 0},
    {0x61, (uint8_t []){0x3F}, 1, 0},
    {0x62, (uint8_t []){0x21}, 1, 0},
    {0x63, (uint8_t []){0x07}, 1, 0},
    {0x64, (uint8_t []){0xE0}, 1, 0},
    {0x65, (uint8_t []){0x02}, 1, 0},
    {0xCA, (uint8_t []){0x20}, 1, 0},   // avdd mux
    {0xCB, (uint8_t []){0x52}, 1, 0},
    {0xCC, (uint8_t []){0x10}, 1, 0},
    {0xCD, (uint8_t []){0x42}, 1, 0},
    {0xD0, (uint8_t []){0x20}, 1, 0},   // avcl mux
    {0xD1, (uint8_t []){0x52}, 1, 0},
    {0xD2, (uint8_t []){0x10}, 1, 0},
    {0xD3, (uint8_t []){0x42}, 1, 0},
    {0xD4, (uint8_t []){0x0A}, 1, 0},   // vgh mux
    {0xD5, (uint8_t []){0x32}, 1, 0},
    {0x80, (uint8_t []){0x00}, 1, 0},   // gamma VRP/VRN...
    {0xA0, (uint8_t []){0x00}, 1, 0},
    {0x81, (uint8_t []){0x07}, 1, 0},
    {0xA1, (uint8_t []){0x06}, 1, 0},
    {0x82, (uint8_t []){0x02}, 1, 0},
    {0xA2, (uint8_t []){0x01}, 1, 0},
    {0x86, (uint8_t []){0x11}, 1, 0},
    {0xA6, (uint8_t []){0x10}, 1, 0},
    {0x87, (uint8_t []){0x27}, 1, 0},
    {0xA7, (uint8_t []){0x27}, 1, 0},
    {0x83, (uint8_t []){0x37}, 1, 0},
    {0xA3, (uint8_t []){0x37}, 1, 0},
    {0x84, (uint8_t []){0x35}, 1, 0},
    {0xA4, (uint8_t []){0x35}, 1, 0},
    {0x85, (uint8_t []){0x3F}, 1, 0},
    {0xA5, (uint8_t []){0x3F}, 1, 0},
    {0x88, (uint8_t []){0x0B}, 1, 0},
    {0xA8, (uint8_t []){0x0B}, 1, 0},
    {0x89, (uint8_t []){0x14}, 1, 0},
    {0xA9, (uint8_t []){0x14}, 1, 0},
    {0x8A, (uint8_t []){0x1A}, 1, 0},
    {0xAA, (uint8_t []){0x1A}, 1, 0},
    {0x8B, (uint8_t []){0x0A}, 1, 0},
    {0xAB, (uint8_t []){0x0A}, 1, 0},
    {0x8C, (uint8_t []){0x14}, 1, 0},
    {0xAC, (uint8_t []){0x08}, 1, 0},
    {0x8D, (uint8_t []){0x17}, 1, 0},
    {0xAD, (uint8_t []){0x07}, 1, 0},
    {0x8E, (uint8_t []){0x16}, 1, 0},
    {0xAE, (uint8_t []){0x06}, 1, 0},
    {0x8F, (uint8_t []){0x1B}, 1, 0},
    {0xAF, (uint8_t []){0x07}, 1, 0},
    {0x90, (uint8_t []){0x04}, 1, 0},
    {0xB0, (uint8_t []){0x04}, 1, 0},
    {0x91, (uint8_t []){0x0A}, 1, 0},
    {0xB1, (uint8_t []){0x0A}, 1, 0},
    {0x92, (uint8_t []){0x16}, 1, 0},
    {0xB2, (uint8_t []){0x15}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},   // exit command page
    {0x21, NULL, 0, 0},                 // INVON — REQUIRED (colors inverted)
    {0x11, NULL, 0, 120},               // SLPOUT + 120 ms
    {0x29, NULL, 0, 100},               // DISPON + 100 ms
};

static esp_err_t tx_param(nv3041a_panel_t *panel, int lcd_cmd, const void *param, size_t param_size)
{
    if (panel->flags.use_qspi_interface) {
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
    }
    return esp_lcd_panel_io_tx_param(panel->io, lcd_cmd, param, param_size);
}

static esp_err_t tx_color(nv3041a_panel_t *panel, int lcd_cmd, const void *param, size_t param_size)
{
    if (panel->flags.use_qspi_interface) {
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_COLOR << 24;
    }
    return esp_lcd_panel_io_tx_color(panel->io, lcd_cmd, param, param_size);
}

esp_err_t esp_lcd_new_panel_nv3041a(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    esp_err_t ret = ESP_OK;
    nv3041a_panel_t *nv3041a = calloc(1, sizeof(nv3041a_panel_t));
    ESP_RETURN_ON_FALSE(nv3041a, ESP_ERR_NO_MEM, TAG, "no mem for nv3041a panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    // NV3041A on this board is RGB order; MADCTL default is rotation 0 (MX|MY|RGB).
    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        nv3041a->madctl_val = NV3041A_MADCTL_DEFAULT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color element order (RGB only)");
        break;
    }

    // This panel is driven as RGB565 (COLMOD 0x01) only.
    ESP_GOTO_ON_FALSE(panel_dev_config->bits_per_pixel == 16, ESP_ERR_NOT_SUPPORTED, err, TAG,
                      "unsupported pixel width (RGB565/16-bit only)");
    nv3041a->fb_bits_per_pixel = 16;

    nv3041a->io = io;
    nv3041a->reset_gpio_num = panel_dev_config->reset_gpio_num;

    // Handle NULL vendor_config gracefully: assume QSPI + built-in init table.
    nv3041a_vendor_config_t *vendor_config = (nv3041a_vendor_config_t *)panel_dev_config->vendor_config;
    if (vendor_config) {
        nv3041a->init_cmds = vendor_config->init_cmds;
        nv3041a->init_cmds_size = vendor_config->init_cmds_size;
        nv3041a->flags.use_qspi_interface = vendor_config->flags.use_qspi_interface;
    } else {
        nv3041a->flags.use_qspi_interface = 1; // JC4827W543 is always QSPI
    }
    nv3041a->flags.reset_level = panel_dev_config->flags.reset_active_high;

    nv3041a->base.del = panel_nv3041a_del;
    nv3041a->base.reset = panel_nv3041a_reset;
    nv3041a->base.init = panel_nv3041a_init;
    nv3041a->base.draw_bitmap = panel_nv3041a_draw_bitmap;
    nv3041a->base.invert_color = panel_nv3041a_invert_color;
    nv3041a->base.set_gap = panel_nv3041a_set_gap;
    nv3041a->base.mirror = panel_nv3041a_mirror;
    nv3041a->base.swap_xy = panel_nv3041a_swap_xy;
    nv3041a->base.disp_on_off = panel_nv3041a_disp_on_off;
    *ret_panel = &(nv3041a->base);
    ESP_LOGD(TAG, "new nv3041a panel @%p", nv3041a);

    return ESP_OK;

err:
    if (nv3041a) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(nv3041a);
    }
    return ret;
}

static esp_err_t panel_nv3041a_del(esp_lcd_panel_t *panel)
{
    nv3041a_panel_t *nv3041a = __containerof(panel, nv3041a_panel_t, base);

    if (nv3041a->reset_gpio_num >= 0) {
        gpio_reset_pin(nv3041a->reset_gpio_num);
    }
    if (nv3041a->swap_buf) {
        free(nv3041a->swap_buf);
    }
    ESP_LOGD(TAG, "del nv3041a panel @%p", nv3041a);
    free(nv3041a);
    return ESP_OK;
}

static esp_err_t panel_nv3041a_reset(esp_lcd_panel_t *panel)
{
    nv3041a_panel_t *nv3041a = __containerof(panel, nv3041a_panel_t, base);

    // Perform hardware reset if a RST line is wired (not on the JC4827W543).
    if (nv3041a->reset_gpio_num >= 0) {
        gpio_set_level(nv3041a->reset_gpio_num, nv3041a->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(nv3041a->reset_gpio_num, !nv3041a->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        // Software reset (SWRESET 0x01) + 120 ms, as tftInit() does.
        ESP_RETURN_ON_ERROR(tx_param(nv3041a, LCD_CMD_SWRESET, NULL, 0), TAG, "send SWRESET failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    return ESP_OK;
}

static esp_err_t panel_nv3041a_init(esp_lcd_panel_t *panel)
{
    nv3041a_panel_t *nv3041a = __containerof(panel, nv3041a_panel_t, base);
    const nv3041a_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;

    // Vendor-specific init sequence (the whole NV3041A power-on sequence lives
    // in the table, including MADCTL/COLMOD/INVON/SLPOUT/DISPON).
    if (nv3041a->init_cmds) {
        init_cmds = nv3041a->init_cmds;
        init_cmds_size = nv3041a->init_cmds_size;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(nv3041a_lcd_init_cmd_t);
    }

    for (int i = 0; i < init_cmds_size; i++) {
        // Track MADCTL so mirror() can toggle bits from the correct base value.
        if (init_cmds[i].cmd == LCD_CMD_MADCTL && init_cmds[i].data && init_cmds[i].data_bytes >= 1) {
            nv3041a->madctl_val = ((const uint8_t *)init_cmds[i].data)[0];
        }
        ESP_RETURN_ON_ERROR(tx_param(nv3041a, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes), TAG,
                            "send init command 0x%02X failed", init_cmds[i].cmd);
        if (init_cmds[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
        }
    }

    // This panel requires color inversion ON. Issue INVON explicitly so it is
    // guaranteed even if a custom init table omits it.
    ESP_RETURN_ON_ERROR(tx_param(nv3041a, LCD_CMD_INVON, NULL, 0), TAG, "send INVON failed");

    ESP_LOGD(TAG, "send init commands success (%u entries)", (unsigned)init_cmds_size);
    return ESP_OK;
}

static esp_err_t panel_nv3041a_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    nv3041a_panel_t *nv3041a = __containerof(panel, nv3041a_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");

    x_start += nv3041a->x_gap;
    x_end += nv3041a->x_gap;
    y_start += nv3041a->y_gap;
    y_end += nv3041a->y_gap;

    // Set the GRAM write window.
    ESP_RETURN_ON_ERROR(tx_param(nv3041a, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF,
        x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF,
        (x_end - 1) & 0xFF,
    }, 4), TAG, "send CASET failed");
    ESP_RETURN_ON_ERROR(tx_param(nv3041a, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF,
        y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF,
        (y_end - 1) & 0xFF,
    }, 4), TAG, "send RASET failed");

    // NV3041A expects RGB565 big-endian on the wire, but the ESP32 framebuffer
    // is little-endian. Byte-swap into a DMA-capable scratch buffer before TX.
    size_t pixels = (size_t)(x_end - x_start) * (size_t)(y_end - y_start);
    if (nv3041a->swap_buf_pixels < pixels) {
        uint16_t *newbuf = heap_caps_realloc(nv3041a->swap_buf, pixels * sizeof(uint16_t), MALLOC_CAP_DMA);
        ESP_RETURN_ON_FALSE(newbuf, ESP_ERR_NO_MEM, TAG, "no mem for byte-swap buffer (%u px)", (unsigned)pixels);
        nv3041a->swap_buf = newbuf;
        nv3041a->swap_buf_pixels = pixels;
    }

    const uint16_t *src = (const uint16_t *)color_data;
    uint16_t *dst = nv3041a->swap_buf;
    for (size_t i = 0; i < pixels; i++) {
        uint16_t c = src[i];
        dst[i] = (uint16_t)((c << 8) | (c >> 8));
    }

    size_t len = pixels * nv3041a->fb_bits_per_pixel / 8;
    ESP_RETURN_ON_ERROR(tx_color(nv3041a, LCD_CMD_RAMWR, nv3041a->swap_buf, len), TAG, "send color data failed");

    return ESP_OK;
}

static esp_err_t panel_nv3041a_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    nv3041a_panel_t *nv3041a = __containerof(panel, nv3041a_panel_t, base);
    int command = invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF;
    ESP_RETURN_ON_ERROR(tx_param(nv3041a, command, NULL, 0), TAG, "send INV command failed");
    return ESP_OK;
}

static esp_err_t panel_nv3041a_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    nv3041a_panel_t *nv3041a = __containerof(panel, nv3041a_panel_t, base);

    // Only 0/180 rotation is valid: MX and MY (both set == rotation 0,
    // both clear == rotation 180). Toggle the requested bits.
    if (mirror_x) {
        nv3041a->madctl_val |= NV3041A_MADCTL_MX;
    } else {
        nv3041a->madctl_val &= ~NV3041A_MADCTL_MX;
    }
    if (mirror_y) {
        nv3041a->madctl_val |= NV3041A_MADCTL_MY;
    } else {
        nv3041a->madctl_val &= ~NV3041A_MADCTL_MY;
    }
    ESP_RETURN_ON_ERROR(tx_param(nv3041a, LCD_CMD_MADCTL, (uint8_t[]) {
        nv3041a->madctl_val
    }, 1), TAG, "send MADCTL failed");
    return ESP_OK;
}

static esp_err_t panel_nv3041a_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    // 90/270 rotation is not valid on this board (0/180 only).
    if (swap_axes) {
        ESP_LOGE(TAG, "swap_xy is not supported by this panel (0/180 rotation only)");
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static esp_err_t panel_nv3041a_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    nv3041a_panel_t *nv3041a = __containerof(panel, nv3041a_panel_t, base);
    nv3041a->x_gap = x_gap;
    nv3041a->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_nv3041a_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    nv3041a_panel_t *nv3041a = __containerof(panel, nv3041a_panel_t, base);
    int command = on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF;
    ESP_RETURN_ON_ERROR(tx_param(nv3041a, command, NULL, 0), TAG, "send DISP on/off failed");
    return ESP_OK;
}
