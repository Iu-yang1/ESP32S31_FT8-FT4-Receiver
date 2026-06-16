#pragma once

#include <stdint.h>

#include "esp_wifi.h"

void network_manager_init(void);
void network_manager_scan(void);
void network_manager_connect(const char *ssid, const char *password);
const wifi_ap_record_t *network_manager_get_records(uint16_t *count);
