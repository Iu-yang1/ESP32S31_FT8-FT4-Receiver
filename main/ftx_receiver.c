#include "ftx_receiver.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app_ui.h"
#include "bsp/esp32_s31_korvo_1.h"
#include "common/monitor.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ft8/message.h"
#include "psk_reporter.h"

#define AUDIO_SAMPLE_RATE 16000
#define MAX_CANDIDATES 140
#define MAX_DECODED_MESSAGES 32

static const char *TAG = "ftx_receiver";
static volatile ftx_protocol_t s_protocol = FTX_PROTOCOL_FT8;
static volatile ftx_decode_strategy_t s_strategy = FTX_DECODE_FAST;
static QueueHandle_t s_decode_queue;

typedef struct {
    ftx_waterfall_t waterfall;
    int min_bin;
    float symbol_period;
    float max_mag;
    time_t slot_start;
} decode_job_t;

static int64_t synchronized_time_us(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    if (now.tv_sec >= 1735689600) {
        return (int64_t)now.tv_sec * 1000000LL + now.tv_usec;
    }
    return esp_timer_get_time();
}

static bool message_sender_callsign(const ftx_message_t *message, char *callsign, size_t size)
{
    char call_to[16];
    char call_de[16];
    char extra[20];
    ftx_field_t fields[FTX_MAX_MESSAGE_FIELDS];
    ftx_message_type_t type = ftx_message_get_type(message);
    ftx_message_rc_t result = FTX_MESSAGE_RC_ERROR_TYPE;
    if (type == FTX_MESSAGE_TYPE_STANDARD) {
        result = ftx_message_decode_std(message, NULL, call_to, call_de, extra, fields);
    } else if (type == FTX_MESSAGE_TYPE_NONSTD_CALL) {
        result = ftx_message_decode_nonstd(message, NULL, call_to, call_de, extra, fields);
    }
    if (result != FTX_MESSAGE_RC_OK || fields[1] != FTX_FIELD_CALL || call_de[0] == '<') {
        return false;
    }
    strlcpy(callsign, call_de, size);
    return true;
}

static void publish_decode(const decode_job_t *job, const ftx_candidate_t *candidate,
                           const ftx_message_t *message, const char *utc_text)
{
    char text[FTX_MAX_MESSAGE_LENGTH];
    ftx_message_offsets_t offsets;
    if (ftx_message_decode(message, NULL, text, &offsets) != FTX_MESSAGE_RC_OK) {
        strlcpy(text, "消息解包失败", sizeof(text));
    }
    float frequency = (job->min_bin + candidate->freq_offset +
                       (float)candidate->freq_sub / job->waterfall.freq_osr) /
                      job->symbol_period;
    const char *mode = job->waterfall.protocol == FTX_PROTOCOL_FT8 ? "FT8" : "FT4";
    float snr = candidate->score * 0.5f;
    app_ui_add_decode(mode, utc_text, snr, frequency, text);

    char sender[16];
    if (message_sender_callsign(message, sender, sizeof(sender))) {
        psk_reporter_queue(sender, CONFIG_FTX_DIAL_FREQUENCY_HZ + (uint32_t)frequency,
                           (int8_t)lroundf(snr), mode, job->slot_start);
    }
}

static void decode_job(const decode_job_t *job)
{
    static const struct {
        int candidates;
        int min_score;
        int iterations;
    } passes[] = {
        {48, 14, 12},
        {90, 10, 22},
        {140, 6, 36},
    };
    uint16_t decoded_hashes[MAX_DECODED_MESSAGES] = {0};
    int decoded_count = 0;
    struct tm utc;
    gmtime_r(&job->slot_start, &utc);
    char utc_text[16];
    strftime(utc_text, sizeof(utc_text), "%H:%M:%S", &utc);
    double slot_period = job->waterfall.protocol == FTX_PROTOCOL_FT8 ? FT8_SLOT_TIME : FT4_SLOT_TIME;
    int64_t started = esp_timer_get_time();
    int64_t deadline = started + (int64_t)(slot_period * 500000.0);
    int pass_count = s_strategy == FTX_DECODE_MULTI_PASS ? 3 : 1;
    bool timed_out = false;

    for (int pass = 0; pass < pass_count && decoded_count < MAX_DECODED_MESSAGES; ++pass) {
        if (esp_timer_get_time() >= deadline) {
            timed_out = true;
            break;
        }
        ftx_candidate_t candidates[MAX_CANDIDATES];
        int count = ftx_find_candidates(&job->waterfall, passes[pass].candidates, candidates,
                                        passes[pass].min_score);
        ESP_LOGI(TAG, "%s pass=%d candidates=%d", job->waterfall.protocol == FTX_PROTOCOL_FT8 ? "FT8" : "FT4",
                 pass + 1, count);

        for (int i = 0; i < count && decoded_count < MAX_DECODED_MESSAGES; ++i) {
            if (esp_timer_get_time() >= deadline) {
                timed_out = true;
                break;
            }
            ftx_message_t message;
            ftx_decode_status_t status;
            if (!ftx_decode_candidate(&job->waterfall, &candidates[i], passes[pass].iterations,
                                      &message, &status)) {
                continue;
            }
            bool duplicate = false;
            for (int j = 0; j < decoded_count; ++j) {
                if (decoded_hashes[j] == message.hash) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                decoded_hashes[decoded_count++] = message.hash;
                publish_decode(job, &candidates[i], &message, utc_text);
            }
        }
    }
    char status[128];
    snprintf(status, sizeof(status), "%s 解码完成: %d 条, 耗时 %lld ms%s",
             job->waterfall.protocol == FTX_PROTOCOL_FT8 ? "FT8" : "FT4", decoded_count,
             (long long)((esp_timer_get_time() - started) / 1000), timed_out ? ", 已到半槽截止" : "");
    app_ui_set_receiver_status(status);
}

