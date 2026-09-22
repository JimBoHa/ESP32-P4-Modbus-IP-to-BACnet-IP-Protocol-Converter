#ifndef MODBUS_TCP_H
#define MODBUS_TCP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MB_MAX_REGISTERS 50U
#define MB_FC03_REQUEST_SIZE 12U
#define MB_MAX_RESPONSE_SIZE (9U + 2U * MB_MAX_REGISTERS)

typedef enum {
    MB_OK = 0,
    MB_ERR_ARGUMENT,
    MB_ERR_CONNECT,
    MB_ERR_IO,
    MB_ERR_TIMEOUT,
    MB_ERR_TRUNCATED,
    MB_ERR_TRANSACTION,
    MB_ERR_PROTOCOL,
    MB_ERR_UNIT,
    MB_ERR_LENGTH,
    MB_ERR_FUNCTION,
    MB_ERR_BYTE_COUNT,
    MB_ERR_EXCEPTION
} mb_error_t;

typedef struct {
    mb_error_t error;
    uint8_t exception_code; /* Set only for a valid matching exception response. */
    uint32_t elapsed_ms;
    int system_error;       /* errno/SO_ERROR for socket failures, otherwise 0. */
} mb_result_t;

/* Read-only FC01/02/03/04 operations. Quantities are limited to 1..50.
 * FC01/02 output one 0/1 uint16_t per requested bit; FC03/04 output registers.
 * Builders/decoders leave caller output unchanged on error. */
size_t mb_build_read_request(uint8_t request[MB_FC03_REQUEST_SIZE],
                            uint16_t tid, uint8_t unit, uint8_t function,
                            uint16_t offset, uint16_t qty);
mb_error_t mb_decode_read_response(const uint8_t *frame, size_t length,
                                  uint16_t expected_tid, uint8_t expected_unit,
                                  uint8_t function, uint16_t qty,
                                  uint16_t out[MB_MAX_REGISTERS], mb_result_t *result);
mb_error_t mb_read_points(const char *host_ipv4, uint16_t port, uint8_t unit,
                         uint8_t function, uint16_t offset, uint16_t qty,
                         uint16_t tid, uint32_t timeout_ms,
                         uint16_t out[MB_MAX_REGISTERS], mb_result_t *result);

/* FC03 only. Returns 12 on success or 0 on invalid arguments. The request
 * buffer is unchanged on failure. Offsets are zero based, not 4xxxx labels. */
size_t mb_build_fc03_request(uint8_t request[MB_FC03_REQUEST_SIZE],
                            uint16_t tid, uint8_t unit, uint16_t offset,
                            uint16_t qty);

/* Decode exactly one complete TCP ADU. Every header/PDU field is checked.
 * On success only out[0..qty-1] is changed. On any failure out is unchanged.
 * result is optional; this pure decoder sets elapsed_ms/system_error to zero. */
mb_error_t mb_decode_fc03_response(const uint8_t *frame, size_t length,
                                   uint16_t expected_tid, uint8_t expected_unit,
                                   uint16_t qty, uint16_t out[MB_MAX_REGISTERS],
                                   mb_result_t *result);

/* One connection and one FC03 request; no retries and no write functions.
 * Numeric IPv4 only (no DNS). timeout_ms must be positive. Connect, send,
 * and all fragmented receives share a single monotonic absolute deadline.
 * qty must be 1..50 and offset+qty must fit the 16-bit register address space.
 * TCP unit identifiers are accepted as 0..255; the configured device decides
 * which identifiers it supports. out is unchanged unless MB_OK is returned.
 * result is optional. Intended to run in the dedicated Modbus polling task. */
mb_error_t mb_read_holding(const char *host_ipv4, uint16_t port, uint8_t unit,
                           uint16_t offset, uint16_t qty, uint16_t tid,
                           uint32_t timeout_ms,
                           uint16_t out[MB_MAX_REGISTERS], mb_result_t *result);

uint64_t mb_monotonic_ms(void);
const char *mb_error_string(mb_error_t error);

#ifdef __cplusplus
}
#endif

#endif
