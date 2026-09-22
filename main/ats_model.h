#ifndef ATS_MODEL_H
#define ATS_MODEL_H
/* Portable C99 read-only MPAC1500 section-13 map. No network or RTOS calls. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ATS_POINT_COUNT 164u
#define ATS_BLOCK_COUNT 23u
#define ATS_MAX_BLOCK_WORDS 40u
#define ATS_MAX_POINT_WORDS 10u
#define ATS_TEXT_CAPACITY 80u

typedef enum {
    ATS_QUALITY_GOOD = 0,
    ATS_QUALITY_COMM = 1,       /* Never acquired, failed read, or stale. */
    ATS_QUALITY_UNVERIFIED = 2, /* Readable raw value lacks measurement qualification. */
    ATS_QUALITY_NA = 3         /* Inapplicable under current verified configuration. */
} ats_quality_t;

typedef struct {
    uint16_t object_type; /* BACnet: 0 AI, 3 BI, 13 MSI, 40 CharacterStringValue. */
    uint32_t instance;    /* Stable catalog row + 1000; never compact by type. */
    const char *name;
    const char *description;
    uint16_t units;       /* BACnet EngineeringUnits enumeration. */
    uint16_t state_count; /* MSI presentValue is 1-based. */
    const char *const *state_text;
    uint16_t offset;     /* Zero-based FC03 wire offset, without leading 4. */
    uint8_t word_count;
    uint8_t bit_lsb;
    uint8_t bit_width;    /* 0 means whole register/encoding. */
} ats_point_def_t;

typedef struct {
    uint16_t offset;
    uint16_t count;
    uint32_t period_ms;
    uint32_t stale_ms;
    const char *name;
} ats_scan_block_t;

typedef struct {
    double numeric; /* NAN until acquired; BI 0/1; MSI 1-based. Honor quality. */
    char text[ATS_TEXT_CAPACITY];
    ats_quality_t quality;
    const char *quality_reason; /* Static string; never free. */
    uint64_t last_good_ms; /* Successful acquisition time, including unqualified raw data. */
    uint16_t raw_words[ATS_MAX_POINT_WORDS];
    uint8_t raw_count;
} ats_value_t;

typedef struct {
    uint16_t words[ATS_MAX_BLOCK_WORDS];
    uint64_t last_good_ms;
    uint64_t last_failure_ms;
    bool acquired;
    bool failed;
} ats_block_state_t;

typedef struct {
    ats_block_state_t blocks[ATS_BLOCK_COUNT];
} ats_model_t;

extern const ats_point_def_t ats_points[ATS_POINT_COUNT];
extern const ats_scan_block_t ats_scan_blocks[ATS_BLOCK_COUNT];

void ats_model_init(ats_model_t *model);
/* Accept only an exact declared block. No partial updates or unknown ranges. */
bool ats_model_apply_block(ats_model_t *model, uint16_t offset, uint16_t count,
                           const uint16_t *words, uint64_t now_ms);
bool ats_model_fail_block(ats_model_t *model, uint16_t offset, uint16_t count,
                          uint64_t now_ms);
/* Recomputes freshness/dependency quality; no mutation, allocation or I/O. */
void ats_model_decode_all(const ats_model_t *model, uint64_t now_ms,
                          ats_value_t values[ATS_POINT_COUNT]);
const char *ats_quality_name(ats_quality_t quality);

#endif
