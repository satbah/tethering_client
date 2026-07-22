#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} display_ui_wifi_ap_t;

typedef struct {
    char ssid[33];
    char password[65];
} display_ui_wifi_credentials_t;

void display_ui_init(void);
void display_ui_append_line(const char *line);
void display_ui_begin_line(const char *line);
void display_ui_replace_last_line(const char *line);
void display_ui_append_linef(const char *fmt, ...);
void display_ui_update(void);
bool display_ui_enabled(void);
bool display_ui_poll_touch_random_dot(void);
bool display_ui_has_gui(void);
bool display_ui_take_select_request(void);
void display_ui_set_log_status_text(const char *text);
void display_ui_set_status_text(const char *text);
void display_ui_set_log_connected_ssid(const char *ssid);
void display_ui_set_usd_jpy_rate(float rate, bool valid);
void display_ui_show_ap_url_qr(void);
void display_ui_show_log_screen(void);
esp_err_t display_ui_wifi_prompt(const display_ui_wifi_ap_t *aps, size_t ap_count, display_ui_wifi_credentials_t *out_creds);
