#ifndef GATEWAY_POLL_H
#define GATEWAY_POLL_H
#include "ats_model.h"
#include "modbus_tcp.h"

typedef struct {
    const char *host;
    uint16_t port;
    uint8_t unit;
    uint16_t expected_firmware;
    uint16_t expected_mac_fragment;
} gateway_poll_config_t;

typedef struct {
    ats_model_t model;
    uint64_t next_due[ATS_BLOCK_COUNT];
    uint64_t next_request_ms;
    uint64_t profile_check_ms;
    uint64_t last_success_ms;
    uint32_t requests;
    uint32_t successes;
    uint32_t failures;
    uint32_t consecutive_failures;
    uint16_t transaction_id;
    uint16_t last_offset;
    uint8_t profile_stage;
    bool profile_valid;
    mb_result_t last_result;
    const char *profile_status;
} gateway_poll_t;

void gateway_poll_init(gateway_poll_t *state);
/* At most one FC03 request, bounded to 1200ms. Call only from the polling task. */
bool gateway_poll_step(gateway_poll_t *state, const gateway_poll_config_t *config,
                       uint64_t now_ms);
/* Preserve samples with faults when the network identity/profile changes. */
void gateway_poll_offline(gateway_poll_t *state, uint64_t now_ms);
#endif
