/* MPAC1500 section-13 decoder. Caller must establish the old-map profile.
 * Successful protocol transport establishes readability, not fitted sensors.
 * No write functions, network addresses, equipment identities, or samples here.
 */
#include "ats_model.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef enum { ENC_U16, ENC_BOOL, ENC_ENUM, ENC_U32, ENC_ASCII,
               ENC_CLOCK, ENC_DATE, ENC_VERSION } encoding_t;
typedef struct { encoding_t encoding; uint16_t numerator, denominator; } point_decode_t;
#include "ats_map.inc"

static int exact_block(uint16_t offset, uint16_t count)
{
    size_t i;
    for (i = 0; i < ATS_BLOCK_COUNT; ++i)
        if (ats_scan_blocks[i].offset == offset && ats_scan_blocks[i].count == count)
            return (int)i;
    return -1;
}

static int containing_block(uint16_t offset, uint16_t count)
{
    size_t i;
    for (i = 0; i < ATS_BLOCK_COUNT; ++i) {
        const ats_scan_block_t *b = &ats_scan_blocks[i];
        if (offset >= b->offset && (uint32_t)offset + count <= (uint32_t)b->offset + b->count)
            return (int)i;
    }
    return -1;
}

void ats_model_init(ats_model_t *model)
{
    if (model) memset(model, 0, sizeof(*model));
}

bool ats_model_apply_block(ats_model_t *model, uint16_t offset, uint16_t count,
                           const uint16_t *words, uint64_t now_ms)
{
    int index = exact_block(offset, count);
    ats_block_state_t *state;
    if (!model || !words || index < 0) return false;
    state = &model->blocks[index];
    if (state->acquired && now_ms < state->last_good_ms) return false;
    memcpy(state->words, words, count * sizeof(*words));
    state->last_good_ms = now_ms;
    state->acquired = true;
    state->failed = false;
    return true;
}

bool ats_model_fail_block(ats_model_t *model, uint16_t offset, uint16_t count,
                          uint64_t now_ms)
{
    int index = exact_block(offset, count);
    ats_block_state_t *state;
    if (!model || index < 0) return false;
    state = &model->blocks[index];
    if (state->acquired && now_ms < state->last_good_ms) return false;
    state->failed = true;
    state->last_failure_ms = now_ms;
    return true;
}

static bool block_good(const ats_model_t *model, int index, uint64_t now_ms)
{
    const ats_block_state_t *state;
    if (index < 0) return false;
    state = &model->blocks[index];
    return state->acquired && !state->failed && now_ms >= state->last_good_ms &&
           now_ms - state->last_good_ms <= ats_scan_blocks[index].stale_ms;
}

static bool register_good(const ats_model_t *model, uint16_t offset,
                          uint64_t now_ms, uint16_t *value)
{
    int index = containing_block(offset, 1);
    if (!block_good(model, index, now_ms)) return false;
    *value = model->blocks[index].words[(size_t)(offset - ats_scan_blocks[index].offset)];
    return true;
}

static void quality(ats_value_t *value, ats_quality_t q, const char *reason)
{
    value->quality = q;
    value->quality_reason = reason;
}

static bool calendar_date(uint16_t word, unsigned *year, unsigned *month, unsigned *day)
{
    static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned limit;
    *year = 2000u + (word >> 9);
    *month = (word >> 5) & 15u;
    *day = word & 31u;
    if (*month < 1 || *month > 12) return false;
    limit = days[*month-1];
    if (*month == 2 && (*year % 4 == 0) && ((*year % 100 != 0) || (*year % 400 == 0))) ++limit;
    return *day >= 1 && *day <= limit;
}

static void decode_value(size_t index, const uint16_t *words, ats_value_t *value)
{
    const ats_point_def_t *def = &ats_points[index];
    const point_decode_t *decoder = &point_decoders[index];
    uint32_t raw = words[0];
    unsigned year, month, day;
    size_t i, n = 0;
    bool terminated = false, printable = true;
    if (def->bit_width)
        raw = (raw >> def->bit_lsb) & ((1u << def->bit_width)-1u);
    switch (decoder->encoding) {
    case ENC_U16:
        value->numeric = (double)raw * decoder->numerator / decoder->denominator;
        break;
    case ENC_BOOL:
        value->numeric = (double)raw;
        if (raw > 1u) quality(value, ATS_QUALITY_UNVERIFIED, "Boolean register is neither zero nor one");
        break;
    case ENC_ENUM:
        if (raw < (uint32_t)(def->state_count-1u)) value->numeric = (double)raw + 1.0;
        else {
            value->numeric = def->state_count; /* Explicit Unknown state. */
            quality(value, ATS_QUALITY_UNVERIFIED, "Undocumented enumeration; raw value retained");
        }
        if (def->offset == 56 && def->bit_lsb == 0 && raw == 3)
            quality(value, ATS_QUALITY_UNVERIFIED, "Reserved transition mode");
        break;
    case ENC_U32:
        value->numeric = (double)(((uint32_t)words[0] << 16) | words[1]);
        break;
    case ENC_VERSION:
        (void)snprintf(value->text, sizeof(value->text), "%u.%02u", words[0] >> 8, words[0] & 255u);
        break;
    case ENC_CLOCK:
        if (raw < 1440u) (void)snprintf(value->text, sizeof(value->text), "%02u:%02u", (unsigned)raw / 60u, (unsigned)raw % 60u);
        else {
            (void)snprintf(value->text, sizeof(value->text), "Invalid clock (%u)", (unsigned)raw);
            quality(value, ATS_QUALITY_UNVERIFIED, "Controller clock is outside minute-of-day range; no timezone inferred");
        }
        break;
    case ENC_DATE:
        if (calendar_date((uint16_t)raw, &year, &month, &day))
            (void)snprintf(value->text, sizeof(value->text), "%04u-%02u-%02u", year, month, day);
        else {
            (void)snprintf(value->text, sizeof(value->text), "Invalid date (0x%04X)", (unsigned)raw);
            quality(value, ATS_QUALITY_UNVERIFIED, "Invalid or unset packed Gregorian date");
        }
        break;
    case ENC_ASCII:
        /* Only text uses this device's observed low-byte-first convention. */
        for (i = 0; i < def->word_count * 2u && !terminated; ++i) {
            unsigned c = (words[i / 2] >> ((i % 2) * 8u)) & 255u;
            if (c == 0) { terminated = true; continue; }
            if (c < 32u || c > 126u) { c = '?'; printable = false; }
            if (n + 1 < sizeof(value->text)) value->text[n++] = (char)c;
        }
        value->text[n] = '\0';
        if (!printable) quality(value, ATS_QUALITY_UNVERIFIED, "Non-ASCII text; raw words retained");
        break;
    }
}

