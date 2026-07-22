#include <stdbool.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/rtc_io.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "display_ui.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define DEFAULT_SLEEP_SECONDS 180
#define BOOT_DEBUG_HOLD_MS 0
#define DEBUG_SOFT_WAIT_SECONDS 30
#define PRE_SLEEP_WAIT_SECONDS 12
static const char *TAG = "tether";
#define LOG_COLOR_RESET "\033[0m"
#define LOG_COLOR_INFO  "\033[0;36m"
#define LOG_COLOR_OK    "\033[0;32m"
#define LOG_COLOR_WARN  "\033[0;33m"
#define LOG_COLOR_ERR   "\033[0;31m"
#define LOGI_C(fmt, ...) ESP_LOGI(TAG, LOG_COLOR_INFO fmt LOG_COLOR_RESET, ##__VA_ARGS__)
#define LOGW_C(fmt, ...) ESP_LOGW(TAG, LOG_COLOR_WARN fmt LOG_COLOR_RESET, ##__VA_ARGS__)
#define LOGE_C(fmt, ...) ESP_LOGE(TAG, LOG_COLOR_ERR fmt LOG_COLOR_RESET, ##__VA_ARGS__)
#define LOGS_C(fmt, ...) ESP_LOGI(TAG, LOG_COLOR_OK fmt LOG_COLOR_RESET, ##__VA_ARGS__)

enum {
    NTP_SYNC_INTERVAL_SEC = 3600,
};

static EventGroupHandle_t wifi_events;
static int retry_count;
static uint16_t wifi_disconnect_reason;
static bool wifi_started;
static bool wifi_stack_ready;
static bool wifi_auto_connect_on_start;
static bool led_ready;
static bool buzzer_ready;
static bool sntp_initialized;
static esp_timer_handle_t buzzer_off_timer;
RTC_DATA_ATTR static uint8_t last_ap_channel;
RTC_DATA_ATTR static bool rtc_time_valid;
RTC_DATA_ATTR static time_t rtc_time_base_epoch;
RTC_DATA_ATTR static uint32_t rtc_elapsed_since_sync_sec;
static int64_t app_start_us;

#define BUZZER_GPIO 18
#define BUZZER_FREQ_TRY_HZ 880
#define BUZZER_FREQ_OK_HZ 1320
#define BUZZER_FREQ_FAIL_HZ 440
#define BUZZER_TONE_MS 1000
#define BUZZER_TRY_TONE_MS 50
#define BUZZER_OK_TONE_MS 100
#define BUZZER_WIFI_SWEEP_START_HZ 520
#define BUZZER_WIFI_SWEEP_END_HZ 980
#define BUZZER_WIFI_SWEEP_STEP_HZ 20
#define BUZZER_WIFI_SWEEP_STEP_MS 20
#define BUZZER_HTTP_ON_MS 200
#define BUZZER_HTTP_OFF_MS 100
#define AP_MODE_HOLD_MS 3000
#define AP_MODE_SSID "TickerPoc"
#define AP_MODE_PASS "ticker-setup"
#define AP_MODE_IP "192.168.4.1"
#define FRANKFURTER_USD_JPY_URL "https://api.frankfurter.dev/v1/latest?from=USD&to=JPY"

#define CFG_NS "cfg"
#define CFG_KEY_STA_SSID "sta_ssid"
#define CFG_KEY_STA_PASS "sta_pass"
#define CFG_KEY_API_URL "api_url"
#define CFG_KEY_POLL_SEC "poll_sec"
#define CFG_KEY_SOUND_ENABLED "sound_enabled"

static httpd_handle_t s_ap_http_server;
static TaskHandle_t s_ap_dns_task;
static volatile bool s_ap_dns_run;
static esp_timer_handle_t s_ap_restart_timer;

static bool wake_button_pressed(void);
static void display_statusf(const char *fmt, ...);

typedef struct {
    char body[256];
    size_t len;
} http_response_buf_t;

typedef struct {
    char sta_ssid[33];
    char sta_pass[65];
    char api_url[256];
    uint32_t poll_sec;
    bool sound_enabled;
} app_config_t;

enum {
    USD_JPY_RATE_SCALE = 100,
};

static app_config_t s_app_cfg;

static uint32_t app_poll_seconds(void)
{
    if (s_app_cfg.poll_sec < 30 || s_app_cfg.poll_sec > 86400) {
        return DEFAULT_SLEEP_SECONDS;
    }
    return s_app_cfg.poll_sec;
}

static void app_config_set_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->poll_sec = DEFAULT_SLEEP_SECONDS;
    cfg->sound_enabled = true;
    strlcpy(cfg->api_url, FRANKFURTER_USD_JPY_URL, sizeof(cfg->api_url));
    if (CONFIG_TETHER_WIFI_SSID[0] != '\0') {
        strlcpy(cfg->sta_ssid, CONFIG_TETHER_WIFI_SSID, sizeof(cfg->sta_ssid));
    }
    if (CONFIG_TETHER_WIFI_PASSWORD[0] != '\0') {
        strlcpy(cfg->sta_pass, CONFIG_TETHER_WIFI_PASSWORD, sizeof(cfg->sta_pass));
    }
}

static void app_config_normalize(app_config_t *cfg)
{
    if (cfg->poll_sec < 30 || cfg->poll_sec > 86400) {
        cfg->poll_sec = DEFAULT_SLEEP_SECONDS;
    }
    if (cfg->api_url[0] == '\0') {
        strlcpy(cfg->api_url, FRANKFURTER_USD_JPY_URL, sizeof(cfg->api_url));
    }
}

