#ifndef ERROR_HISTORY_H
#define ERROR_HISTORY_H

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "modbus_tcp.h"

#define ERROR_HISTORY_CAPACITY 32U
#define ERROR_HISTORY_FLUSH_INTERVAL_MS 30000U

typedef struct {
    char host[16];
    uint16_t port, offset, quantity, transaction_id;
    uint8_t unit, function;
    uint32_t config_revision;
    char profile[32], phase[16];
} error_history_request_t;

/* Call once after gateway_cfg NVS initialization. Flash writes run in a
 * separate task; successful reads never clear the history. A restart can lose
 * events awaiting the next (at most once per 30 seconds) background flush.
 * Unreadable or incompatible existing history is preserved without writes. */
void error_history_init(void);

/* The caller excludes expected protocol exceptions. MB_OK is ignored.
 * uptime_ms is monotonic time at completion; utc_ms is zero until a real
 * wall-clock synchronization. This function does not perform flash I/O. */
void error_history_record(const error_history_request_t *request,
                          const mb_result_t *result,
                          uint64_t uptime_ms, int64_t utc_ms);

/* Caller owns the returned JSON object. Events are newest first. */
cJSON *error_history_json(bool clock_synchronized);

#endif
