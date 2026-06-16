#pragma once

#include "ft8/decode.h"

typedef enum {
    FTX_DECODE_FAST,
    FTX_DECODE_MULTI_PASS,
} ftx_decode_strategy_t;

void ftx_receiver_start(void);
void ftx_receiver_set_protocol(ftx_protocol_t protocol);
ftx_protocol_t ftx_receiver_get_protocol(void);
void ftx_receiver_set_decode_strategy(ftx_decode_strategy_t strategy);
ftx_decode_strategy_t ftx_receiver_get_decode_strategy(void);
