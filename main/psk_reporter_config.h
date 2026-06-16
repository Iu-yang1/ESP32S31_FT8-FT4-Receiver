#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool enabled;
    char callsign[16];
    char grid[9];
    char antenna[48];
    char server[64];
    uint16_t port;
} psk_reporter_config_t;

void psk_reporter_config_init(void);
const psk_reporter_config_t *psk_reporter_config_get(void);
bool psk_reporter_config_save(const psk_reporter_config_t *config);