static esp_err_t app_config_load(app_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    app_config_set_defaults(cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t len = sizeof(cfg->sta_ssid);
        if (nvs_get_str(h, CFG_KEY_STA_SSID, cfg->sta_ssid, &len) != ESP_OK) {
            cfg->sta_ssid[0] = '\0';
        }
        len = sizeof(cfg->sta_pass);
        if (nvs_get_str(h, CFG_KEY_STA_PASS, cfg->sta_pass, &len) != ESP_OK) {
            cfg->sta_pass[0] = '\0';
        }
        len = sizeof(cfg->api_url);
        if (nvs_get_str(h, CFG_KEY_API_URL, cfg->api_url, &len) != ESP_OK) {
            cfg->api_url[0] = '\0';
        }
        uint32_t poll = cfg->poll_sec;
        if (nvs_get_u32(h, CFG_KEY_POLL_SEC, &poll) == ESP_OK) {
            cfg->poll_sec = poll;
        }
        uint8_t sound = cfg->sound_enabled ? 1 : 0;
        if (nvs_get_u8(h, CFG_KEY_SOUND_ENABLED, &sound) == ESP_OK) {
            cfg->sound_enabled = (sound != 0);
        }
        nvs_close(h);
    }

    if (cfg->sta_ssid[0] == '\0' || cfg->sta_pass[0] == '\0') {
        nvs_handle_t old_h;
        if (nvs_open("wifi_cfg", NVS_READONLY, &old_h) == ESP_OK) {
            size_t len = sizeof(cfg->sta_ssid);
            if (nvs_get_str(old_h, "ssid", cfg->sta_ssid, &len) != ESP_OK) {
                cfg->sta_ssid[0] = '\0';
            }
            len = sizeof(cfg->sta_pass);
            if (nvs_get_str(old_h, "pass", cfg->sta_pass, &len) != ESP_OK) {
                cfg->sta_pass[0] = '\0';
            }
            nvs_close(old_h);
        }
    }

    app_config_normalize(cfg);
    return ESP_OK;
}

static esp_err_t app_config_save(const app_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    app_config_t normalized = *cfg;
    app_config_normalize(&normalized);

    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, CFG_KEY_STA_SSID, normalized.sta_ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, CFG_KEY_STA_PASS, normalized.sta_pass);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, CFG_KEY_API_URL, normalized.api_url);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(h, CFG_KEY_POLL_SEC, normalized.poll_sec);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, CFG_KEY_SOUND_ENABLED, normalized.sound_enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        return err;
    }

    /* Keep compatibility with legacy wifi_cfg readers */
    nvs_handle_t old_h;
    if (nvs_open("wifi_cfg", NVS_READWRITE, &old_h) == ESP_OK) {
        (void)nvs_set_str(old_h, "ssid", normalized.sta_ssid);
        (void)nvs_set_str(old_h, "pass", normalized.sta_pass);
        (void)nvs_commit(old_h);
        nvs_close(old_h);
    }

    return ESP_OK;
}

static bool json_extract_string(const char *json, const char *key, char *out, size_t out_len)
{
    if (!json || !key || !out || out_len == 0) {
        return false;
    }

    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *pos = strstr(json, needle);
    if (!pos) {
        return false;
    }
    const char *colon = strchr(pos, ':');
    if (!colon) {
        return false;
    }
    const char *q1 = strchr(colon, '"');
    if (!q1) {
        return false;
    }
    q1++;
    const char *q2 = strchr(q1, '"');
    if (!q2) {
        return false;
    }
    size_t len = (size_t)(q2 - q1);
    if (len >= out_len) {
        len = out_len - 1;
    }
    memcpy(out, q1, len);
    out[len] = '\0';
    return true;
}

static bool json_extract_u32(const char *json, const char *key, uint32_t *out)
{
    if (!json || !key || !out) {
        return false;
    }

    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *pos = strstr(json, needle);
    if (!pos) {
        return false;
    }
    const char *colon = strchr(pos, ':');
    if (!colon) {
        return false;
    }

    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(colon + 1, &end, 10);
    if (end == colon + 1 || errno != 0 || v > 0xFFFFFFFFUL) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool json_extract_bool(const char *json, const char *key, bool *out)
{
    if (!json || !key || !out) {
        return false;
    }

    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *pos = strstr(json, needle);
    if (!pos) {
        return false;
    }
    const char *colon = strchr(pos, ':');
    if (!colon) {
        return false;
    }

    const char *value = colon + 1;
    while (*value == ' ' || *value == '\t' || *value == '\n' || *value == '\r') {
        value++;
    }

    if (strncmp(value, "true", 4) == 0 || *value == '1') {
        *out = true;
        return true;
    }
    if (strncmp(value, "false", 5) == 0 || *value == '0') {
        *out = false;
        return true;
    }
    return false;
}

static int status_led_on_level(void) {
    return CONFIG_TETHER_STATUS_LED_ACTIVE_LEVEL;
}

static int status_led_off_level(void) {
    return !CONFIG_TETHER_STATUS_LED_ACTIVE_LEVEL;
}

static void status_led_set(bool on) {
    if (!led_ready) {
        return;
    }
    const int level = on ? status_led_on_level() : status_led_off_level();
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_TETHER_STATUS_LED_GPIO, level));
}

static void status_led_blink(unsigned count, unsigned on_ms, unsigned off_ms) {
    if (!led_ready) {
        return;
    }
    for (unsigned i = 0; i < count; ++i) {
        status_led_set(true);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        status_led_set(false);
        if (i + 1 < count) {
            vTaskDelay(pdMS_TO_TICKS(off_ms));
        }
    }
}

static void status_led_success(void) {
    status_led_blink(1, CONFIG_TETHER_STATUS_LED_BLINK_MS, 0);
}

static void status_led_failure(void) {
    status_led_blink(5, CONFIG_TETHER_STATUS_LED_BLINK_MS, CONFIG_TETHER_STATUS_LED_BLINK_MS);
}

static void init_status_led(void) {
#if CONFIG_TETHER_STATUS_LED_GPIO >= 0
    const gpio_num_t led = CONFIG_TETHER_STATUS_LED_GPIO;
    gpio_reset_pin(led);
    ESP_ERROR_CHECK(gpio_set_direction(led, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(led, status_led_off_level()));
    led_ready = true;
#endif
}

static void buzzer_off_cb(void *arg)
{
    (void)arg;
    if (!buzzer_ready) {
        return;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static void buzzer_play_tone_ms(uint32_t freq_hz, uint32_t tone_ms)
{
    if (!buzzer_ready || !s_app_cfg.sound_enabled) {
        return;
    }

    if (buzzer_off_timer) {
        esp_timer_stop(buzzer_off_timer);
    }

    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_1, freq_hz);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, (1 << 9));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);

    if (buzzer_off_timer) {
        esp_timer_start_once(buzzer_off_timer, tone_ms * 1000ULL);
    }
}

