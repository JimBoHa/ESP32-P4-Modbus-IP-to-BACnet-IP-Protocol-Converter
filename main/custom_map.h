#ifndef CUSTOM_MAP_H
#define CUSTOM_MAP_H
#include "ats_model.h"
#include "modbus_tcp.h"

#define CUSTOM_MAX_POINTS 128U
#define CUSTOM_MAX_CSV_BYTES 32768U
#define CUSTOM_MAX_STATES 16U
#define CUSTOM_MAX_TOTAL_STATES 512U
#define CUSTOM_CSV_HEADER "instance,name,object_type,function,address,data_type,byte_order,scale,offset,units,bit,states,length,poll_ms,description"

typedef enum {
    CUSTOM_U16, CUSTOM_S16, CUSTOM_U32, CUSTOM_S32, CUSTOM_F32,
    CUSTOM_BIT, CUSTOM_ASCII, CUSTOM_BOOL
} custom_data_type_t;

typedef struct {
    custom_data_type_t data_type;
    uint8_t function;
    uint8_t order; /* 0 AB/ABCD, 1 BA/BADC, 2 CDAB, 3 DCBA. */
    uint8_t length; /* ASCII character count, otherwise zero. */
    uint32_t poll_ms;
    double scale;
    double offset;
} custom_point_t;

typedef struct {
    size_t count;
    ats_point_def_t defs[CUSTOM_MAX_POINTS];
    custom_point_t points[CUSTOM_MAX_POINTS];
    /* String and state pointers refer to this object; do not shallow-copy it. */
    char strings[CUSTOM_MAX_CSV_BYTES + 1U];
    const char *state_text[CUSTOM_MAX_TOTAL_STATES];
    size_t strings_used;
    size_t states_used;
} custom_map_t;

typedef struct {
    ats_value_t values[CUSTOM_MAX_POINTS];
    uint64_t next_due[CUSTOM_MAX_POINTS];
    bool acquired[CUSTOM_MAX_POINTS];
    uint64_t next_request_ms;
    uint64_t last_success_ms;
    uint32_t requests, successes, failures, consecutive_failures;
    /* Metadata for the last actual request, retained across no-op steps. */
    uint16_t transaction_id, last_offset, last_quantity;
    uint8_t last_function;
    mb_result_t last_result;
} custom_poll_t;

/* CSV input is an explicit byte span; no NUL terminator is required.
 * Parse is transactional: failure leaves map unchanged. On success all string
 * pointers belong to map. ASCII printable fields only; quoted commas/quotes
 * are accepted, embedded newlines and control bytes are rejected.
 * errors include 1-based source line. Caller persists the original CSV. */
bool custom_map_parse(custom_map_t *map, const char *csv, size_t length,
                      char *error, size_t error_size);
/* Decode a complete point. Invalid/infinite values do not replace old samples. */
bool custom_map_apply(const custom_map_t *map, size_t index, const uint16_t *words,
                      size_t count, uint64_t now_ms, ats_value_t *value);
/* Freshness allows a bounded healthy sequential sweep; failed reads fault immediately. */
uint64_t custom_map_stale_ms(const custom_map_t *map, size_t index);
void custom_poll_init(custom_poll_t *state, const custom_map_t *map);
/* One read-only FC01/02/03/04 request maximum; 1200ms deadline; >=250ms gap.
 * With many rows, requested poll_ms is a minimum interval, not a guarantee. */
bool custom_poll_step(custom_poll_t *state, const custom_map_t *map,
                       const char *host, uint16_t port, uint8_t unit, uint64_t now_ms);
void custom_poll_snapshot(const custom_poll_t *state, const custom_map_t *map,
                          uint64_t now_ms, ats_value_t *values);
void custom_poll_offline(custom_poll_t *state, const custom_map_t *map, uint64_t now_ms);
#endif
