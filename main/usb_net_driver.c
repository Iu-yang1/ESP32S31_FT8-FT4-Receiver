#include "usb_net_driver.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_ui.h"
#include "bsp/esp32_s31_korvo_1.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/esp_netif_net_stack.h"
#include "lwip/inet.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

#define RNDIS_CONTROL_BUFFER_SIZE 1024
#define RNDIS_BULK_BUFFER_SIZE 16384
#define RNDIS_PACKET_HEADER_SIZE 44
#define RNDIS_TX_QUEUE_LENGTH 8

#define RNDIS_MSG_PACKET 0x00000001
#define RNDIS_MSG_INITIALIZE 0x00000002
#define RNDIS_MSG_QUERY 0x00000004
#define RNDIS_MSG_SET 0x00000005
#define RNDIS_MSG_INITIALIZE_CMPLT 0x80000002
#define RNDIS_MSG_QUERY_CMPLT 0x80000004
#define RNDIS_MSG_SET_CMPLT 0x80000005

#define RNDIS_STATUS_SUCCESS 0x00000000
#define RNDIS_OID_GEN_CURRENT_PACKET_FILTER 0x0001010E
#define RNDIS_OID_802_3_CURRENT_ADDRESS 0x01010102
#define RNDIS_PACKET_FILTER 0x0000000F

#define CDC_SEND_ENCAPSULATED_COMMAND 0x00
#define CDC_GET_ENCAPSULATED_RESPONSE 0x01

typedef struct {
    uint8_t *data;
    size_t length;
} tx_packet_t;

typedef struct {
    esp_netif_driver_base_t base;
    usb_host_client_handle_t client;
    usb_device_handle_t device;
    usb_transfer_t *rx_transfer;
    usb_transfer_t *tx_transfer;
    QueueHandle_t tx_queue;
    uint8_t control_interface;
    uint8_t data_interface;
    uint8_t bulk_in;
    uint8_t bulk_out;
    uint16_t bulk_in_mps;
    uint16_t control_mps;
    uint32_t request_id;
    uint8_t mac[6];
    bool ready;
    bool tx_busy;
    bool control_done;
    usb_transfer_status_t control_status;
} rndis_driver_t;

static const char *TAG = "usb_rndis";
static rndis_driver_t s_driver;
static volatile uint8_t s_new_address;
static volatile bool s_device_gone;

static uint32_t read_u32(const uint8_t *data)
{
    uint32_t value;
    memcpy(&value, data, sizeof(value));
    return value;
}

static void write_u32(uint8_t *data, uint32_t value)
{
    memcpy(data, &value, sizeof(value));
}

static void event_callback(const usb_host_client_event_msg_t *event, void *arg)
{
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_new_address = event->new_dev.address;
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        s_device_gone = true;
    }
}

static void control_callback(usb_transfer_t *transfer)
{
    rndis_driver_t *driver = transfer->context;
    driver->control_status = transfer->status;
    driver->control_done = true;
}

