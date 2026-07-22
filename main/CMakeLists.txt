idf_component_register(SRCS "main.c" "display_ui.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_event esp_netif esp_http_client esp_http_server esp_timer nvs_flash driver mbedtls esp_lcd esp_lcd_nv3041a)