static void buzzer_stop_now(void)
{
    if (!buzzer_ready) {
        return;
    }
    if (buzzer_off_timer) {
        esp_timer_stop(buzzer_off_timer);
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static void buzzer_play_tone_blocking(uint32_t freq_hz, uint32_t tone_ms)
{
    if (!buzzer_ready || !s_app_cfg.sound_enabled) {
        return;
    }
    if (buzzer_off_timer) {
        esp_timer_stop(buzzer_off_timer);
    }
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_1, freq_hz);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, (1 << 9));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    vTaskDelay(pdMS_TO_TICKS(tone_ms));
    buzzer_stop_now();
}

static void buzzer_play_wifi_fail_pattern(void)
{
    if (!buzzer_ready || !s_app_cfg.sound_enabled) {
        return;
    }

    if (buzzer_off_timer) {
        esp_timer_stop(buzzer_off_timer);
    }

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, (1 << 9));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);

    for (int repeat = 0; repeat < 3; ++repeat) {
        for (uint32_t hz = BUZZER_WIFI_SWEEP_START_HZ; hz <= BUZZER_WIFI_SWEEP_END_HZ; hz += BUZZER_WIFI_SWEEP_STEP_HZ) {
            ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_1, hz);
            vTaskDelay(pdMS_TO_TICKS(BUZZER_WIFI_SWEEP_STEP_MS));
        }
    }

    buzzer_stop_now();
}

static void buzzer_play_http_fail_pattern(void)
{
    for (int i = 0; i < 3; ++i) {
        buzzer_play_tone_blocking(BUZZER_FREQ_FAIL_HZ, BUZZER_HTTP_ON_MS);
        if (i < 2) {
            vTaskDelay(pdMS_TO_TICKS(BUZZER_HTTP_OFF_MS));
        }
    }
}

static void init_buzzer(void)
{
    const ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = BUZZER_FREQ_TRY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer_cfg) != ESP_OK) {
        return;
    }

    const ledc_channel_config_t ch_cfg = {
        .gpio_num = BUZZER_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
    };
    if (ledc_channel_config(&ch_cfg) != ESP_OK) {
        return;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = buzzer_off_cb,
        .name = "buzzer_off",
    };
    if (esp_timer_create(&timer_args, &buzzer_off_timer) != ESP_OK) {
        return;
    }

    buzzer_ready = true;
}

