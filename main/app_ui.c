#include "app_ui.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "bsp/esp32_s31_korvo_1.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "ftx_receiver.h"
#include "network_manager.h"
#include "psk_reporter_config.h"
#include "ui_font_cn.h"

#define WATERFALL_WIDTH 270
#define WATERFALL_HEIGHT 140

static lv_obj_t *s_wifi_page;
static lv_obj_t *s_network_list;
static lv_obj_t *s_password_area;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_time_dialog;
static lv_obj_t *s_time_area;
static lv_obj_t *s_time_status_label;
static lv_obj_t *s_psk_dialog;
static lv_obj_t *s_psk_enabled;
static lv_obj_t *s_psk_callsign;
static lv_obj_t *s_psk_grid;
static lv_obj_t *s_psk_antenna;
static lv_obj_t *s_psk_server;
static lv_obj_t *s_psk_port;
static lv_obj_t *s_psk_status_label;
static lv_obj_t *s_wifi_status_label;
static lv_obj_t *s_selected_label;
static lv_obj_t *s_clock_label;
static lv_obj_t *s_time_source_label;
static lv_obj_t *s_wired_network_badge;
static lv_obj_t *s_wifi_network_badge;
static lv_obj_t *s_mode_label;
static lv_obj_t *s_strategy_label;
static lv_obj_t *s_slot_bar;
static lv_obj_t *s_slot_label;
static lv_obj_t *s_receiver_status;
static lv_obj_t *s_decode_list;
static lv_obj_t *s_waterfall_canvas;
static lv_obj_t *s_usb_status_label;
static uint16_t *s_waterfall_buffer;
static char s_selected_ssid[33];

static lv_style_t s_card_style;
static lv_style_t s_button_style;

static void set_label(lv_obj_t *label, const char *text)
{
    if (label != NULL && bsp_display_lock(1000)) {
        lv_label_set_text(label, text);
        bsp_display_unlock();
    }
}

static lv_obj_t *make_card(lv_obj_t *parent, int width, int height, int x, int y)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, width, height);
    lv_obj_set_pos(card, x, y);
    lv_obj_add_style(card, &s_card_style, 0);
    return card;
}

static void style_button(lv_obj_t *button)
{
    lv_obj_add_style(button, &s_button_style, 0);
}

static void show_wifi_page(bool show)
{
    if (!bsp_display_lock(1000)) {
        return;
    }
    if (show) {
        lv_obj_remove_flag(s_wifi_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_wifi_page);
    } else {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wifi_page, LV_OBJ_FLAG_HIDDEN);
    }
    bsp_display_unlock();
}

static void show_wifi_event(lv_event_t *event)
{
    show_wifi_page(true);
}

static void close_wifi_event(lv_event_t *event)
{
    show_wifi_page(false);
}

static void scan_event(lv_event_t *event)
{
    network_manager_scan();
}

static void network_event(lv_event_t *event)
{
    const char *ssid = lv_event_get_user_data(event);
    strlcpy(s_selected_ssid, ssid, sizeof(s_selected_ssid));
    lv_textarea_set_text(s_password_area, "");
    lv_label_set_text_fmt(s_selected_label, "已选择网络:\n%s", s_selected_ssid);
    lv_keyboard_set_textarea(s_keyboard, s_password_area);
    lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_keyboard);
}

static void connect_event(lv_event_t *event)
{
    if (s_selected_ssid[0] == '\0') {
        lv_label_set_text(s_wifi_status_label, "请先选择网络.");
        return;
    }
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    network_manager_connect(s_selected_ssid, lv_textarea_get_text(s_password_area));
}

static void password_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(s_keyboard, s_password_area);
        lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_keyboard);
    }
}

static void text_input_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(s_keyboard, lv_event_get_target(event));
        lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_keyboard);
    }
}

static void time_area_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(s_keyboard, s_time_area);
        lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_keyboard);
    }
}

static void show_time_event(lv_event_t *event)
{
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);

    char text[24];
    if (utc.tm_year + 1900 >= 2025) {
        strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &utc);
    } else {
        strlcpy(text, "2026-01-01 00:00:00", sizeof(text));
    }
    lv_textarea_set_text(s_time_area, text);
    lv_label_set_text(s_time_status_label, "输入 UTC 时间后确认.");
    lv_obj_remove_flag(s_time_dialog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_time_dialog);
}

static void close_time_event(lv_event_t *event)
{
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_time_dialog, LV_OBJ_FLAG_HIDDEN);
}

