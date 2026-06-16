#include "app_ui.h"
#include "esp_err.h"
#include "ftx_receiver.h"
#include "network_manager.h"
#include "nvs_flash.h"
#include "psk_reporter_config.h"
#include "psk_reporter.h"
#include "usb_net_driver.h"

void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(result);
    }

    psk_reporter_config_init();
    app_ui_init();
    network_manager_init();
    psk_reporter_start();
    usb_net_driver_start();
    ftx_receiver_start();
}