static void disable_oled_power(void) {
#if CONFIG_TETHER_OLED_VEXT_GPIO >= 0
    const gpio_num_t vext = CONFIG_TETHER_OLED_VEXT_GPIO;
    const int off_level = !CONFIG_TETHER_OLED_VEXT_ACTIVE_LEVEL;
    gpio_reset_pin(vext);
    ESP_ERROR_CHECK(gpio_set_direction(vext, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(vext, off_level));
#endif
}

static bool boot_button_held_for_ap_mode(void)
{
#if CONFIG_TETHER_WAKE_BUTTON_GPIO >= 0
    if (!wake_button_pressed()) {
        return false;
    }

    uint32_t held_ms = 0;
    while (held_ms < AP_MODE_HOLD_MS) {
        if (!wake_button_pressed()) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        held_ms += 50;
    }
    return true;
#else
    return false;
#endif
}

static esp_err_t ap_mode_root_get_handler(httpd_req_t *req)
{
    static const char *html =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Ticker Setup</title>"
        "<style>:root{--bg:#f3f6fa;--card:#fff;--text:#11213b;--muted:#5c6f86;--line:#d9e3ee;--accent:#1868db;--accent2:#2aa9a8;}"
        "*{box-sizing:border-box;}"
        "body{margin:0;font-family:'Hiragino Sans','Noto Sans JP','Yu Gothic UI',sans-serif;"
        "background:radial-gradient(circle at 15% -10%,#def0ff 0%,#f3f6fa 45%,#edf2f8 100%);color:var(--text);}"
        ".wrap{max-width:760px;margin:22px auto;padding:0 14px;}"
        ".card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:18px 16px;box-shadow:0 16px 40px rgba(16,33,59,0.08);}"
        ".chip{display:inline-block;padding:5px 11px;border-radius:999px;background:#e8f1ff;color:#2859a3;font-size:12px;font-weight:700;}"
        "h1{margin:8px 0 8px;font-size:28px;letter-spacing:.02em;}"
        ".lead{margin:0 0 14px;color:var(--muted);}"
        ".note{margin:14px 0 16px;padding:12px 14px;border-left:4px solid var(--accent2);background:#f6fbfb;border-radius:10px;}"
        ".grid{display:grid;grid-template-columns:1fr;gap:10px;}"
        "label{font-weight:700;font-size:13px;letter-spacing:.02em;color:#334a67;}"
        "input{width:100%;padding:11px 12px;border:1px solid var(--line);border-radius:10px;background:#fff;color:var(--text);font-size:14px;}"
        "input:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px rgba(24,104,219,0.16);}"
        ".switch{display:flex;align-items:center;justify-content:space-between;padding:12px;border:1px solid var(--line);border-radius:10px;background:#fbfdff;}"
        ".switch span{font-size:14px;color:#334a67;}"
        ".toggle{appearance:none;width:44px;height:26px;border-radius:999px;background:#c5d0dc;position:relative;cursor:pointer;transition:all .2s;}"
        ".toggle:checked{background:#2b8cff;}"
        ".toggle:before{content:'';position:absolute;top:3px;left:3px;width:20px;height:20px;border-radius:50%;background:#fff;transition:all .2s;}"
        ".toggle:checked:before{left:21px;}"
        ".actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:14px;}"
        "button{border:0;border-radius:10px;padding:11px 16px;font-weight:700;cursor:pointer;}"
        "#save-only{background:#e7edf4;color:#243347;}"
        "#save-reboot{background:linear-gradient(135deg,var(--accent),#2c8dff);color:#fff;}"
        "button:disabled{opacity:.55;cursor:not-allowed;}"
        "small{color:var(--muted);font-size:12px;}"
        "#status{margin-top:12px;min-height:20px;color:#1b3556;font-weight:700;}"
        "@media (max-width:540px){h1{font-size:24px;}.card{padding:16px 12px;}}</style>"
        "</head><body>"
        "<div class=\"wrap\"><div class=\"card\">"
        "<span class=\"chip\">AP mode active</span>"
        "<h1>Ticker Setup</h1>"
        "<p class=\"lead\">SSID: <b>" AP_MODE_SSID "</b> に接続中です。設定後に再起動して通常運用へ戻します。</p>"
        "<div class=\"note\">"
        "<p><b>セットアップ手順</b></p>"
        "<ol>"
        "<li>スマホのテザリングSSIDとパスワードを入力します。</li>"
        "<li>\"保存して再起動\" を押します。</li>"
        "<li>再起動後は、スマホの設定画面でインターネット共有のページを開いたままにしてください。</li>"
        "</ol>"
        "<p>\"保存\" だけでは AP mode のままです。</p>"
        "</div>"
        "<div class=\"grid\">"
        "<label>STA SSID</label><input id=\"sta-ssid\" maxlength=\"32\">"
        "<label>STA Password</label><input id=\"sta-pass\" type=\"password\" maxlength=\"64\">"
        "<label>API URL</label><input id=\"api-url\" maxlength=\"255\">"
        "<label>Poll interval (sec)</label><input id=\"poll-sec\" type=\"number\" min=\"30\" max=\"86400\">"
        "<div class=\"switch\"><span>通信状態を音で知らせる</span><input id=\"sound-enabled\" class=\"toggle\" type=\"checkbox\" checked></div>"
        "<small>音通知はデフォルトでONです。</small>"
        "</div>"
        "<div class=\"actions\">"
        "<button id=\"save-only\" onclick=\"saveOnly()\">保存</button>"
        "<button id=\"save-reboot\" onclick=\"saveAndRestart()\">保存して再起動</button>"
        "</div>"
        "<p id=\"status\"></p>"
        "<script>"
        "function setBusy(busy){"
        "document.getElementById('save-only').disabled=busy;"
        "document.getElementById('save-reboot').disabled=busy;"
        "}"
        "function readForm(){return {"
        "sta_ssid:document.getElementById('sta-ssid').value.trim(),"
        "sta_pass:document.getElementById('sta-pass').value.trim(),"
        "api_url:document.getElementById('api-url').value.trim(),"
        "poll_sec:parseInt(document.getElementById('poll-sec').value||'0',10),"
        "sound_enabled:document.getElementById('sound-enabled').checked"
        "};}"
        "function writeForm(c){"
        "document.getElementById('sta-ssid').value=c.sta_ssid||'';"
        "document.getElementById('sta-pass').value=c.sta_pass||'';"
        "document.getElementById('api-url').value=c.api_url||'';"
        "document.getElementById('poll-sec').value=c.poll_sec||180;"
        "document.getElementById('sound-enabled').checked=(c.sound_enabled!==false);"
        "}"
        "async function loadConfig(){"
        "const r=await fetch('/api/config');"
        "if(!r.ok) throw new Error('load failed');"
        "writeForm(await r.json());"
        "}"
        "async function saveConfig(){"
        "const payload=readForm();"
        "const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});"
        "if(!r.ok){throw new Error('save failed: '+r.status);}"
        "return r.json();"
        "}"
        "async function saveOnly(){"
        "const status=document.getElementById('status');"
        "setBusy(true);"
        "status.textContent='saving...';"
        "try{await saveConfig();status.textContent='saved. AP mode is still active.';}catch(e){status.textContent='save failed';}finally{setBusy(false);}"
        "}"
        "async function saveAndRestart(){"
        "const status=document.getElementById('status');"
        "setBusy(true);"
        "status.textContent='saving...';"
        "try{"
        "await saveConfig();"
        "status.textContent='restarting...';"
        "const r=await fetch('/api/restart',{method:'POST'});"
        "if(!r.ok){throw new Error('http '+r.status);}"
        "status.textContent='restart requested. This page will disconnect. Open your phone hotspot settings screen after reboot.';"
        "}catch(e){status.textContent='request failed';setBusy(false);}"
        "}"
        "loadConfig().catch(()=>{document.getElementById('status').textContent='config load failed';});"
        "</script>"
        "</div></div></body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ap_mode_health_get_handler(httpd_req_t *req)
{
    static const char *json = "{\"ok\":true,\"mode\":\"ap\"}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ap_mode_config_get_handler(httpd_req_t *req)
{
    app_config_t cfg;
    app_config_load(&cfg);

    char json[512];
    snprintf(json,
             sizeof(json),
             "{\"sta_ssid\":\"%s\",\"sta_pass\":\"%s\",\"api_url\":\"%s\",\"poll_sec\":%u,\"sound_enabled\":%s}",
             cfg.sta_ssid,
             cfg.sta_pass,
             cfg.api_url,
             (unsigned)cfg.poll_sec,
             cfg.sound_enabled ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ap_mode_config_post_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total >= 768) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }

    char body[768];
    int read = 0;
    while (read < total) {
        int r = httpd_req_recv(req, body + read, total - read);
        if (r <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read failed");
        }
        read += r;
    }
    body[read] = '\0';

    app_config_t next = s_app_cfg;
    (void)json_extract_string(body, "sta_ssid", next.sta_ssid, sizeof(next.sta_ssid));
    (void)json_extract_string(body, "sta_pass", next.sta_pass, sizeof(next.sta_pass));
    (void)json_extract_string(body, "api_url", next.api_url, sizeof(next.api_url));
    uint32_t poll = next.poll_sec;
    if (json_extract_u32(body, "poll_sec", &poll)) {
        next.poll_sec = poll;
    }
    bool sound_enabled = next.sound_enabled;
    if (json_extract_bool(body, "sound_enabled", &sound_enabled)) {
        next.sound_enabled = sound_enabled;
    }
    app_config_normalize(&next);

    if (next.sta_ssid[0] == '\0' || next.sta_pass[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid/pass required");
    }
    if (strncmp(next.api_url, "http://", 7) != 0 && strncmp(next.api_url, "https://", 8) != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "api_url must start with http:// or https://");
    }

    esp_err_t err = app_config_save(&next);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    }
    s_app_cfg = next;

    static const char *json = "{\"ok\":true}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static void ap_mode_restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static esp_err_t ap_mode_restart_post_handler(httpd_req_t *req)
{
    display_statusf("AP restart requested");
    LOGW_C("AP restart requested");

    if (!s_ap_restart_timer) {
        const esp_timer_create_args_t timer_args = {
            .callback = ap_mode_restart_cb,
            .name = "ap_restart",
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_ap_restart_timer));
    }

    (void)esp_timer_stop(s_ap_restart_timer);
    ESP_ERROR_CHECK(esp_timer_start_once(s_ap_restart_timer, 200000ULL));

    static const char *json = "{\"ok\":true,\"restarting\":true}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ap_mode_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_MODE_IP "/");
    return httpd_resp_send(req, "", 0);
}

static esp_err_t ap_mode_404_redirect_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_MODE_IP "/");
    return httpd_resp_send(req, "", 0);
}