static esp_err_t control_transfer(rndis_driver_t *driver, bool input, uint8_t request,
                                  const void *out_data, size_t out_length,
                                  void *in_data, size_t in_capacity, size_t *in_length)
{
    size_t requested = input ? in_capacity : out_length;
    size_t rounded = input ? usb_round_up_to_mps(requested, driver->control_mps) : requested;
    usb_transfer_t *transfer;
    ESP_RETURN_ON_ERROR(usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + rounded, 0, &transfer),
                        TAG, "control transfer allocation failed");

    usb_setup_packet_t *setup = (usb_setup_packet_t *)transfer->data_buffer;
    setup->bmRequestType = (input ? USB_BM_REQUEST_TYPE_DIR_IN : USB_BM_REQUEST_TYPE_DIR_OUT) |
                           USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    setup->bRequest = request;
    setup->wValue = 0;
    setup->wIndex = driver->control_interface;
    setup->wLength = requested;
    if (!input && out_length > 0) {
        memcpy(transfer->data_buffer + sizeof(*setup), out_data, out_length);
    }

    transfer->num_bytes = sizeof(*setup) + rounded;
    transfer->device_handle = driver->device;
    transfer->callback = control_callback;
    transfer->context = driver;
    driver->control_done = false;
    esp_err_t result = usb_host_transfer_submit_control(driver->client, transfer);
    if (result == ESP_OK) {
        int waited_ms = 0;
        while (!driver->control_done && !s_device_gone && waited_ms < 3000) {
            usb_host_client_handle_events(driver->client, pdMS_TO_TICKS(10));
            waited_ms += 10;
        }
        if (!driver->control_done || driver->control_status != USB_TRANSFER_STATUS_COMPLETED) {
            result = ESP_ERR_TIMEOUT;
        } else if (input) {
            size_t actual = transfer->actual_num_bytes > (int)sizeof(*setup)
                                ? transfer->actual_num_bytes - sizeof(*setup)
                                : 0;
            actual = actual > in_capacity ? in_capacity : actual;
            memcpy(in_data, transfer->data_buffer + sizeof(*setup), actual);
            if (in_length != NULL) {
                *in_length = actual;
            }
        }
    }
    usb_host_transfer_free(transfer);
    return result;
}

