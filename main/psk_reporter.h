#pragma once

#include <stdint.h>
#include <time.h>

void psk_reporter_start(void);
void psk_reporter_queue(const char *sender_callsign, uint32_t frequency_hz, int8_t snr,
                        const char *mode, time_t received_at);
