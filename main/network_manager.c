#include "network_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_ui.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"

#define MAX_AP_COUNT 20

typedef struct {
    char ssid[33];
    char password[65];
} connection_request_t;

static const char *TAG = "network";
static wifi_ap_record_t s_records[MAX_AP_COUNT];
static uint16_t s_record_count;

static void ntp_sync_callback(struct timeval *value)
{
    struct tm utc;
    gmtime_r(&value->tv_sec, &utc);

    char synced_at[40];
    strftime(synced_at, sizeof(synced_at), "%Y-%m-%d %H:%M:%S UTC", &utc);
    ESP_LOGI(TAG, "NTP time synchronized: %s", synced_at);
    app_ui_set_time_status("时间: NTP 已同步");
    app_ui_set_receiver_status("NTP 时间同步完成.");
}

static void start_ntp(void)
{
    if (esp_sntp_enabled()) {
        return;
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CONFIG_FTX_NTP_SERVER);
    esp_sntp_set_time_sync_notification_cb(ntp_sync_callback);
    esp_sntp_init();
    app_ui_set_time_status("时间来源: 正在同步 NTP");
    app_ui_set_receiver_status("正在同步 NTP 时间.");
}

static void event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        app_ui_set_wifi_network_status("Wi-Fi 离线");
        app_ui_set_wifi_status("网络已断开, 请重新选择.");
    } else if (base == IP_EVENT && event_id == IP_EVENT_ETH_LOST_IP) {
        app_ui_set_wired_network_status("有线离线");
    } else if (base == IP_EVENT &&
               (event_id == IP_EVENT_STA_GOT_IP || event_id == IP_EVENT_ETH_GOT_IP)) {
        const ip_event_got_ip_t *event = event_data;
        char status[64];
        snprintf(status, sizeof(status), "%s 已连接: " IPSTR,
                 event_id == IP_EVENT_ETH_GOT_IP ? "USB" : "Wi-Fi", IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "%s", status);
        if (event_id == IP_EVENT_STA_GOT_IP) {
            app_ui_set_wifi_network_status("Wi-Fi 在线");
            app_ui_set_wifi_status(status);
        } else {
            app_ui_set_wired_network_status("有线在线");
        }
        start_ntp();
    }
}

static void scan_task(void *arg)
{
    app_ui_set_wifi_status("正在扫描 Wi-Fi 网络.");
    esp_err_t result = esp_wifi_scan_start(NULL, true);
    s_record_count = MAX_AP_COUNT;
    if (result == ESP_OK) {
        result = esp_wifi_scan_get_ap_records(&s_record_count, s_records);
    }
    if (result != ESP_OK) {
        s_record_count = 0;
        app_ui_set_wifi_status("扫描失败.");
    } else {
        app_ui_set_wifi_status(s_record_count ? "请从列表中选择网络." : "没有发现网络.");
    }
    app_ui_update_networks();
    vTaskDelete(NULL);
}

static void connect_task(void *arg)
{
    connection_request_t *request = arg;
    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, request->ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, request->password, sizeof(config.sta.password));
    free(request);

    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    if (esp_wifi_connect() != ESP_OK) {
        app_ui_set_wifi_status("连接启动失败.");
    }
    vTaskDelete(NULL);
}

void network_manager_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_config_t saved = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &saved) == ESP_OK && saved.sta.ssid[0] != '\0') {
        app_ui_set_wifi_status("正在连接已保存网络.");
        esp_wifi_connect();
    }
}

void network_manager_scan(void)
{
    xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 4, NULL);
}

void network_manager_connect(const char *ssid, const char *password)
{
    connection_request_t *request = calloc(1, sizeof(*request));
    if (request == NULL) {
        app_ui_set_wifi_status("内存不足.");
        return;
    }
    strlcpy(request->ssid, ssid, sizeof(request->ssid));
    strlcpy(request->password, password, sizeof(request->password));
    app_ui_set_wifi_status("正在连接.");
    xTaskCreate(connect_task, "wifi_connect", 4096, request, 5, NULL);
}

const wifi_ap_record_t *network_manager_get_records(uint16_t *count)
{
    *count = s_record_count;
    return s_records;
}
