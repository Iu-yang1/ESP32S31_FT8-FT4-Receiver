#include "psk_reporter.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "psk_reporter_config.h"

#define MAX_REPORTS 32
#define MAX_PACKET_SIZE 1400
#define SEND_INTERVAL_MS (300000 + (esp_random() % 30000))

typedef struct {
    char callsign[16];
    char mode[5];
    uint32_t frequency_hz;
    uint32_t received_at;
    int8_t snr;
} reception_report_t;

typedef struct {
    uint8_t data[MAX_PACKET_SIZE];
    size_t length;
} packet_t;

static const char *TAG = "psk_reporter";
static reception_report_t s_reports[MAX_REPORTS];
static size_t s_report_count;
static SemaphoreHandle_t s_lock;
static uint32_t s_sequence;
static uint32_t s_session_id;
static unsigned s_packets_sent;
static int s_socket_fd = -1;
static char s_connected_server[64];
static uint16_t s_connected_port;
static TaskHandle_t s_sender_task;

static const uint8_t RECEIVER_TEMPLATE[] = {
    0x00, 0x03, 0x00, 0x2C, 0x99, 0x92, 0x00, 0x04, 0x00, 0x01,
    0x80, 0x02, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
    0x80, 0x04, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
    0x80, 0x08, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
    0x80, 0x09, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
    0x00, 0x00,
};

static const uint8_t SENDER_TEMPLATE[] = {
    0x00, 0x02, 0x00, 0x3C, 0x99, 0x93, 0x00, 0x07,
    0x80, 0x01, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
    0x80, 0x05, 0x00, 0x04, 0x00, 0x00, 0x76, 0x8F,
    0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8F,
    0x80, 0x07, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8F,
    0x80, 0x0A, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
    0x80, 0x0B, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8F,
    0x00, 0x96, 0x00, 0x04,
};

static bool put_u8(packet_t *packet, uint8_t value)
{
    if (packet->length + 1 > sizeof(packet->data)) {
        return false;
    }
    packet->data[packet->length++] = value;
    return true;
}

static bool put_u16(packet_t *packet, uint16_t value)
{
    return put_u8(packet, value >> 8) && put_u8(packet, value);
}

static bool put_u32(packet_t *packet, uint32_t value)
{
    return put_u16(packet, value >> 16) && put_u16(packet, value);
}

static bool put_bytes(packet_t *packet, const void *data, size_t length)
{
    if (packet->length + length > sizeof(packet->data)) {
        return false;
    }
    memcpy(packet->data + packet->length, data, length);
    packet->length += length;
    return true;
}

static bool put_string(packet_t *packet, const char *text)
{
    size_t length = strnlen(text, 254);
    return put_u8(packet, (uint8_t)length) && put_bytes(packet, text, length);
}

static void patch_u16(packet_t *packet, size_t offset, uint16_t value)
{
    packet->data[offset] = value >> 8;
    packet->data[offset + 1] = value;
}

static void pad_four(packet_t *packet)
{
    while ((packet->length & 3U) != 0) {
        put_u8(packet, 0);
    }
}

static bool build_packet(packet_t *packet, const reception_report_t *reports, size_t count)
{
    const psk_reporter_config_t *config = psk_reporter_config_get();
    packet->length = 0;
    put_u16(packet, 10);
    size_t total_length_offset = packet->length;
    put_u16(packet, 0);
    put_u32(packet, (uint32_t)time(NULL));
    put_u32(packet, s_sequence);
    put_u32(packet, s_session_id);

    bool include_templates = s_packets_sent < 3 || (s_packets_sent % 12) == 0;
    if (include_templates) {
        put_bytes(packet, RECEIVER_TEMPLATE, sizeof(RECEIVER_TEMPLATE));
        put_bytes(packet, SENDER_TEMPLATE, sizeof(SENDER_TEMPLATE));
    }

    put_u16(packet, 0x9992);
    size_t receiver_length_offset = packet->length;
    size_t receiver_start = packet->length - 2;
    put_u16(packet, 0);
    put_string(packet, config->callsign);
    put_string(packet, config->grid);
    put_string(packet, "ESP32-S31 FT8 Receiver");
    put_string(packet, config->antenna);
    pad_four(packet);
    patch_u16(packet, receiver_length_offset, (uint16_t)(packet->length - receiver_start));

    put_u16(packet, 0x9993);
    size_t sender_length_offset = packet->length;
    size_t sender_start = packet->length - 2;
    put_u16(packet, 0);
    for (size_t i = 0; i < count; ++i) {
        put_string(packet, reports[i].callsign);
        put_u32(packet, reports[i].frequency_hz);
        put_u8(packet, (uint8_t)reports[i].snr);
        put_u8(packet, 0);
        put_string(packet, reports[i].mode);
        put_u8(packet, 1);
        put_u32(packet, reports[i].received_at);
    }
    pad_four(packet);
    patch_u16(packet, sender_length_offset, (uint16_t)(packet->length - sender_start));
    patch_u16(packet, total_length_offset, (uint16_t)packet->length);
    return packet->length <= MAX_PACKET_SIZE;
}