static void decoder_task(void *arg)
{
    decode_job_t *job;
    while (xQueueReceive(s_decode_queue, &job, portMAX_DELAY) == pdTRUE) {
        decode_job(job);
        free(job->waterfall.mag);
        free(job);
    }
}

static void queue_decode(const monitor_t *monitor, time_t slot_start)
{
    decode_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM);
    size_t size = monitor->wf.max_blocks * monitor->wf.block_stride * sizeof(WF_ELEM_T);
    WF_ELEM_T *snapshot = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (job == NULL || snapshot == NULL) {
        free(snapshot);
        free(job);
        app_ui_set_receiver_status("解码任务内存不足.");
        return;
    }
    job->waterfall = monitor->wf;
    job->waterfall.mag = snapshot;
    memcpy(snapshot, monitor->wf.mag, size);
    job->min_bin = monitor->min_bin;
    job->symbol_period = monitor->symbol_period;
    job->max_mag = monitor->max_mag;
    job->slot_start = slot_start;
    if (xQueueSend(s_decode_queue, &job, 0) != pdTRUE) {
        free(job->waterfall.mag);
        free(job);
        app_ui_set_receiver_status("上一槽仍在解码, 已跳过本槽.");
    }
}

static esp_codec_dev_handle_t microphone_init(void)
{
    i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
        },
    };
    ESP_ERROR_CHECK(bsp_audio_init(&i2s_config));

    esp_codec_dev_handle_t microphone = bsp_audio_codec_microphone_init();
    esp_codec_dev_sample_info_t format = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    if (microphone == NULL || esp_codec_dev_open(microphone, &format) != ESP_CODEC_DEV_OK) {
        return NULL;
    }
    esp_codec_dev_set_in_gain(microphone, 36.0f);
    return microphone;
}

static void receiver_task(void *arg)
{
    esp_codec_dev_handle_t microphone = microphone_init();
    if (microphone == NULL) {
        app_ui_set_receiver_status("麦克风初始化失败.");
        vTaskDelete(NULL);
    }
    app_ui_set_receiver_status("麦克风已启动, 正在等待完整时隙.");

    while (true) {
        ftx_protocol_t protocol = s_protocol;
        monitor_config_t config = {
            .f_min = 200.0f,
            .f_max = 3000.0f,
            .sample_rate = AUDIO_SAMPLE_RATE,
            .time_osr = 1,
            .freq_osr = 1,
            .protocol = protocol,
        };
        monitor_t monitor;
        monitor_init(&monitor, &config);

        int16_t *pcm = heap_caps_malloc(monitor.block_size * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        float *samples = heap_caps_malloc(monitor.block_size * sizeof(float), MALLOC_CAP_SPIRAM);
        if (pcm == NULL || samples == NULL) {
            app_ui_set_receiver_status("内存不足.");
            free(pcm);
            free(samples);
            monitor_free(&monitor);
            vTaskDelete(NULL);
        }

        double slot_period = protocol == FTX_PROTOCOL_FT8 ? FT8_SLOT_TIME : FT4_SLOT_TIME;
        int64_t slot_us = (int64_t)(slot_period * 1000000.0);
        int64_t current_slot = synchronized_time_us() / slot_us;
        time_t slot_start = time(NULL);

        while (s_protocol == protocol) {
            if (esp_codec_dev_read(microphone, pcm, monitor.block_size * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
                app_ui_set_receiver_status("麦克风读取失败.");
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            for (int i = 0; i < monitor.block_size; ++i) {
                samples[i] = pcm[i] / 32768.0f;
            }
            monitor_process(&monitor, samples);
            if (monitor.wf.num_blocks > 0) {
                const uint8_t *latest = monitor.wf.mag +
                                        (monitor.wf.num_blocks - 1) * monitor.wf.block_stride;
                app_ui_update_waterfall(latest, monitor.wf.num_bins);
            }

            int64_t new_slot = synchronized_time_us() / slot_us;
            if (new_slot != current_slot) {
                if (monitor.wf.num_blocks > 20) {
                    queue_decode(&monitor, slot_start);
                }
                monitor_reset(&monitor);
                current_slot = new_slot;
                slot_start = time(NULL);
            }
        }
        free(pcm);
        free(samples);
        monitor_free(&monitor);
    }
}

void ftx_receiver_start(void)
{
    s_decode_queue = xQueueCreate(1, sizeof(decode_job_t *));
    xTaskCreate(decoder_task, "ftx_decoder", 16384, NULL, 4, NULL);
    xTaskCreate(receiver_task, "ftx_receiver", 49152, NULL, 5, NULL);
}

void ftx_receiver_set_protocol(ftx_protocol_t protocol)
{
    s_protocol = protocol;
}

ftx_protocol_t ftx_receiver_get_protocol(void)
{
    return s_protocol;
}

void ftx_receiver_set_decode_strategy(ftx_decode_strategy_t strategy)
{
    s_strategy = strategy;
}

ftx_decode_strategy_t ftx_receiver_get_decode_strategy(void)
{
    return s_strategy;
}