static esp_err_t rndis_command(rndis_driver_t *driver, const void *command, size_t command_length,
                               uint32_t expected_type, uint8_t *response, size_t capacity,
                               size_t *response_length)
{
    ESP_RETURN_ON_ERROR(control_transfer(driver, false, CDC_SEND_ENCAPSULATED_COMMAND,
                                         command, command_length, NULL, 0, NULL),
                        TAG, "RNDIS command send failed");
    for (int attempt = 0; attempt < 20; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(20));
        size_t length = 0;
        if (control_transfer(driver, true, CDC_GET_ENCAPSULATED_RESPONSE, NULL, 0,
                             response, capacity, &length) != ESP_OK) {
            continue;
        }
        if (length >= 16 && read_u32(response) == expected_type) {
            if (response_length != NULL) {
                *response_length = length;
            }
            return read_u32(response + 12) == RNDIS_STATUS_SUCCESS ? ESP_OK : ESP_FAIL;
        }
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t rndis_initialize(rndis_driver_t *driver)
{
    uint8_t command[24] = {0};
    uint8_t response[RNDIS_CONTROL_BUFFER_SIZE];
    write_u32(command, RNDIS_MSG_INITIALIZE);
    write_u32(command + 4, sizeof(command));
    write_u32(command + 8, ++driver->request_id);
    write_u32(command + 12, 1);
    write_u32(command + 16, 0);
    write_u32(command + 20, RNDIS_BULK_BUFFER_SIZE);
    ESP_RETURN_ON_ERROR(rndis_command(driver, command, sizeof(command), RNDIS_MSG_INITIALIZE_CMPLT,
                                      response, sizeof(response), NULL),
                        TAG, "RNDIS initialize failed");
    ESP_LOGI(TAG, "RNDIS initialized, device max transfer=%lu", (unsigned long)read_u32(response + 36));
    return ESP_OK;
}

static esp_err_t rndis_query(rndis_driver_t *driver, uint32_t oid, void *value, size_t capacity,
                             size_t *value_length)
{
    uint8_t command[28] = {0};
    uint8_t response[RNDIS_CONTROL_BUFFER_SIZE];
    size_t response_length;
    write_u32(command, RNDIS_MSG_QUERY);
    write_u32(command + 4, sizeof(command));
    write_u32(command + 8, ++driver->request_id);
    write_u32(command + 12, oid);
    ESP_RETURN_ON_ERROR(rndis_command(driver, command, sizeof(command), RNDIS_MSG_QUERY_CMPLT,
                                      response, sizeof(response), &response_length),
                        TAG, "RNDIS query failed");
    uint32_t length = read_u32(response + 16);
    uint32_t offset = read_u32(response + 20);
    size_t start = 8 + offset;
    ESP_RETURN_ON_FALSE(start <= response_length && length <= response_length - start && length <= capacity,
                        ESP_ERR_INVALID_RESPONSE, TAG, "invalid RNDIS query response");
    memcpy(value, response + start, length);
    if (value_length != NULL) {
        *value_length = length;
    }
    return ESP_OK;
}

static esp_err_t rndis_set(rndis_driver_t *driver, uint32_t oid, const void *value, size_t length)
{
    ESP_RETURN_ON_FALSE(length <= RNDIS_CONTROL_BUFFER_SIZE - 28, ESP_ERR_INVALID_SIZE, TAG,
                        "RNDIS set value too large");
    uint8_t command[RNDIS_CONTROL_BUFFER_SIZE] = {0};
    uint8_t response[RNDIS_CONTROL_BUFFER_SIZE];
    write_u32(command, RNDIS_MSG_SET);
    write_u32(command + 4, 28 + length);
    write_u32(command + 8, ++driver->request_id);
    write_u32(command + 12, oid);
    write_u32(command + 16, length);
    write_u32(command + 20, 20);
    memcpy(command + 28, value, length);
    return rndis_command(driver, command, 28 + length, RNDIS_MSG_SET_CMPLT,
                         response, sizeof(response), NULL);
}

static void free_rx_buffer(void *handle, void *buffer)
{
    free(buffer);
}

static esp_err_t netif_transmit(void *handle, void *buffer, size_t length)
{
    rndis_driver_t *driver = handle;
    if (!driver->ready || length > 1600) {
        return ESP_ERR_INVALID_STATE;
    }
    tx_packet_t packet = {
        .data = malloc(length),
        .length = length,
    };
    if (packet.data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(packet.data, buffer, length);
    if (xQueueSend(driver->tx_queue, &packet, 0) != pdTRUE) {
        free(packet.data);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void process_rndis_packets(rndis_driver_t *driver, const uint8_t *data, size_t total)
{
    size_t position = 0;
    while (total - position >= RNDIS_PACKET_HEADER_SIZE) {
        const uint8_t *message = data + position;
        uint32_t message_length = read_u32(message + 4);
        if (read_u32(message) != RNDIS_MSG_PACKET ||
            message_length < RNDIS_PACKET_HEADER_SIZE || message_length > total - position) {
            ESP_LOGW(TAG, "Invalid RNDIS packet message");
            return;
        }
        uint32_t data_offset = read_u32(message + 8);
        uint32_t data_length = read_u32(message + 12);
        size_t payload = position + 8 + data_offset;
        if (payload <= total && data_length <= total - payload && data_length >= 14) {
            uint8_t *frame = malloc(data_length);
            if (frame != NULL) {
                memcpy(frame, data + payload, data_length);
                if (esp_netif_receive(driver->base.netif, frame, data_length, NULL) != ESP_OK) {
                    free(frame);
                }
            }
        }
        position += message_length;
    }
}

static void rx_callback(usb_transfer_t *transfer)
{
    rndis_driver_t *driver = transfer->context;
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED && transfer->actual_num_bytes > 0) {
        process_rndis_packets(driver, transfer->data_buffer, transfer->actual_num_bytes);
    }
    if (driver->ready && !s_device_gone) {
        transfer->num_bytes = RNDIS_BULK_BUFFER_SIZE;
        if (usb_host_transfer_submit(transfer) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to resubmit RNDIS RX transfer");
        }
    }
}

static void tx_callback(usb_transfer_t *transfer)
{
    rndis_driver_t *driver = transfer->context;
    driver->tx_busy = false;
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "RNDIS TX failed, status=%d", transfer->status);
    }
}

static void submit_next_tx(rndis_driver_t *driver)
{
    if (!driver->ready || driver->tx_busy) {
        return;
    }
    tx_packet_t packet;
    if (xQueueReceive(driver->tx_queue, &packet, 0) != pdTRUE) {
        return;
    }
    size_t total = RNDIS_PACKET_HEADER_SIZE + packet.length;
    memset(driver->tx_transfer->data_buffer, 0, RNDIS_PACKET_HEADER_SIZE);
    write_u32(driver->tx_transfer->data_buffer, RNDIS_MSG_PACKET);
    write_u32(driver->tx_transfer->data_buffer + 4, total);
    write_u32(driver->tx_transfer->data_buffer + 8, 36);
    write_u32(driver->tx_transfer->data_buffer + 12, packet.length);
    memcpy(driver->tx_transfer->data_buffer + RNDIS_PACKET_HEADER_SIZE, packet.data, packet.length);
    free(packet.data);

    driver->tx_transfer->num_bytes = total;
    driver->tx_busy = true;
    if (usb_host_transfer_submit(driver->tx_transfer) != ESP_OK) {
        driver->tx_busy = false;
        ESP_LOGW(TAG, "Failed to submit RNDIS TX transfer");
    }
}

static esp_err_t netif_post_attach(esp_netif_t *netif, void *handle)
{
    rndis_driver_t *driver = handle;
    driver->base.netif = netif;
    esp_netif_driver_ifconfig_t config = {
        .handle = driver,
        .transmit = netif_transmit,
        .driver_free_rx_buffer = free_rx_buffer,
    };
    return esp_netif_set_driver_config(netif, &config);
}

static esp_err_t create_netif(rndis_driver_t *driver)
{
    static const esp_netif_inherent_config_t base_config = {
        .flags = ESP_NETIF_DHCP_CLIENT | ESP_NETIF_FLAG_AUTOUP | ESP_NETIF_FLAG_EVENT_IP_MODIFIED,
        .get_ip_event = IP_EVENT_ETH_GOT_IP,
        .lost_ip_event = IP_EVENT_ETH_LOST_IP,
        .if_key = "USB_RNDIS",
        .if_desc = "usb-rndis",
        .route_prio = 120,
        .mtu = 1500,
    };
    static const struct esp_netif_netstack_config stack_config = {
        .lwip = {
            .init_fn = ethernetif_init,
            .input_fn = ethernetif_input,
        },
    };
    driver->base.post_attach = netif_post_attach;
    esp_netif_config_t config = {
        .base = &base_config,
        .stack = &stack_config,
    };
    esp_netif_t *netif = esp_netif_new(&config);
    ESP_RETURN_ON_FALSE(netif != NULL, ESP_ERR_NO_MEM, TAG, "esp_netif creation failed");
    ESP_RETURN_ON_ERROR(esp_netif_attach(netif, driver), TAG, "esp_netif attach failed");
    ESP_RETURN_ON_ERROR(esp_netif_set_mac(netif, driver->mac), TAG, "esp_netif MAC setup failed");
    esp_netif_action_start(netif, NULL, 0, NULL);
    esp_netif_action_connected(netif, NULL, 0, NULL);
    return ESP_OK;
}

static bool find_rndis_interfaces(rndis_driver_t *driver, const usb_config_desc_t *config)
{
    int offset = 0;
    const usb_standard_desc_t *descriptor = (const usb_standard_desc_t *)config;
    while ((descriptor = usb_parse_next_descriptor(descriptor, config->wTotalLength, &offset)) != NULL) {
        if (descriptor->bDescriptorType != USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            continue;
        }
        const usb_intf_desc_t *interface = (const usb_intf_desc_t *)descriptor;
        ESP_LOGI(TAG, "Interface %u alt=%u class=%02x subclass=%02x protocol=%02x endpoints=%u",
                 interface->bInterfaceNumber, interface->bAlternateSetting, interface->bInterfaceClass,
                 interface->bInterfaceSubClass, interface->bInterfaceProtocol, interface->bNumEndpoints);
        if (interface->bInterfaceClass == 0xE0 && interface->bInterfaceSubClass == 0x01 &&
            interface->bInterfaceProtocol == 0x03) {
            driver->control_interface = interface->bInterfaceNumber;
        } else if (interface->bInterfaceClass == 0x0A && interface->bNumEndpoints >= 2) {
            driver->data_interface = interface->bInterfaceNumber;
            for (int i = 0; i < interface->bNumEndpoints; ++i) {
                int endpoint_offset = offset;
                const usb_ep_desc_t *endpoint =
                    usb_parse_endpoint_descriptor_by_index(interface, i, config->wTotalLength, &endpoint_offset);
                if (endpoint == NULL || (endpoint->bmAttributes & 0x03) != 0x02) {
                    continue;
                }
                ESP_LOGI(TAG, "Bulk endpoint 0x%02x MPS=%u", endpoint->bEndpointAddress,
                         endpoint->wMaxPacketSize);
                if ((endpoint->bEndpointAddress & 0x80) != 0) {
                    driver->bulk_in = endpoint->bEndpointAddress;
                    driver->bulk_in_mps = endpoint->wMaxPacketSize;
                } else {
                    driver->bulk_out = endpoint->bEndpointAddress;
                }
            }
        }
    }
    return driver->bulk_in != 0 && driver->bulk_out != 0;
}

static esp_err_t start_rndis_device(rndis_driver_t *driver, uint8_t address)
{
    ESP_RETURN_ON_ERROR(usb_host_device_open(driver->client, address, &driver->device), TAG, "device open failed");
    const usb_device_desc_t *device;
    const usb_config_desc_t *config;
    ESP_RETURN_ON_ERROR(usb_host_get_device_descriptor(driver->device, &device), TAG, "device descriptor failed");
    ESP_RETURN_ON_ERROR(usb_host_get_active_config_descriptor(driver->device, &config), TAG, "config descriptor failed");
    driver->control_mps = device->bMaxPacketSize0;
    ESP_RETURN_ON_FALSE(find_rndis_interfaces(driver, config), ESP_ERR_NOT_SUPPORTED, TAG,
                        "RNDIS interfaces not found");
    ESP_RETURN_ON_ERROR(usb_host_interface_claim(driver->client, driver->device, driver->control_interface, 0),
                        TAG, "control interface claim failed");
    ESP_RETURN_ON_ERROR(usb_host_interface_claim(driver->client, driver->device, driver->data_interface, 0),
                        TAG, "data interface claim failed");
    ESP_RETURN_ON_ERROR(rndis_initialize(driver), TAG, "RNDIS initialization failed");

    size_t mac_length;
    ESP_RETURN_ON_ERROR(rndis_query(driver, RNDIS_OID_802_3_CURRENT_ADDRESS,
                                    driver->mac, sizeof(driver->mac), &mac_length),
                        TAG, "MAC query failed");
    ESP_RETURN_ON_FALSE(mac_length == sizeof(driver->mac), ESP_ERR_INVALID_RESPONSE, TAG, "invalid MAC length");
    uint32_t filter = RNDIS_PACKET_FILTER;
    ESP_RETURN_ON_ERROR(rndis_set(driver, RNDIS_OID_GEN_CURRENT_PACKET_FILTER, &filter, sizeof(filter)),
                        TAG, "packet filter setup failed");
    ESP_LOGI(TAG, "RNDIS MAC %02x:%02x:%02x:%02x:%02x:%02x", driver->mac[0], driver->mac[1],
             driver->mac[2], driver->mac[3], driver->mac[4], driver->mac[5]);

    ESP_RETURN_ON_ERROR(usb_host_transfer_alloc(RNDIS_BULK_BUFFER_SIZE, 0, &driver->rx_transfer),
                        TAG, "RX transfer allocation failed");
    ESP_RETURN_ON_ERROR(usb_host_transfer_alloc(2048, 0, &driver->tx_transfer),
                        TAG, "TX transfer allocation failed");
    driver->rx_transfer->device_handle = driver->device;
    driver->rx_transfer->bEndpointAddress = driver->bulk_in;
    driver->rx_transfer->num_bytes = usb_round_up_to_mps(RNDIS_BULK_BUFFER_SIZE, driver->bulk_in_mps);
    driver->rx_transfer->callback = rx_callback;
    driver->rx_transfer->context = driver;
    driver->tx_transfer->device_handle = driver->device;
    driver->tx_transfer->bEndpointAddress = driver->bulk_out;
    driver->tx_transfer->flags = USB_TRANSFER_FLAG_ZERO_PACK;
    driver->tx_transfer->callback = tx_callback;
    driver->tx_transfer->context = driver;
    driver->ready = true;
    ESP_RETURN_ON_ERROR(usb_host_transfer_submit(driver->rx_transfer), TAG, "RX submit failed");
    ESP_RETURN_ON_ERROR(create_netif(driver), TAG, "network interface setup failed");
    app_ui_set_usb_status("RNDIS 已初始化, 正在通过 DHCP 获取网络配置.");
    return ESP_OK;
}

static void got_ip_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    const ip_event_got_ip_t *event = event_data;
    if (event->esp_netif != s_driver.base.netif) {
        return;
    }
    esp_netif_dns_info_t dns = {0};
    esp_netif_get_dns_info(event->esp_netif, ESP_NETIF_DNS_MAIN, &dns);
    char status[128];
    snprintf(status, sizeof(status), "USB IP:" IPSTR " GW:" IPSTR " DNS:" IPSTR,
             IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw), IP2STR(&dns.ip.u_addr.ip4));
    ESP_LOGI(TAG, "%s", status);
    app_ui_set_usb_status(status);
    app_ui_set_wired_network_status("有线在线");
}