static esp_err_t ap_mode_start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.max_open_sockets = 2;

    esp_err_t err = httpd_start(&s_ap_http_server, &config);
    if (err != ESP_OK) {
        return err;
    }

    static const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = ap_mode_root_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t health_uri = {
        .uri = "/api/health",
        .method = HTTP_GET,
        .handler = ap_mode_health_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t config_get_uri = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = ap_mode_config_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t config_post_uri = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = ap_mode_config_post_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t restart_uri = {
        .uri = "/api/restart",
        .method = HTTP_POST,
        .handler = ap_mode_restart_post_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t generate_204_uri = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = ap_mode_redirect_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t hotspot_detect_uri = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = ap_mode_redirect_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t ncsi_uri = {
        .uri = "/ncsi.txt",
        .method = HTTP_GET,
        .handler = ap_mode_redirect_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t connecttest_uri = {
        .uri = "/connecttest.txt",
        .method = HTTP_GET,
        .handler = ap_mode_redirect_handler,
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_ap_http_server, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_ap_http_server, &health_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_ap_http_server, &config_get_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_ap_http_server, &config_post_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_ap_http_server, &restart_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_ap_http_server, &generate_204_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_ap_http_server, &hotspot_detect_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_ap_http_server, &ncsi_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_ap_http_server, &connecttest_uri));
    ESP_ERROR_CHECK(httpd_register_err_handler(s_ap_http_server, HTTPD_404_NOT_FOUND, ap_mode_404_redirect_handler));
    return ESP_OK;
}

static void ap_mode_dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t rx[512];
    uint8_t tx[512];
    s_ap_dns_run = true;
    while (s_ap_dns_run) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &from_len);
        if (n < 12) {
            continue;
        }

        int qname = 12;
        while (qname < n && rx[qname] != 0) {
            const int label_len = rx[qname];
            qname += label_len + 1;
        }
        if (qname + 5 >= n) {
            continue;
        }

        const int question_len = (qname + 1 + 4) - 12;
        memset(tx, 0, sizeof(tx));
        tx[0] = rx[0];
        tx[1] = rx[1];
        tx[2] = 0x81;
        tx[3] = 0x80;
        tx[4] = 0x00;
        tx[5] = 0x01;
        tx[6] = 0x00;
        tx[7] = 0x01;
        memcpy(&tx[12], &rx[12], question_len);

        int pos = 12 + question_len;
        tx[pos++] = 0xC0;
        tx[pos++] = 0x0C;
        tx[pos++] = 0x00;
        tx[pos++] = 0x01;
        tx[pos++] = 0x00;
        tx[pos++] = 0x01;
        tx[pos++] = 0x00;
        tx[pos++] = 0x00;
        tx[pos++] = 0x00;
        tx[pos++] = 0x1E;
        tx[pos++] = 0x00;
        tx[pos++] = 0x04;
        tx[pos++] = 192;
        tx[pos++] = 168;
        tx[pos++] = 4;
        tx[pos++] = 1;

        sendto(sock, tx, pos, 0, (struct sockaddr *)&from, from_len);
    }

    close(sock);
    vTaskDelete(NULL);
}

