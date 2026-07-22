// esp_lcd_nv3041a.h — QSPI panel driver for the NV3041A LCD controller.
//
// Public API mirrors Espressif's QSPI esp_lcd drivers (esp_lcd_sh8601 style):
// create a QSPI panel-IO with NV3041A_PANEL_IO_QSPI_CONFIG(), then a panel
// with esp_lcd_new_panel_nv3041a(). The panel is driven with the standard
// esp_lcd_panel_* ops (reset/init/draw_bitmap/invert_color/mirror/...).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// One NV3041A init command: register + payload + optional post-delay.
typedef struct {
    int cmd;                 // command / register
    const void *data;        // payload bytes (may be NULL)
    size_t data_bytes;       // payload length
    unsigned int delay_ms;   // delay after sending
} nv3041a_lcd_init_cmd_t;

// Vendor-specific config passed via esp_lcd_panel_dev_config_t::vendor_config.
typedef struct {
    const nv3041a_lcd_init_cmd_t *init_cmds; // NULL -> use built-in default table
    uint16_t init_cmds_size;                 // number of entries in init_cmds
    struct {
        unsigned int use_qspi_interface: 1;  // must be 1 for the JC4827W543
    } flags;
} nv3041a_vendor_config_t;

// Create an NV3041A panel bound to a (QSPI) panel-IO handle.
esp_err_t esp_lcd_new_panel_nv3041a(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel);

// QSPI panel-IO config helper. NV3041A over QSPI uses a 8-bit command phase
// and 24-bit address phase (Arduino_GFX convention: 0x02 write-cmd prefix,
// 0x32 write-memory prefix are emitted by the driver via dc bits).
#define NV3041A_PANEL_IO_QSPI_CONFIG(cs_gpio, callback, callback_ctx)   \
    {                                                                   \
        .cs_gpio_num = cs_gpio,                                         \
        .dc_gpio_num = -1,                                             \
        .spi_mode = 0,                                                  \
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,                                  \
        .trans_queue_depth = 10,                                       \
        .on_color_trans_done = callback,                               \
        .user_ctx = callback_ctx,                                      \
        .lcd_cmd_bits = 32,                                            \
        .lcd_param_bits = 8,                                           \
        .flags = {                                                     \
            .quad_mode = true,                                         \
        },                                                             \
    }

#ifdef __cplusplus
}
#endif
