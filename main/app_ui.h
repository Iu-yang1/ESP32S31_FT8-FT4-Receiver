#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ft8/decode.h"

void app_ui_init(void);
void app_ui_set_wired_network_status(const char *text);
void app_ui_set_wifi_network_status(const char *text);
void app_ui_set_wifi_status(const char *text);
void app_ui_set_receiver_status(const char *text);
void app_ui_set_usb_status(const char *text);
void app_ui_set_time_status(const char *text);
void app_ui_add_decode(const char *mode, const char *utc, float snr, float frequency, const char *message);
void app_ui_update_waterfall(const uint8_t *magnitudes, size_t count);
void app_ui_update_networks(void);
ftx_protocol_t app_ui_get_protocol(void);