static esp_err_t ap_mode_start_wifi_ap(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, AP_MODE_SSID, sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, AP_MODE_PASS, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = strlen(AP_MODE_SSID);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

static void ap_mode_run(void)
{
    display_statusf("AP MODE start");
    LOGW_C("AP MODE start");

    if (ap_mode_start_wifi_ap() != ESP_OK) {
        display_statusf("AP MODE wifi init fail");
        LOGE_C("AP MODE wifi init fail");
        return;
    }
    if (ap_mode_start_http_server() != ESP_OK) {
        display_statusf("AP MODE http init fail");
        LOGE_C("AP MODE http init fail");
        return;
    }
    xTaskCreate(ap_mode_dns_task, "ap_dns", 3072, NULL, 4, &s_ap_dns_task);

    display_statusf("SETUP AP %s %s", AP_MODE_SSID, AP_MODE_IP);
    display_ui_show_ap_url_qr();
    while (1) {
        display_ui_update();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void ensure_timezone(void)
{
    static bool tz_done;
    if (!tz_done) {
        setenv("TZ", "JST-9", 1);
        tzset();
        tz_done = true;
    }
}

static void format_timestamp(char *buf, size_t len)
{
    ensure_timezone();
    const time_t now = time(NULL);
    if (now < 1700000000) {
        strlcpy(buf, "no-time", len);
        return;
    }
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm_now);
}

static void display_statusf(const char *fmt, ...)
{
    char body[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    display_ui_set_log_status_text(body);
}

static void display_final_http_summary(bool ok, int status)
{
    char ts[32];
    char line[160];

    format_timestamp(ts, sizeof(ts));
    snprintf(line,
             sizeof(line),
             "%s HTTPS GET %s %d",
             ts,
             ok ? "OK" : "FAIL",
             status);

    display_ui_append_line(line);
}

static void restore_time_from_rtc(void)
{
    if (!rtc_time_valid) {
        return;
    }

    const time_t estimated = rtc_time_base_epoch + rtc_elapsed_since_sync_sec;
    struct timeval tv = {
        .tv_sec = estimated,
        .tv_usec = 0,
    };
    settimeofday(&tv, NULL);
}

static void mark_time_synced(void)
{
    const time_t now = time(NULL);
    rtc_time_valid = true;
    rtc_time_base_epoch = now;
    rtc_elapsed_since_sync_sec = 0;
}

static bool time_sync_needed(void)
{
    if (!rtc_time_valid) {
        return true;
    }
    return rtc_elapsed_since_sync_sec >= NTP_SYNC_INTERVAL_SEC;
}

static esp_err_t sync_time_via_ntp(void)
{
    if (!sntp_initialized) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        config.start = true;
        config.wait_for_sync = false;
        config.server_from_dhcp = false;
        config.renew_servers_after_new_IP = false;

        esp_err_t init_err = esp_netif_sntp_init(&config);
        if (init_err != ESP_OK && init_err != ESP_ERR_INVALID_STATE) {
            return init_err;
        }
        sntp_initialized = true;
    }

    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
    if (err == ESP_OK) {
        mark_time_synced();
    } else if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    return err;
}

static void wifi_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (wifi_auto_connect_on_start) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *conn = (const wifi_event_sta_connected_t *)data;
        if (conn && conn->channel != 0) {
            last_ap_channel = conn->channel;
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)data;
        wifi_disconnect_reason = disc ? disc->reason : 0;
        if (++retry_count < 4) {
            esp_wifi_connect();
        } else if (wifi_events) {
            xEventGroupSetBits(wifi_events, WIFI_FAILED_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (wifi_events) {
            xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
        }
    }
}

static esp_err_t wifi_stack_init(void)
{
    if (wifi_stack_ready) {
        return ESP_OK;
    }

    if (!wifi_events) {
        wifi_events = xEventGroupCreate();
        if (!wifi_events) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    wifi_stack_ready = true;
    return ESP_OK;
}

static esp_err_t wifi_ensure_started(void)
{
    esp_err_t err = wifi_stack_init();
    if (err != ESP_OK) {
        return err;
    }
    if (wifi_started) {
        return ESP_OK;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }
    wifi_started = true;
    return ESP_OK;
}

static esp_err_t wifi_connect_with_credentials(const display_ui_wifi_credentials_t *creds)
{
    if (!creds || creds->ssid[0] == '\0' || creds->password[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    LOGI_C("WIFI connect start ssid=%s", creds->ssid);

    esp_err_t err = wifi_ensure_started();
    if (err != ESP_OK) {
        return err;
    }

    retry_count = 0;
    wifi_disconnect_reason = 0;
    xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    wifi_auto_connect_on_start = false;

    wifi_config_t config = {
        .sta = {
            // Keep threshold open so mixed-mode APs (WPA/WPA2, WPA2/WPA3) are not filtered out.
            .threshold.authmode = WIFI_AUTH_OPEN,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            .sae_pk_mode = WPA3_SAE_PK_MODE_AUTOMATIC,
            .failure_retry_cnt = 3,
        },
    };
    strlcpy((char *)config.sta.ssid, creds->ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, creds->password, sizeof(config.sta.password));

    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT && err != ESP_ERR_WIFI_NOT_STARTED) {
        return err;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        LOGE_C("WIFI connect trigger failed err=%s", esp_err_to_name(err));
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(CONFIG_TETHER_WIFI_TIMEOUT_SECONDS * 1000));
    if (bits & WIFI_CONNECTED_BIT) {
        LOGI_C("WIFI connect success ssid=%s", creds->ssid);
        return ESP_OK;
    }
    LOGW_C("WIFI connect timeout/fail ssid=%s reason=%u", creds->ssid, (unsigned)wifi_disconnect_reason);
    return ESP_FAIL;
}

static esp_err_t wifi_creds_load(display_ui_wifi_credentials_t *creds)
{
    if (!creds) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(creds, 0, sizeof(*creds));

    app_config_t cfg;
    esp_err_t err = app_config_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    if (cfg.sta_ssid[0] == '\0' || cfg.sta_pass[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    strlcpy(creds->ssid, cfg.sta_ssid, sizeof(creds->ssid));
    strlcpy(creds->password, cfg.sta_pass, sizeof(creds->password));
    return ESP_OK;
}

static esp_err_t usd_jpy_rate_cache_save(float rate)
{
    if (!isfinite(rate) || rate <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    const int32_t scaled = (int32_t)lroundf(rate * USD_JPY_RATE_SCALE);
    nvs_handle_t handle;
    esp_err_t err = nvs_open("fx_cache", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_i32(handle, "usd_jpy_x100", scaled);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t usd_jpy_rate_cache_load(float *out_rate)
{
    if (!out_rate) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open("fx_cache", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    int32_t scaled = 0;
    err = nvs_get_i32(handle, "usd_jpy_x100", &scaled);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }
    if (scaled <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_rate = (float)scaled / (float)USD_JPY_RATE_SCALE;
    return ESP_OK;
}

static bool wifi_creds_from_config(display_ui_wifi_credentials_t *creds)
{
    if (!creds) {
        return false;
    }

    if (s_app_cfg.sta_ssid[0] == '\0' || s_app_cfg.sta_pass[0] == '\0') {
        return false;
    }

    memset(creds, 0, sizeof(*creds));
    strlcpy(creds->ssid, s_app_cfg.sta_ssid, sizeof(creds->ssid));
    strlcpy(creds->password, s_app_cfg.sta_pass, sizeof(creds->password));
    return true;
}

static bool http_status_is_success(int status)
{
    return status >= 200 && status < 300;
}

static esp_err_t http_capture_handler(esp_http_client_event_t *evt)
{
    if (!evt || evt->event_id != HTTP_EVENT_ON_DATA || !evt->data || evt->data_len <= 0 || !evt->user_data) {
        return ESP_OK;
    }

    http_response_buf_t *resp = (http_response_buf_t *)evt->user_data;
    if (resp->len >= sizeof(resp->body) - 1) {
        return ESP_OK;
    }

    size_t copy_len = (size_t)evt->data_len;
    const size_t remain = (sizeof(resp->body) - 1) - resp->len;
    if (copy_len > remain) {
        copy_len = remain;
    }
    memcpy(resp->body + resp->len, evt->data, copy_len);
    resp->len += copy_len;
    resp->body[resp->len] = '\0';
    return ESP_OK;
}

static bool parse_usd_jpy_rate(const char *json, float *out_rate)
{
    if (!json || !out_rate) {
        return false;
    }

    const char *rates = strstr(json, "\"rates\"");
    if (!rates) {
        return false;
    }

    const char *jpy = strstr(rates, "\"JPY\"");
    if (!jpy) {
        return false;
    }

    const char *colon = strchr(jpy, ':');
    if (!colon) {
        return false;
    }

    errno = 0;
    char *endptr = NULL;
    const float rate = strtof(colon + 1, &endptr);
    if (endptr == colon + 1 || errno != 0 || !isfinite(rate) || rate <= 0.0f) {
        return false;
    }

    *out_rate = rate;
    return true;
}

static esp_err_t call_server(int *status, float *usd_jpy_rate, bool *rate_valid) {
    if (status) {
        *status = -1;
    }
    if (usd_jpy_rate) {
        *usd_jpy_rate = 0.0f;
    }
    if (rate_valid) {
        *rate_valid = false;
    }

    http_response_buf_t response = {0};

    const char *url = (s_app_cfg.api_url[0] != '\0') ? s_app_cfg.api_url : FRANKFURTER_USD_JPY_URL;
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_capture_handler,
        .user_data = &response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    const int http_status = esp_http_client_get_status_code(client);
    const int64_t content_length = esp_http_client_get_content_length(client);
    if (status) {
        *status = http_status;
    }

    bool parsed = false;
    float parsed_rate = 0.0f;
    if (err == ESP_OK && http_status_is_success(http_status)) {
        parsed = parse_usd_jpy_rate(response.body, &parsed_rate);
        if (!parsed) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }

    if (parsed) {
        if (usd_jpy_rate) {
            *usd_jpy_rate = parsed_rate;
        }
        if (rate_valid) {
            *rate_valid = true;
        }
    }

    if (err == ESP_OK) {
        LOGI_C("HTTP response status=%d len=%lld USD/JPY=%.2f url=%s",
               http_status,
               (long long)content_length,
               (double)parsed_rate,
               url);
    } else {
        LOGE_C("HTTP transport err=%s status=%d len=%lld url=%s",
               esp_err_to_name(err),
               http_status,
               (long long)content_length,
               url);
    }

    esp_http_client_cleanup(client);
    return err;
}

static void disable_lora(void) {
#if CONFIG_TETHER_DISABLE_LORA
    const gpio_num_t outputs[] = {
        CONFIG_TETHER_LORA_NSS_GPIO, CONFIG_TETHER_LORA_SCK_GPIO,
        CONFIG_TETHER_LORA_MOSI_GPIO, CONFIG_TETHER_LORA_RESET_GPIO,
    };
    for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); ++i) {
        gpio_reset_pin(outputs[i]);
        gpio_set_direction(outputs[i], GPIO_MODE_OUTPUT);
        gpio_set_level(outputs[i], outputs[i] == CONFIG_TETHER_LORA_NSS_GPIO ? 1 : 0);
    }
#endif
}

static void configure_wakeup_sources(uint32_t sleep_sec) {
    if (CONFIG_TETHER_WAKE_BUTTON_GPIO >= 0) {
        const gpio_num_t button = CONFIG_TETHER_WAKE_BUTTON_GPIO;
        const int level = CONFIG_TETHER_WAKE_BUTTON_ACTIVE_LEVEL;
        ESP_ERROR_CHECK(rtc_gpio_deinit(button));
        ESP_ERROR_CHECK(rtc_gpio_init(button));
        ESP_ERROR_CHECK(rtc_gpio_set_direction(button, RTC_GPIO_MODE_INPUT_ONLY));
        if (level == 0) {
            ESP_ERROR_CHECK(rtc_gpio_pullup_en(button));
            ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(button));
        } else {
            ESP_ERROR_CHECK(rtc_gpio_pulldown_en(button));
            ESP_ERROR_CHECK(rtc_gpio_pullup_dis(button));
        }
        ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(button, level));
    }
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)sleep_sec * 1000000ULL));
}

static bool enter_idle_or_sleep(uint32_t sleep_sec)
{
#if CONFIG_TETHER_DEBUG_SOFT_RESET
    (void)sleep_sec;
    const uint32_t soft_wait_sec = DEBUG_SOFT_WAIT_SECONDS;
    LOGW_C("SOFT WAIT %u", (unsigned)soft_wait_sec);
    display_statusf("SOFT WAIT %u", (unsigned)soft_wait_sec);
    const TickType_t start = xTaskGetTickCount();
    TickType_t next_report = start + pdMS_TO_TICKS(1000);
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(soft_wait_sec * 1000U);
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        display_ui_update();
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - next_report) >= 0) {
            const uint32_t elapsed_sec = (uint32_t)((now - start) / configTICK_RATE_HZ);
            LOGI_C("SOFT WAIT %u/%u", (unsigned)elapsed_sec, (unsigned)soft_wait_sec);
            next_report += pdMS_TO_TICKS(1000);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    LOGW_C("RESTART");
    display_statusf("RESTART");
    esp_restart();
    return false;
#else
    const uint32_t wait_sec = PRE_SLEEP_WAIT_SECONDS;
    if (wait_sec > 0) {
        LOGW_C("WAIT %u", (unsigned)wait_sec);
        display_statusf("WAIT %u", (unsigned)wait_sec);
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(wait_sec * 1000U);
        while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
            display_ui_update();
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    configure_wakeup_sources(sleep_sec);
    esp_deep_sleep_start();
    return false;
#endif
}

static uint32_t compute_adjusted_sleep_seconds(void)
{
    const int64_t now_us = esp_timer_get_time();
    const uint32_t active_sec = (uint32_t)((now_us - app_start_us) / 1000000ULL);
    const uint32_t target_sleep = app_poll_seconds();

    if (active_sec + PRE_SLEEP_WAIT_SECONDS >= target_sleep) {
        return 1;
    }
    return target_sleep - active_sec - PRE_SLEEP_WAIT_SECONDS;
}

static bool wake_button_pressed(void)
{
#if CONFIG_TETHER_WAKE_BUTTON_GPIO >= 0
    return gpio_get_level(CONFIG_TETHER_WAKE_BUTTON_GPIO) == CONFIG_TETHER_WAKE_BUTTON_ACTIVE_LEVEL;
#else
    return false;
#endif
}

static void init_runtime_wake_button_input(void)
{
#if CONFIG_TETHER_WAKE_BUTTON_GPIO >= 0
    const gpio_num_t button = CONFIG_TETHER_WAKE_BUTTON_GPIO;
    gpio_reset_pin(button);
    gpio_set_direction(button, GPIO_MODE_INPUT);
    if (CONFIG_TETHER_WAKE_BUTTON_ACTIVE_LEVEL == 0) {
        gpio_pullup_en(button);
        gpio_pulldown_dis(button);
    } else {
        gpio_pulldown_en(button);
        gpio_pullup_dis(button);
    }
#endif
}

void app_main(void) {
    esp_log_level_set("*", ESP_LOG_INFO);
#if CONFIG_COMPILER_OPTIMIZATION_DEBUG
    esp_log_level_set(TAG, ESP_LOG_INFO);
    esp_log_level_set("ui", ESP_LOG_INFO);
    esp_log_level_set("lcd_panel.nv3041", ESP_LOG_INFO);
#endif
    ESP_LOGI(TAG, "boot log enabled");
    app_start_us = esp_timer_get_time();
    if (BOOT_DEBUG_HOLD_MS > 0) {
        LOGW_C("BOOT HOLD %u ms", (unsigned)BOOT_DEBUG_HOLD_MS);
        vTaskDelay(pdMS_TO_TICKS(BOOT_DEBUG_HOLD_MS));
    }
    disable_lora();
    disable_oled_power();
    init_status_led();
    init_buzzer();
    init_runtime_wake_button_input();

    if (boot_button_held_for_ap_mode()) {
        ESP_ERROR_CHECK(nvs_flash_init());
        (void)app_config_load(&s_app_cfg);
        display_ui_init();
        ap_mode_run();
    }

    ESP_ERROR_CHECK(nvs_flash_init());
    (void)app_config_load(&s_app_cfg);
    display_ui_init();
    {
        float cached_rate = 0.0f;
        if (usd_jpy_rate_cache_load(&cached_rate) == ESP_OK) {
            display_ui_set_usd_jpy_rate(cached_rate, true);
            display_statusf("USD/JPY cache %.2f", (double)cached_rate);
        } else {
            display_ui_set_usd_jpy_rate(0.0f, false);
        }
    }
    restore_time_from_rtc();
    LOGI_C("WAKE sleep=%u", (unsigned)app_poll_seconds());
    while (1) {
        display_statusf("WAKE sleep=%u", (unsigned)app_poll_seconds());

        display_ui_wifi_credentials_t wifi_creds = {0};
        bool wifi_ok = false;
        if (!wifi_ok) {
            bool have_creds = false;
            if (wifi_creds_load(&wifi_creds) == ESP_OK) {
                display_statusf("WIFI creds loaded");
                display_ui_set_log_connected_ssid(wifi_creds.ssid);
                have_creds = true;
            } else if (wifi_creds_from_config(&wifi_creds)) {
                display_statusf("WIFI creds from config");
                display_ui_set_log_connected_ssid(wifi_creds.ssid);
                have_creds = true;
            }

            if (have_creds) {
                buzzer_play_tone_ms(BUZZER_FREQ_TRY_HZ, BUZZER_TRY_TONE_MS);
                wifi_ok = (wifi_connect_with_credentials(&wifi_creds) == ESP_OK);
            }
        }

        if (wifi_ok) {
            buzzer_play_tone_ms(BUZZER_FREQ_OK_HZ, BUZZER_OK_TONE_MS);
            display_statusf("WIFI connected");
            if (time_sync_needed()) {
                display_statusf("NTP sync start");
                esp_err_t ntp_err = sync_time_via_ntp();
                if (ntp_err == ESP_OK) {
                    display_statusf("NTP sync OK");
                } else if (ntp_err == ESP_ERR_INVALID_STATE) {
                    LOGW_C("NTP sync already active");
                } else {
                    display_statusf("NTP sync FAIL %s", esp_err_to_name(ntp_err));
                }
            }
            int status = 0;
            float usd_jpy_rate = 0.0f;
            bool rate_valid = false;
            esp_err_t err = call_server(&status, &usd_jpy_rate, &rate_valid);
            if (rate_valid) {
                display_ui_set_usd_jpy_rate(usd_jpy_rate, true);
                if (usd_jpy_rate_cache_save(usd_jpy_rate) != ESP_OK) {
                    LOGW_C("USD/JPY cache save failed");
                }
            }
            if (err == ESP_OK && http_status_is_success(status)) {
                status_led_success();
                LOGS_C("OK sleep=%u http=%d", (unsigned)app_poll_seconds(), status);
                display_final_http_summary(true, status);
                display_statusf("HTTP %d OK", status);
            } else {
                buzzer_play_http_fail_pattern();
                status_led_failure();
                LOGE_C("HTTP_FAIL sleep=%u http=%d err=%s",
                       (unsigned)app_poll_seconds(),
                       status,
                       esp_err_to_name(err));
                display_final_http_summary(false, status);
                if (err == ESP_OK) {
                    display_statusf("HTTP %d", status);
                } else {
                    display_statusf("HTTP FAIL %s", esp_err_to_name(err));
                }
            }
        } else {
            buzzer_play_wifi_fail_pattern();
            status_led_failure();
            LOGE_C("WIFI_FAIL sleep=%u reason=%u", (unsigned)app_poll_seconds(), (unsigned)wifi_disconnect_reason);
            display_final_http_summary(false, -1);
            display_statusf("WIFI FAIL reason=%u", (unsigned)wifi_disconnect_reason);
        }
        const uint32_t sleep_sec = compute_adjusted_sleep_seconds();
        status_led_set(false);
        LOGW_C("SLEEP %u (target=%u)", (unsigned)sleep_sec, (unsigned)app_poll_seconds());
        display_statusf("SLEEP %u/%u", (unsigned)sleep_sec, (unsigned)app_poll_seconds());
        if (rtc_time_valid) {
            const uint32_t active_sec = (uint32_t)((esp_timer_get_time() - app_start_us) / 1000000ULL);
            rtc_elapsed_since_sync_sec += active_sec + PRE_SLEEP_WAIT_SECONDS + sleep_sec;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        if (enter_idle_or_sleep(sleep_sec)) {
            continue;
        }
    }
}