static void show_psk_event(lv_event_t *event)
{
    const psk_reporter_config_t *config = psk_reporter_config_get();
    if (config->enabled) {
        lv_obj_add_state(s_psk_enabled, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(s_psk_enabled, LV_STATE_CHECKED);
    }
    lv_textarea_set_text(s_psk_callsign, config->callsign);
    lv_textarea_set_text(s_psk_grid, config->grid);
    lv_textarea_set_text(s_psk_antenna, config->antenna);
    lv_textarea_set_text(s_psk_server, config->server);
    char port_text[6];
    snprintf(port_text, sizeof(port_text), "%u", config->port);
    lv_textarea_set_text(s_psk_port, port_text);
    lv_label_set_text(s_psk_status_label, "配置已保存.");
    lv_obj_remove_flag(s_psk_dialog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_psk_dialog);
}

static void close_psk_event(lv_event_t *event)
{
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_psk_dialog, LV_OBJ_FLAG_HIDDEN);
}

static void save_psk_event(lv_event_t *event)
{
    long port = strtol(lv_textarea_get_text(s_psk_port), NULL, 10);
    if (port < 1 || port > 65535) {
        lv_label_set_text(s_psk_status_label, "端口无效, 请输入 1 到 65535.");
        return;
    }

    psk_reporter_config_t config = {
        .enabled = lv_obj_has_state(s_psk_enabled, LV_STATE_CHECKED),
        .port = (uint16_t)port,
    };
    strlcpy(config.callsign, lv_textarea_get_text(s_psk_callsign), sizeof(config.callsign));
    strlcpy(config.grid, lv_textarea_get_text(s_psk_grid), sizeof(config.grid));
    strlcpy(config.antenna, lv_textarea_get_text(s_psk_antenna), sizeof(config.antenna));
    strlcpy(config.server, lv_textarea_get_text(s_psk_server), sizeof(config.server));

    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_psk_status_label,
                      psk_reporter_config_save(&config) ? "PSK Reporter 配置已保存." : "配置保存失败.");
}

static void apply_time_event(lv_event_t *event)
{
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    const char *text = lv_textarea_get_text(s_time_area);
    if (sscanf(text, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6 ||
        year < 2025 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        lv_label_set_text(s_time_status_label, "时间格式或数值无效.");
        return;
    }

    struct tm utc = {
        .tm_year = year - 1900,
        .tm_mon = month - 1,
        .tm_mday = day,
        .tm_hour = hour,
        .tm_min = minute,
        .tm_sec = second,
        .tm_isdst = 0,
    };
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t timestamp = mktime(&utc);
    if (timestamp < 0 || utc.tm_year != year - 1900 || utc.tm_mon != month - 1 || utc.tm_mday != day) {
        lv_label_set_text(s_time_status_label, "日历日期无效.");
        return;
    }

    struct timeval value = {.tv_sec = timestamp, .tv_usec = 0};
    if (settimeofday(&value, NULL) != 0) {
        lv_label_set_text(s_time_status_label, "系统时间设置失败.");
        return;
    }
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_time_status_label, "UTC 时间已更新.");
    lv_label_set_text(s_wifi_status_label, "系统 UTC 时间已手动设置.");
    lv_label_set_text(s_time_source_label, "时间: 手动设置");
}

static void keyboard_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void mode_event(lv_event_t *event)
{
    ftx_protocol_t next = ftx_receiver_get_protocol() == FTX_PROTOCOL_FT8
                              ? FTX_PROTOCOL_FT4
                              : FTX_PROTOCOL_FT8;
    ftx_receiver_set_protocol(next);
    lv_label_set_text(s_mode_label, next == FTX_PROTOCOL_FT8 ? "FT8" : "FT4");
    lv_label_set_text(s_receiver_status, "模式将在下一音频块切换.");
}

static void strategy_event(lv_event_t *event)
{
    ftx_decode_strategy_t next = ftx_receiver_get_decode_strategy() == FTX_DECODE_FAST
                                     ? FTX_DECODE_MULTI_PASS
                                     : FTX_DECODE_FAST;
    ftx_receiver_set_decode_strategy(next);
    lv_label_set_text(s_strategy_label, next == FTX_DECODE_FAST ? "快速" : "多次");
    lv_label_set_text(s_receiver_status,
                      next == FTX_DECODE_FAST ? "已选择快速解码." : "已选择多次解码.");
}