static void close_socket(void)
{
    if (s_socket_fd >= 0) {
        close(s_socket_fd);
        s_socket_fd = -1;
    }
    s_connected_server[0] = '\0';
    s_connected_port = 0;
}

static bool ensure_socket(const psk_reporter_config_t *config)
{
    if (s_socket_fd >= 0 && strcmp(s_connected_server, config->server) == 0 &&
        s_connected_port == config->port) {
        return true;
    }

    close_socket();

    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM};
    struct addrinfo *address = NULL;
    char port[6];
    snprintf(port, sizeof(port), "%u", config->port);
    if (getaddrinfo(config->server, port, &hints, &address) != 0 || address == NULL) {
        ESP_LOGW(TAG, "Unable to resolve %s", config->server);
        return false;
    }

    s_socket_fd = socket(address->ai_family, SOCK_DGRAM, IPPROTO_UDP);
    bool connected = s_socket_fd >= 0 &&
                     connect(s_socket_fd, address->ai_addr, address->ai_addrlen) == 0;
    freeaddrinfo(address);
    if (!connected) {
        ESP_LOGW(TAG, "Unable to open UDP connection to %s:%u", config->server, config->port);
        close_socket();
        return false;
    }

    strlcpy(s_connected_server, config->server, sizeof(s_connected_server));
    s_connected_port = config->port;
    return true;
}

static void send_pending(void)
{
    reception_report_t reports[MAX_REPORTS];
    size_t count;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    count = s_report_count;
    memcpy(reports, s_reports, count * sizeof(reports[0]));
    xSemaphoreGive(s_lock);

    const psk_reporter_config_t *config = psk_reporter_config_get();
    if (!config->enabled || config->callsign[0] == '\0' || count == 0) {
        return;
    }

    packet_t packet;
    if (!build_packet(&packet, reports, count)) {
        ESP_LOGE(TAG, "IPFIX packet overflow");
        return;
    }

    int sent = ensure_socket(config) ? send(s_socket_fd, packet.data, packet.length, 0) : -1;
    if (sent == (int)packet.length) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_report_count > count) {
            memmove(s_reports, s_reports + count, (s_report_count - count) * sizeof(s_reports[0]));
        }
        s_report_count -= count;
        xSemaphoreGive(s_lock);
        s_sequence += count;
        ++s_packets_sent;
        ESP_LOGI(TAG, "Sent %u IPFIX reception report(s)", (unsigned)count);
    } else {
        ESP_LOGW(TAG, "IPFIX UDP send failed, reports retained for retry");
        close_socket();
    }
}

static void sender_task(void *arg)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SEND_INTERVAL_MS));
        send_pending();
    }
}

void psk_reporter_start(void)
{
    s_session_id = esp_random();
    s_lock = xSemaphoreCreateMutex();
    xTaskCreate(sender_task, "psk_reporter", 6144, NULL, 3, &s_sender_task);
}

void psk_reporter_queue(const char *sender_callsign, uint32_t frequency_hz, int8_t snr,
                        const char *mode, time_t received_at)
{
    const psk_reporter_config_t *config = psk_reporter_config_get();
    if (!config->enabled || sender_callsign == NULL || sender_callsign[0] == '\0' ||
        sender_callsign[0] == '<') {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_report_count; ++i) {
        if (strcmp(s_reports[i].callsign, sender_callsign) == 0 &&
            strcmp(s_reports[i].mode, mode) == 0) {
            xSemaphoreGive(s_lock);
            return;
        }
    }
    if (s_report_count < MAX_REPORTS) {
        reception_report_t *report = &s_reports[s_report_count++];
        strlcpy(report->callsign, sender_callsign, sizeof(report->callsign));
        strlcpy(report->mode, mode, sizeof(report->mode));
        report->frequency_hz = frequency_hz;
        report->snr = snr;
        report->received_at = (uint32_t)received_at;
        if (s_report_count == MAX_REPORTS && s_sender_task != NULL) {
            xTaskNotifyGive(s_sender_task);
        }
    }
    xSemaphoreGive(s_lock);
}