static void driver_task(void *arg)
{
    rndis_driver_t *driver = &s_driver;
    driver->tx_queue = xQueueCreate(RNDIS_TX_QUEUE_LENGTH, sizeof(tx_packet_t));
    if (driver->tx_queue == NULL ||
        bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true) != ESP_OK) {
        app_ui_set_usb_status("USB Host 启动失败.");
        vTaskDelete(NULL);
    }
    usb_host_client_config_t config = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {.client_event_callback = event_callback, .callback_arg = NULL},
    };
    if (usb_host_client_register(&config, &driver->client) != ESP_OK) {
        app_ui_set_usb_status("USB RNDIS 驱动注册失败.");
        vTaskDelete(NULL);
    }
    app_ui_set_usb_status("USB Host 已启动, 等待 RNDIS 手机网络.");
    while (true) {
        usb_host_client_handle_events(driver->client, pdMS_TO_TICKS(10));
        submit_next_tx(driver);
        if (s_new_address != 0) {
            uint8_t address = s_new_address;
            s_new_address = 0;
            esp_err_t result = start_rndis_device(driver, address);
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "RNDIS startup failed: %s", esp_err_to_name(result));
                app_ui_set_usb_status("RNDIS 初始化失败, 请重新打开手机 USB 网络共享.");
            }
        }
        if (s_device_gone) {
            s_device_gone = false;
            driver->ready = false;
            driver->tx_busy = false;
            if (driver->base.netif != NULL) {
                esp_netif_action_disconnected(driver->base.netif, NULL, 0, NULL);
            }
            app_ui_set_usb_status("USB 网络设备已断开.");
            app_ui_set_wired_network_status("有线离线");
        }
    }
}

void usb_net_driver_start(void)
{
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_handler, NULL));
    xTaskCreate(driver_task, "usb_rndis", 12288, NULL, 5, NULL);
}