static void ui_timer_event(lv_timer_t *timer)
{
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);

    char clock_text[40];
    if (utc.tm_year + 1900 >= 2025) {
        strftime(clock_text, sizeof(clock_text), "%Y-%m-%d %H:%M:%S UTC", &utc);
    } else {
        strlcpy(clock_text, "时间未同步", sizeof(clock_text));
    }
    lv_label_set_text(s_clock_label, clock_text);

    double period = ftx_receiver_get_protocol() == FTX_PROTOCOL_FT8 ? FT8_SLOT_TIME : FT4_SLOT_TIME;
    double seconds = utc.tm_year + 1900 >= 2025 ? (double)now : (double)esp_timer_get_time() / 1000000.0;
    double within_slot = fmod(seconds, period);
    lv_bar_set_value(s_slot_bar, (int)(within_slot * 1000.0 / period), LV_ANIM_OFF);
    int tenths = (int)lround((period - within_slot) * 10.0);
    lv_label_set_text_fmt(s_slot_label, "下次解码: %d.%d s", tenths / 10, tenths % 10);
}

static void create_dashboard(lv_obj_t *screen)
{
    lv_obj_t *dashboard = lv_obj_create(screen);
    lv_obj_remove_style_all(dashboard);
    lv_obj_set_size(dashboard, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(dashboard, lv_color_hex(0xFFF9FC), 0);
    lv_obj_set_style_bg_opa(dashboard, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(dashboard);
    lv_label_set_text(title, "解码");
    lv_obj_set_style_text_color(title, lv_color_hex(0x488DD8), 0);
    lv_obj_set_pos(title, 20, 18);

    s_clock_label = lv_label_create(dashboard);
    lv_label_set_text(s_clock_label, "时间未同步");
    lv_obj_set_style_text_color(s_clock_label, lv_color_hex(0x526374), 0);
    lv_obj_align(s_clock_label, LV_ALIGN_TOP_MID, 0, 9);

    s_time_source_label = lv_label_create(dashboard);
    lv_label_set_text(s_time_source_label, "时间: 等待网络");
    lv_obj_set_style_text_color(s_time_source_label, lv_color_hex(0x7B91A4), 0);
    lv_obj_align(s_time_source_label, LV_ALIGN_TOP_MID, 0, 34);

    s_wired_network_badge = lv_label_create(dashboard);
    lv_obj_set_width(s_wired_network_badge, 80);
    lv_label_set_text(s_wired_network_badge, "有线离线");
    lv_obj_set_style_text_align(s_wired_network_badge, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_wired_network_badge, lv_color_hex(0x718397), 0);
    lv_obj_align(s_wired_network_badge, LV_ALIGN_TOP_RIGHT, -195, 20);

    s_wifi_network_badge = lv_label_create(dashboard);
    lv_obj_set_width(s_wifi_network_badge, 80);
    lv_label_set_text(s_wifi_network_badge, "Wi-Fi 离线");
    lv_obj_set_style_text_align(s_wifi_network_badge, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_wifi_network_badge, lv_color_hex(0x718397), 0);
    lv_obj_align(s_wifi_network_badge, LV_ALIGN_TOP_RIGHT, -105, 20);

    lv_obj_t *wifi_button = lv_button_create(dashboard);
    lv_obj_set_size(wifi_button, 82, 38);
    lv_obj_align(wifi_button, LV_ALIGN_TOP_RIGHT, -16, 10);
    style_button(wifi_button);
    lv_obj_add_event_cb(wifi_button, show_wifi_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *wifi_label = lv_label_create(wifi_button);
    lv_label_set_text(wifi_label, LV_SYMBOL_WIFI);
    lv_obj_center(wifi_label);

    lv_obj_t *waterfall_card = make_card(dashboard, 300, 225, 18, 66);
    lv_obj_t *waterfall_title = lv_label_create(waterfall_card);
    lv_label_set_text(waterfall_title, "实时音频频谱");
    lv_obj_set_style_text_color(waterfall_title, lv_color_hex(0x488DD8), 0);

    s_waterfall_canvas = lv_canvas_create(waterfall_card);
    lv_canvas_set_buffer(s_waterfall_canvas, s_waterfall_buffer, WATERFALL_WIDTH, WATERFALL_HEIGHT,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_waterfall_canvas, 0, 28);
    lv_obj_set_style_border_color(s_waterfall_canvas, lv_color_hex(0xC9E7FA), 0);
    lv_obj_set_style_border_width(s_waterfall_canvas, 1, 0);
    lv_obj_set_style_radius(s_waterfall_canvas, 8, 0);

    lv_obj_t *axis = lv_label_create(waterfall_card);
    lv_label_set_text(axis, "100 Hz               1.5 kHz               3 kHz");
    lv_obj_set_style_text_color(axis, lv_color_hex(0x8092A5), 0);
    lv_obj_set_pos(axis, 0, 172);

    lv_obj_t *decoder_card = make_card(dashboard, 300, 145, 18, 305);
    lv_obj_t *decoder_title = lv_label_create(decoder_card);
    lv_label_set_text(decoder_title, "解码器");
    lv_obj_set_style_text_color(decoder_title, lv_color_hex(0xD96B9E), 0);

    lv_obj_t *mode_button = lv_button_create(decoder_card);
    lv_obj_set_size(mode_button, 64, 42);
    lv_obj_align(mode_button, LV_ALIGN_TOP_RIGHT, 0, 0);
    style_button(mode_button);
    lv_obj_add_event_cb(mode_button, mode_event, LV_EVENT_CLICKED, NULL);
    s_mode_label = lv_label_create(mode_button);
    lv_label_set_text(s_mode_label, "FT8");
    lv_obj_center(s_mode_label);

    lv_obj_t *strategy_button = lv_button_create(decoder_card);
    lv_obj_set_size(strategy_button, 76, 42);
    lv_obj_align(strategy_button, LV_ALIGN_TOP_RIGHT, -72, 0);
    style_button(strategy_button);
    lv_obj_add_event_cb(strategy_button, strategy_event, LV_EVENT_CLICKED, NULL);
    s_strategy_label = lv_label_create(strategy_button);
    lv_label_set_text(s_strategy_label, "快速");
    lv_obj_center(s_strategy_label);

    s_slot_bar = lv_bar_create(decoder_card);
    lv_obj_set_size(s_slot_bar, 190, 12);
    lv_obj_set_pos(s_slot_bar, 0, 48);
    lv_bar_set_range(s_slot_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_slot_bar, lv_color_hex(0xE4F4FF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slot_bar, lv_color_hex(0xF39AC1), LV_PART_INDICATOR);

    s_slot_label = lv_label_create(decoder_card);
    lv_label_set_text(s_slot_label, "等待完整时隙.");
    lv_obj_set_style_text_color(s_slot_label, lv_color_hex(0x526374), 0);
    lv_obj_set_pos(s_slot_label, 0, 65);

    s_receiver_status = lv_label_create(decoder_card);
    lv_obj_set_width(s_receiver_status, 265);
    lv_label_set_long_mode(s_receiver_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_receiver_status, "正在启动麦克风.");
    lv_obj_set_style_text_color(s_receiver_status, lv_color_hex(0x718397), 0);
    lv_obj_set_pos(s_receiver_status, 0, 86);

    lv_obj_t *decode_card = make_card(dashboard, 447, 384, 335, 66);
    lv_obj_t *decode_title = lv_label_create(decode_card);
    lv_label_set_text(decode_title, "最近解码");
    lv_obj_set_style_text_color(decode_title, lv_color_hex(0x488DD8), 0);

    s_decode_list = lv_list_create(decode_card);
    lv_obj_set_size(s_decode_list, 415, 330);
    lv_obj_set_pos(s_decode_list, 0, 30);
    lv_obj_set_style_bg_color(s_decode_list, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_decode_list, 0, 0);
    lv_list_add_text(s_decode_list, "正在监听 FT8.");
}

static void create_wifi_page(lv_obj_t *screen)
{
    s_wifi_page = lv_obj_create(screen);
    lv_obj_set_size(s_wifi_page, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_wifi_page, lv_color_hex(0xFFF9FC), 0);
    lv_obj_set_style_pad_all(s_wifi_page, 12, 0);

    lv_obj_t *title = lv_label_create(s_wifi_page);
    lv_label_set_text(title, "网络与时间设置");
    lv_obj_set_style_text_color(title, lv_color_hex(0x488DD8), 0);

    lv_obj_t *close_button = lv_button_create(s_wifi_page);
    lv_obj_set_size(close_button, 90, 42);
    lv_obj_align(close_button, LV_ALIGN_TOP_RIGHT, 0, 0);
    style_button(close_button);
    lv_obj_add_event_cb(close_button, close_wifi_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = lv_label_create(close_button);
    lv_label_set_text(close_label, "返回");
    lv_obj_center(close_label);

    lv_obj_t *scan_button = lv_button_create(s_wifi_page);
    lv_obj_set_size(scan_button, 150, 42);
    lv_obj_align(scan_button, LV_ALIGN_TOP_RIGHT, -105, 0);
    style_button(scan_button);
    lv_obj_add_event_cb(scan_button, scan_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scan_label = lv_label_create(scan_button);
    lv_label_set_text(scan_label, LV_SYMBOL_REFRESH " 扫描 Wi-Fi");
    lv_obj_center(scan_label);

    lv_obj_t *time_button = lv_button_create(s_wifi_page);
    lv_obj_set_size(time_button, 105, 42);
    lv_obj_align(time_button, LV_ALIGN_TOP_RIGHT, -270, 0);
    style_button(time_button);
    lv_obj_add_event_cb(time_button, show_time_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *time_label = lv_label_create(time_button);
    lv_label_set_text(time_label, "设置 UTC");
    lv_obj_center(time_label);

    lv_obj_t *psk_button = lv_button_create(s_wifi_page);
    lv_obj_set_size(psk_button, 110, 42);
    lv_obj_align(psk_button, LV_ALIGN_TOP_RIGHT, -390, 0);
    style_button(psk_button);
    lv_obj_add_event_cb(psk_button, show_psk_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *psk_label = lv_label_create(psk_button);
    lv_label_set_text(psk_label, "PSK 配置");
    lv_obj_center(psk_label);

    s_wifi_status_label = lv_label_create(s_wifi_page);
    lv_label_set_text(s_wifi_status_label, "请选择 Wi-Fi 网络或手机热点.");
    lv_obj_set_style_text_color(s_wifi_status_label, lv_color_hex(0x526374), 0);
    lv_obj_set_pos(s_wifi_status_label, 0, 42);

    s_network_list = lv_list_create(s_wifi_page);
    lv_obj_set_size(s_network_list, 455, 382);
    lv_obj_align(s_network_list, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_network_list, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t *credentials = make_card(s_wifi_page, 300, 194, 475, 65);
    s_selected_label = lv_label_create(credentials);
    lv_obj_set_width(s_selected_label, 270);
    lv_label_set_long_mode(s_selected_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_selected_label, "已选择网络:\n无");

    s_password_area = lv_textarea_create(credentials);
    lv_obj_set_size(s_password_area, 270, 48);
    lv_obj_set_pos(s_password_area, 0, 52);
    lv_textarea_set_one_line(s_password_area, true);
    lv_textarea_set_password_mode(s_password_area, true);
    lv_textarea_set_placeholder_text(s_password_area, "Wi-Fi 密码");
    lv_obj_add_event_cb(s_password_area, password_event, LV_EVENT_ALL, NULL);

    lv_obj_t *connect_button = lv_button_create(credentials);
    lv_obj_set_size(connect_button, 270, 44);
    lv_obj_set_pos(connect_button, 0, 112);
    style_button(connect_button);
    lv_obj_add_event_cb(connect_button, connect_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *connect_label = lv_label_create(connect_button);
    lv_label_set_text(connect_label, LV_SYMBOL_WIFI " 连接");
    lv_obj_center(connect_label);

    lv_obj_t *phone_card = make_card(s_wifi_page, 300, 170, 475, 277);
    lv_obj_t *phone_title = lv_label_create(phone_card);
    lv_label_set_text(phone_title, "USB 网卡状态");
    lv_obj_set_style_text_color(phone_title, lv_color_hex(0xD96B9E), 0);
    lv_obj_t *phone_text = lv_label_create(phone_card);
    lv_obj_set_width(phone_text, 270);
    lv_label_set_long_mode(phone_text, LV_LABEL_LONG_WRAP);
    lv_label_set_text(phone_text, "可识别 RNDIS, CDC-ECM 和 CDC-NCM 网络设备.");
    lv_obj_set_style_text_color(phone_text, lv_color_hex(0x526374), 0);
    lv_obj_set_pos(phone_text, 0, 30);
    s_usb_status_label = lv_label_create(phone_card);
    lv_obj_set_width(s_usb_status_label, 270);
    lv_label_set_long_mode(s_usb_status_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_usb_status_label, "USB Host 尚未启动.");
    lv_obj_set_style_text_color(s_usb_status_label, lv_color_hex(0x526374), 0);
    lv_obj_set_pos(s_usb_status_label, 0, 78);

    s_keyboard = lv_keyboard_create(screen);
    lv_obj_set_size(s_keyboard, BSP_LCD_H_RES, 210);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(s_keyboard, keyboard_event, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

    s_time_dialog = lv_obj_create(screen);
    lv_obj_set_size(s_time_dialog, 500, 210);
    lv_obj_center(s_time_dialog);
    lv_obj_add_style(s_time_dialog, &s_card_style, 0);

    lv_obj_t *time_title = lv_label_create(s_time_dialog);
    lv_label_set_text(time_title, "手动设置 UTC 时间");
    lv_obj_set_style_text_color(time_title, lv_color_hex(0x488DD8), 0);

    s_time_area = lv_textarea_create(s_time_dialog);
    lv_obj_set_size(s_time_area, 466, 48);
    lv_obj_set_pos(s_time_area, 0, 34);
    lv_textarea_set_one_line(s_time_area, true);
    lv_textarea_set_max_length(s_time_area, 19);
    lv_textarea_set_accepted_chars(s_time_area, "0123456789-: ");
    lv_textarea_set_placeholder_text(s_time_area, "YYYY-MM-DD HH:MM:SS");
    lv_obj_add_event_cb(s_time_area, time_area_event, LV_EVENT_ALL, NULL);

    s_time_status_label = lv_label_create(s_time_dialog);
    lv_label_set_text(s_time_status_label, "输入 UTC 时间后确认.");
    lv_obj_set_style_text_color(s_time_status_label, lv_color_hex(0x526374), 0);
    lv_obj_set_pos(s_time_status_label, 0, 91);

    lv_obj_t *apply_button = lv_button_create(s_time_dialog);
    lv_obj_set_size(apply_button, 220, 44);
    lv_obj_set_pos(apply_button, 0, 125);
    style_button(apply_button);
    lv_obj_add_event_cb(apply_button, apply_time_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *apply_label = lv_label_create(apply_button);
    lv_label_set_text(apply_label, "确认设置");
    lv_obj_center(apply_label);

    lv_obj_t *cancel_button = lv_button_create(s_time_dialog);
    lv_obj_set_size(cancel_button, 220, 44);
    lv_obj_set_pos(cancel_button, 246, 125);
    style_button(cancel_button);
    lv_obj_add_event_cb(cancel_button, close_time_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel_button);
    lv_label_set_text(cancel_label, "取消");
    lv_obj_center(cancel_label);

    lv_obj_add_flag(s_time_dialog, LV_OBJ_FLAG_HIDDEN);

    s_psk_dialog = lv_obj_create(screen);
    lv_obj_set_size(s_psk_dialog, 620, 390);
    lv_obj_center(s_psk_dialog);
    lv_obj_add_style(s_psk_dialog, &s_card_style, 0);

    lv_obj_t *psk_title = lv_label_create(s_psk_dialog);
    lv_label_set_text(psk_title, "PSKReporter 配置");
    lv_obj_set_style_text_color(psk_title, lv_color_hex(0x488DD8), 0);

    s_psk_enabled = lv_checkbox_create(s_psk_dialog);
    lv_checkbox_set_text(s_psk_enabled, "启用上传");
    lv_obj_set_pos(s_psk_enabled, 430, 0);

    lv_obj_t *callsign_label = lv_label_create(s_psk_dialog);
    lv_label_set_text(callsign_label, "接收站呼号");
    lv_obj_set_pos(callsign_label, 0, 38);
    s_psk_callsign = lv_textarea_create(s_psk_dialog);
    lv_obj_set_size(s_psk_callsign, 280, 42);
    lv_obj_set_pos(s_psk_callsign, 0, 60);
    lv_textarea_set_one_line(s_psk_callsign, true);
    lv_textarea_set_max_length(s_psk_callsign, 15);
    lv_obj_add_event_cb(s_psk_callsign, text_input_event, LV_EVENT_ALL, NULL);

    lv_obj_t *grid_label = lv_label_create(s_psk_dialog);
    lv_label_set_text(grid_label, "网格坐标");
    lv_obj_set_pos(grid_label, 310, 38);
    s_psk_grid = lv_textarea_create(s_psk_dialog);
    lv_obj_set_size(s_psk_grid, 280, 42);
    lv_obj_set_pos(s_psk_grid, 310, 60);
    lv_textarea_set_one_line(s_psk_grid, true);
    lv_textarea_set_max_length(s_psk_grid, 8);
    lv_obj_add_event_cb(s_psk_grid, text_input_event, LV_EVENT_ALL, NULL);

    lv_obj_t *antenna_label = lv_label_create(s_psk_dialog);
    lv_label_set_text(antenna_label, "备注");
    lv_obj_set_pos(antenna_label, 0, 112);
    s_psk_antenna = lv_textarea_create(s_psk_dialog);
    lv_obj_set_size(s_psk_antenna, 590, 42);
    lv_obj_set_pos(s_psk_antenna, 0, 134);
    lv_textarea_set_one_line(s_psk_antenna, true);
    lv_textarea_set_max_length(s_psk_antenna, 47);
    lv_obj_add_event_cb(s_psk_antenna, text_input_event, LV_EVENT_ALL, NULL);

    lv_obj_t *server_label = lv_label_create(s_psk_dialog);
    lv_label_set_text(server_label, "服务器");
    lv_obj_set_pos(server_label, 0, 186);
    s_psk_server = lv_textarea_create(s_psk_dialog);
    lv_obj_set_size(s_psk_server, 430, 42);
    lv_obj_set_pos(s_psk_server, 0, 208);
    lv_textarea_set_one_line(s_psk_server, true);
    lv_textarea_set_max_length(s_psk_server, 63);
    lv_obj_add_event_cb(s_psk_server, text_input_event, LV_EVENT_ALL, NULL);

    lv_obj_t *port_label = lv_label_create(s_psk_dialog);
    lv_label_set_text(port_label, "端口");
    lv_obj_set_pos(port_label, 455, 186);
    s_psk_port = lv_textarea_create(s_psk_dialog);
    lv_obj_set_size(s_psk_port, 135, 42);
    lv_obj_set_pos(s_psk_port, 455, 208);
    lv_textarea_set_one_line(s_psk_port, true);
    lv_textarea_set_accepted_chars(s_psk_port, "0123456789");
    lv_textarea_set_max_length(s_psk_port, 5);
    lv_obj_add_event_cb(s_psk_port, text_input_event, LV_EVENT_ALL, NULL);

    s_psk_status_label = lv_label_create(s_psk_dialog);
    lv_obj_set_width(s_psk_status_label, 590);
    lv_label_set_text(s_psk_status_label, "配置保存在设备中.");
    lv_obj_set_style_text_color(s_psk_status_label, lv_color_hex(0x526374), 0);
    lv_obj_set_pos(s_psk_status_label, 0, 260);

    lv_obj_t *save_psk_button = lv_button_create(s_psk_dialog);
    lv_obj_set_size(save_psk_button, 280, 44);
    lv_obj_set_pos(save_psk_button, 0, 304);
    style_button(save_psk_button);
    lv_obj_add_event_cb(save_psk_button, save_psk_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_psk_label = lv_label_create(save_psk_button);
    lv_label_set_text(save_psk_label, "保存配置");
    lv_obj_center(save_psk_label);

    lv_obj_t *close_psk_button = lv_button_create(s_psk_dialog);
    lv_obj_set_size(close_psk_button, 280, 44);
    lv_obj_set_pos(close_psk_button, 310, 304);
    style_button(close_psk_button);
    lv_obj_add_event_cb(close_psk_button, close_psk_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_psk_label = lv_label_create(close_psk_button);
    lv_label_set_text(close_psk_label, "返回");
    lv_obj_center(close_psk_label);

    lv_obj_add_flag(s_psk_dialog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_wifi_page, LV_OBJ_FLAG_HIDDEN);
}

void app_ui_init(void)
{
    setenv("TZ", "UTC0", 1);
    tzset();
    bsp_display_start();
    bsp_display_lock(0);

    s_waterfall_buffer = heap_caps_calloc(WATERFALL_WIDTH * WATERFALL_HEIGHT, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    assert(s_waterfall_buffer != NULL);
    for (int i = 0; i < WATERFALL_WIDTH * WATERFALL_HEIGHT; ++i) {
        s_waterfall_buffer[i] = lv_color_to_u16(lv_color_hex(0xF8FBFF));
    }

    lv_style_init(&s_card_style);
    lv_style_set_bg_color(&s_card_style, lv_color_hex(0xFFFFFF));
    lv_style_set_bg_opa(&s_card_style, LV_OPA_COVER);
    lv_style_set_border_color(&s_card_style, lv_color_hex(0xD8ECF8));
    lv_style_set_border_width(&s_card_style, 1);
    lv_style_set_radius(&s_card_style, 16);
    lv_style_set_pad_all(&s_card_style, 14);
    lv_style_set_shadow_color(&s_card_style, lv_color_hex(0xDDEAF2));
    lv_style_set_shadow_width(&s_card_style, 10);
    lv_style_set_shadow_opa(&s_card_style, LV_OPA_30);

    lv_style_init(&s_button_style);
    lv_style_set_bg_color(&s_button_style, lv_color_hex(0xA9DDFC));
    lv_style_set_text_color(&s_button_style, lv_color_hex(0x254D6D));
    lv_style_set_radius(&s_button_style, 12);
    lv_style_set_border_color(&s_button_style, lv_color_hex(0x8BCBF2));
    lv_style_set_border_width(&s_button_style, 1);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, &ui_font_cn, 0);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFF9FC), 0);
    create_dashboard(screen);
    create_wifi_page(screen);
    lv_timer_create(ui_timer_event, 250, NULL);
    bsp_display_unlock();
}

void app_ui_set_wired_network_status(const char *text)
{
    set_label(s_wired_network_badge, text);
}

void app_ui_set_wifi_network_status(const char *text)
{
    set_label(s_wifi_network_badge, text);
}

void app_ui_set_wifi_status(const char *text)
{
    set_label(s_wifi_status_label, text);
}

void app_ui_set_receiver_status(const char *text)
{
    set_label(s_receiver_status, text);
}

void app_ui_set_usb_status(const char *text)
{
    set_label(s_usb_status_label, text);
}

void app_ui_set_time_status(const char *text)
{
    set_label(s_time_source_label, text);
}

void app_ui_add_decode(const char *mode, const char *utc, float snr, float frequency, const char *message)
{
    if (!bsp_display_lock(2000)) {
        return;
    }
    if (lv_obj_get_child_count(s_decode_list) >= 12) {
        lv_obj_delete(lv_obj_get_child(s_decode_list, 0));
    }
    char row[128];
    snprintf(row, sizeof(row), "%s | %s | %+d dB | %d Hz | %s",
             utc, mode, (int)lroundf(snr), (int)lroundf(frequency), message);
    lv_obj_t *button = lv_list_add_button(s_decode_list, NULL, row);
    lv_obj_set_width(button, LV_PCT(100));
    lv_obj_set_height(button, 46);
    lv_obj_set_style_text_color(button, lv_color_hex(0x36556D), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xF5FAFF), 0);
    bsp_display_unlock();
}

void app_ui_update_waterfall(const uint8_t *magnitudes, size_t count)
{
    if (magnitudes == NULL || count == 0 || !bsp_display_lock(1000)) {
        return;
    }
    memmove(s_waterfall_buffer, s_waterfall_buffer + WATERFALL_WIDTH,
            (WATERFALL_HEIGHT - 1) * WATERFALL_WIDTH * sizeof(uint16_t));

    uint16_t *row = s_waterfall_buffer + (WATERFALL_HEIGHT - 1) * WATERFALL_WIDTH;
    for (int x = 0; x < WATERFALL_WIDTH; ++x) {
        uint8_t value = magnitudes[(size_t)x * count / WATERFALL_WIDTH];
        uint8_t r;
        uint8_t g;
        uint8_t b;
        if (value < 100) {
            r = 248 - value / 5;
            g = 251 - value / 8;
            b = 255;
        } else if (value < 180) {
            uint8_t t = value - 100;
            r = 228 + t / 3;
            g = 238 - t / 2;
            b = 255 - t / 5;
        } else {
            uint8_t t = value - 180;
            r = 254;
            g = 198 - t;
            b = 222 - t / 2;
        }
        row[x] = lv_color_to_u16(lv_color_make(r, g, b));
    }
    lv_obj_invalidate(s_waterfall_canvas);
    bsp_display_unlock();
}

void app_ui_update_networks(void)
{
    uint16_t count = 0;
    const wifi_ap_record_t *records = network_manager_get_records(&count);
    if (!bsp_display_lock(2000)) {
        return;
    }
    lv_obj_clean(s_network_list);
    if (count == 0) {
        lv_list_add_text(s_network_list, "没有发现网络");
    } else {
        for (int i = 0; i < count; ++i) {
            char item[64];
            snprintf(item, sizeof(item), "%s  (%d dBm)", records[i].ssid, records[i].rssi);
            lv_obj_t *button = lv_list_add_button(s_network_list, LV_SYMBOL_WIFI, item);
            lv_obj_add_event_cb(button, network_event, LV_EVENT_CLICKED, (void *)records[i].ssid);
        }
    }
    bsp_display_unlock();
}

ftx_protocol_t app_ui_get_protocol(void)
{
    return ftx_receiver_get_protocol();
}