static void qualify(const ats_model_t *model, const ats_point_def_t *def,
                    uint64_t now_ms, ats_value_t *value)
{
    uint16_t context, overview, mode;
    unsigned board;
    if (value->quality != ATS_QUALITY_GOOD) return;
    if (def->offset >= 15 && def->offset <= 17) {
        quality(value, ATS_QUALITY_UNVERIFIED, "Current sensing/CT installation has not been independently verified");
    } else if (def->offset == 18 || def->offset == 40) {
        if (!register_good(model, 0, now_ms, &overview) ||
            !register_good(model, 56, now_ms, &mode)) {
            quality(value, ATS_QUALITY_COMM, "Source availability or transition mode is unavailable/stale");
        } else if ((overview & 0x6000u) != 0x6000u || (def->offset == 18 && (mode & 3u) != 2u)) {
            quality(value, ATS_QUALITY_NA, "Inter-source angle is not applicable with an unavailable source or incompatible transition mode");
        } else {
            quality(value, ATS_QUALITY_UNVERIFIED, "Angle register readable; synchronization/measurement applicability remains unverified");
        }
    } else if (def->offset >= 195 && def->offset <= 202) {
        board = (def->offset-195u) % 4u;
        if (!register_good(model, (uint16_t)(153u+board), now_ms, &context))
            quality(value, ATS_QUALITY_COMM, "Expansion-board presence is unavailable/stale");
        else if (context == 0)
            quality(value, ATS_QUALITY_NA, "Corresponding expansion board is not installed");
        else if (context > 2)
            quality(value, ATS_QUALITY_UNVERIFIED, "Unknown expansion-board type");
    } else if (def->offset == 249 || def->offset == 251) {
        if (!register_good(model, (uint16_t)(def->offset+1u), now_ms, &context))
            quality(value, ATS_QUALITY_COMM, "I/O assignment bitmap is unavailable/stale");
        else if ((context & (def->offset == 249 ? 15u : 31u)) == 0)
            quality(value, ATS_QUALITY_NA, "No software outputs or remote monitored inputs are assigned");
    } else if (def->offset == 1011) {
        quality(value, ATS_QUALITY_UNVERIFIED, "Outage duration word order is not independently verified; raw pair retained");
    } else if (def->offset >= 1294 && def->offset <= 1298 && value->raw_words[0] == 0) {
        quality(value, ATS_QUALITY_UNVERIFIED, "Zero historical timing may mean unset or inapplicable");
    } else if (def->offset == 1319 && value->raw_words[0] == 0x0221u) {
        quality(value, ATS_QUALITY_UNVERIFIED, "Stored maintenance date matches known default; not proof of a service visit");
    } else if (def->offset == 0 && def->bit_width == 0 && (value->raw_words[0] & 0x8000u)) {
        quality(value, ATS_QUALITY_UNVERIFIED, "Reserved overview bit 15 is set; raw word retained");
    }
}

void ats_model_decode_all(const ats_model_t *model, uint64_t now_ms,
                          ats_value_t values[ATS_POINT_COUNT])
{
    size_t i;
    if (!values) return;
    for (i = 0; i < ATS_POINT_COUNT; ++i) {
        const ats_point_def_t *def = &ats_points[i];
        ats_value_t *value = &values[i];
        const ats_block_state_t *state;
        const uint16_t *words;
        int block;
        memset(value, 0, sizeof(*value));
        value->numeric = NAN;
        quality(value, ATS_QUALITY_COMM, "Not yet acquired");
        if (!model) continue;
        block = containing_block(def->offset, def->word_count);
        if (block < 0) { value->quality_reason = "Point is not contained in a scan block"; continue; }
        state = &model->blocks[block];
        if (!state->acquired) continue;
        words = state->words + (size_t)(def->offset - ats_scan_blocks[block].offset);
        memcpy(value->raw_words, words, def->word_count * sizeof(*words));
        value->raw_count = def->word_count;
        value->last_good_ms = state->last_good_ms;
        quality(value, ATS_QUALITY_GOOD, "Fresh controller-reported value");
        decode_value(i, words, value);
        if (!block_good(model, block, now_ms)) {
            quality(value, ATS_QUALITY_COMM, state->failed ? "Latest block read failed; previous raw value retained" : "Block data is stale or clock moved backwards");
        } else {
            qualify(model, def, now_ms, value);
        }
    }
}

const char *ats_quality_name(ats_quality_t q)
{
    switch (q) {
    case ATS_QUALITY_GOOD: return "good";
    case ATS_QUALITY_COMM: return "communication-failure";
    case ATS_QUALITY_UNVERIFIED: return "unverified";
    case ATS_QUALITY_NA: return "not-applicable";
    default: return "unknown-quality";
    }
}
