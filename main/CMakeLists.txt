idf_component_register(
    SRCS "main.c" "app_ui.c" "network_manager.c" "ftx_receiver.c"
         "psk_reporter_config.c" "psk_reporter.c" "usb_net_driver.c" "ui_font_cn.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif esp_timer nvs_flash lwip esp_lcd ft8_lib
)
