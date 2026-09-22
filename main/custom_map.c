#include "custom_map.h"
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIELD_COUNT 15U
#define FIELD_BYTES 513U

typedef struct { char fields[FIELD_COUNT][FIELD_BYTES]; } csv_row_t;

static bool fail(char *error, size_t size, size_t line, const char *why)
{
    if (error && size) (void)snprintf(error, size, "Line %u: %s", (unsigned)line, why);
    return false;
}

static bool integer(const char *s, uint32_t minimum, uint32_t maximum, uint32_t *out)
{
    if (!s || !*s) return false;
    uint32_t n = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return false;
        unsigned digit = (unsigned)(*s - '0');
        if (digit > maximum || n > (maximum - digit) / 10U)
            return false;
        n = n * 10U + digit;
        if (n > maximum) return false;
    }
    if (n < minimum) return false;
    *out = n;
    return true;
}

static bool number(const char *s, double fallback, double *out)
{
    if (!*s) { *out = fallback; return true; }
    char *end;
    errno = 0;
    double n = strtod(s, &end);
    if (errno || end == s || *end || !isfinite(n)) return false;
    *out = n;
    return true;
}

/* Exactly 15 fields. A closing quote may be followed only by comma/EOL. */
static bool read_row(const char *csv, size_t length, size_t *position, csv_row_t *row)
{
    memset(row, 0, sizeof(*row));
    for (size_t field = 0; field < FIELD_COUNT; ++field) {
        size_t n = 0;
        bool quoted = *position < length && csv[*position] == '"';
        bool closed = !quoted;
        if (quoted) ++*position;
        while (*position < length) {
            unsigned char ch = (unsigned char)csv[*position];
            if (ch < 32U || ch > 126U) {
                if (!quoted && (ch == '\r' || ch == '\n')) break;
                return false;
            }
            if (quoted && ch == '"') {
                ++*position;
                if (*position < length && csv[*position] == '"') ++*position;
                else { closed = true; break; }
            } else {
                if (!quoted && ch == ',') break;
                if (!quoted && ch == '"') return false;
                ++*position;
            }
            if (n + 1U >= FIELD_BYTES) return false;
            row->fields[field][n++] = (char)ch;
        }
        if (!closed) return false;
        if (field + 1U < FIELD_COUNT) {
            if (*position >= length || csv[*position] != ',') return false;
            ++*position;
        } else if (*position < length) {
            if (csv[*position] == '\r') {
                ++*position;
                if (*position >= length || csv[*position] != '\n') return false;
            }
            if (csv[*position] != '\n') return false;
            ++*position;
        }
    }
    return true;
}

static const char *save_string(custom_map_t *map, const char *s)
{
    size_t n = strlen(s) + 1U;
    if (n > sizeof(map->strings) - map->strings_used) return NULL;
    char *p = map->strings + map->strings_used;
    memcpy(p, s, n);
    map->strings_used += n;
    return p;
}

static bool add_row(custom_map_t *map, csv_row_t *row, const char **why)
{
#define F(i) row->fields[(i)]
#define REJECT(s) do { *why = (s); return false; } while (0)
    if (map->count == CUSTOM_MAX_POINTS) REJECT("Maximum 128 points exceeded");
    size_t index = map->count;
    ats_point_def_t *d = &map->defs[index];
    custom_point_t *p = &map->points[index];
    uint32_t v;
    if (!integer(F(0), 1, 4194302, &d->instance) ||
        (d->instance >= 9001 && d->instance <= 9004)) REJECT("Invalid/reserved instance");
    if (!*F(1) || strlen(F(1)) > 63 || strncmp(F(1), "Gateway-", 8) == 0)
        REJECT("Name must be 1..63 characters and not start Gateway-");
    if (strlen(F(14)) > 127) REJECT("Description exceeds 127 characters");
    if (!strcmp(F(2), "AI")) d->object_type = 0;
    else if (!strcmp(F(2), "BI")) d->object_type = 3;
    else if (!strcmp(F(2), "MSI")) d->object_type = 13;
    else if (!strcmp(F(2), "CSV")) d->object_type = 40;
    else REJECT("object_type must be AI, BI, MSI or CSV");
    for (size_t i = 0; i < index; ++i) {
        if (!strcmp(map->defs[i].name, F(1))) REJECT("Duplicate point name");
        if (map->defs[i].object_type == d->object_type && map->defs[i].instance == d->instance)
            REJECT("Duplicate BACnet object identifier");
    }
    if (!integer(F(3), 1, 4, &v)) REJECT("Only read functions 1..4 allowed");
    p->function = (uint8_t)v;
    if (!integer(F(4), 0, 65535, &v)) REJECT("Address must be zero-based 0..65535");
    d->offset = (uint16_t)v;
    const char *types[] = {"u16", "s16", "u32", "s32", "f32", "bit", "ascii", "bool"};
    size_t kind;
    for (kind = 0; kind < sizeof(types) / sizeof(types[0]); ++kind)
        if (!strcmp(F(5), types[kind])) break;
    if (kind == sizeof(types) / sizeof(types[0])) REJECT("Unknown data_type");
    p->data_type = (custom_data_type_t)kind;
    d->word_count = kind >= CUSTOM_U32 && kind <= CUSTOM_F32 ? 2 : 1;
    if (p->function <= 2 && (kind != CUSTOM_BOOL || d->object_type != 3))
        REJECT("FC1/2 require BI bool");
    if (p->function >= 3 && kind == CUSTOM_BOOL) REJECT("bool requires FC1/2; use bit for a register");
    if ((d->object_type == 40) != (kind == CUSTOM_ASCII)) REJECT("CSV objects require ascii and vice versa");
    if (d->object_type == 3 && kind != CUSTOM_BIT && kind != CUSTOM_BOOL) REJECT("BI requires bit or bool");
    if (d->object_type == 13 && kind > CUSTOM_S32) REJECT("MSI requires an integer data_type");
    if (d->object_type == 0 && kind > CUSTOM_F32) REJECT("AI requires a numeric data_type");
    if (!number(F(7), 1.0, &p->scale) || !number(F(8), 0.0, &p->offset)) REJECT("Scale/offset must be finite numbers");
    if ((kind == CUSTOM_BIT || kind == CUSTOM_BOOL || kind == CUSTOM_ASCII) &&
        (p->scale != 1.0 || p->offset != 0.0)) REJECT("Bit/bool/ascii require scale=1 and offset=0");
    if (*F(9)) {
        if (!integer(F(9), 0, 65535, &v)) REJECT("Units must be a BACnet units enumeration");
        d->units = (uint16_t)v;
    } else d->units = 95; /* no-units */
    if (d->object_type != 0 && d->units != 95) REJECT("Only AI can have engineering units");
    if (kind == CUSTOM_BIT) {
        if (!integer(F(10), 0, 15, &v)) REJECT("bit must be 0..15");
        d->bit_lsb = (uint8_t)v;
        d->bit_width = 1;
    } else if (*F(10)) REJECT("bit field is valid only for bit data_type");
    if (kind == CUSTOM_ASCII) {
        if (!integer(F(12), 1, ATS_MAX_POINT_WORDS * 2U, &v)) REJECT("ASCII length must be 1..20 characters");
        p->length = (uint8_t)v;
        d->word_count = (uint8_t)((v + 1U) / 2U);
    } else if (*F(12)) REJECT("length field is valid only for ascii");
    if (d->word_count + (uint32_t)d->offset > 65536U) REJECT("Point extends past address 65535");
    if (kind == CUSTOM_BOOL) {
        if (*F(6)) REJECT("bool byte_order must be empty");
    } else if (kind >= CUSTOM_U32 && kind <= CUSTOM_F32) {
        if (!strcmp(F(6), "ABCD")) p->order = 0;
        else if (!strcmp(F(6), "BADC")) p->order = 1;
        else if (!strcmp(F(6), "CDAB")) p->order = 2;
        else if (!strcmp(F(6), "DCBA")) p->order = 3;
        else REJECT("32-bit byte_order must be ABCD, BADC, CDAB or DCBA");
    } else {
        if (!strcmp(F(6), "AB")) p->order = 0;
        else if (!strcmp(F(6), "BA")) p->order = 1;
        else REJECT("16-bit/ascii byte_order must be AB or BA");
    }
    if (!integer(F(13), 1000, 3600000, &p->poll_ms)) REJECT("poll_ms must be 1000..3600000");
    d->name = save_string(map, F(1));
    d->description = save_string(map, F(14));
    if (!d->name || !d->description) REJECT("String storage limit exceeded");
    if (d->object_type == 13) {
        if (!*F(11)) REJECT("MSI states must contain 1..16 pipe-separated labels");
        d->state_text = &map->state_text[map->states_used];
        char *start = F(11);
        for (;;) {
            char *separator = strchr(start, '|');
            if (separator) *separator = 0;
            if (!*start || strlen(start) > 63 || d->state_count == CUSTOM_MAX_STATES ||
                map->states_used == CUSTOM_MAX_TOTAL_STATES) REJECT("Invalid state label/count; max16 per point,512 total");
            for (size_t previous = 0; previous < d->state_count; ++previous)
                if (!strcmp(d->state_text[previous], start)) REJECT("Duplicate MSI state label");
            const char *label = save_string(map, start);
            if (!label) REJECT("String storage limit exceeded");
            map->state_text[map->states_used++] = label;
            ++d->state_count;
            if (!separator) break;
            start = separator + 1;
        }
    } else if (*F(11)) REJECT("states field is valid only for MSI");
    ++map->count;
    return true;
#undef F
#undef REJECT
}

bool custom_map_parse(custom_map_t *map, const char *csv, size_t length,
                      char *error, size_t error_size)
{
    if (error && error_size) *error = 0;
    if (!map || !csv || !length || length > CUSTOM_MAX_CSV_BYTES)
        return fail(error, error_size, 1, "CSV size must be 1..32768 bytes");
    custom_map_t *scratch = calloc(1, sizeof(*scratch));
    csv_row_t *row = calloc(1, sizeof(*row));
    bool ok = false;
    size_t position = 0, line = 1;
    const char *why = "Invalid CSV syntax, field size or non-ASCII text";
    if (!scratch || !row) { why = "Insufficient memory"; goto done; }
    size_t header_length = strlen(CUSTOM_CSV_HEADER);
    if (length < header_length || memcmp(csv, CUSTOM_CSV_HEADER, header_length) ||
        (length > header_length && csv[header_length] != '\r' && csv[header_length] != '\n')) {
        why = "Header must match documented 15 columns exactly";
        goto done;
    }
    if (!read_row(csv, length, &position, row)) goto done;
    while (position < length) {
        ++line;
        if (!read_row(csv, length, &position, row)) goto done;
        if (!add_row(scratch, row, &why)) goto done;
    }
    if (!scratch->count) { why = "At least one point is required"; goto done; }
    /* Rebase before copying, without subtracting pointers from unrelated objects. */
    for (size_t i = 0; i < scratch->count; ++i) {
        ats_point_def_t *d = &scratch->defs[i];
        d->name = map->strings + (d->name - scratch->strings);
        d->description = map->strings + (d->description - scratch->strings);
        if (d->state_text) d->state_text = map->state_text + (d->state_text - scratch->state_text);
    }
    for (size_t i = 0; i < scratch->states_used; ++i)
        scratch->state_text[i] = map->strings + (scratch->state_text[i] - scratch->strings);
    memcpy(map, scratch, sizeof(*map));
    ok = true;
done:
    if (!ok) fail(error, error_size, line, why);
    free(row);
    free(scratch);
    return ok;
}

static uint16_t swap16(uint16_t word) { return (uint16_t)((word >> 8) | (word << 8)); }

bool custom_map_apply(const custom_map_t *map, size_t index, const uint16_t *words,
                      size_t count, uint64_t now, ats_value_t *value)
{
    if (!map || index >= map->count || !words || !value) return false;
    const ats_point_def_t *d = &map->defs[index];
    const custom_point_t *p = &map->points[index];
    if (count != d->word_count) return false;
    ats_value_t next = *value;
    uint16_t a = p->order & 1U ? swap16(words[0]) : words[0];
    double n = 0;
    if (p->data_type == CUSTOM_ASCII) {
        bool ended = false;
        for (size_t i = 0; i < p->length; ++i) {
            uint16_t word = p->order ? swap16(words[i / 2U]) : words[i / 2U];
            unsigned char ch = (unsigned char)(i % 2U ? word : word >> 8);
            if (!ch) ended = true;
            if ((ch && (ch < 32U || ch > 126U)) || (ended && ch)) goto invalid;
            next.text[i] = (char)ch;
        }
        next.text[p->length] = 0;
    } else {
        uint32_t full = 0;
        if (count == 2) {
            uint16_t b = p->order & 1U ? swap16(words[1]) : words[1];
            full = p->order & 2U ? ((uint32_t)b << 16) | a : ((uint32_t)a << 16) | b;
        }
        switch (p->data_type) {
        case CUSTOM_U16: n = a; break;
        case CUSTOM_S16: n = a >= 0x8000U ? (double)a - 65536.0 : a; break;
        case CUSTOM_U32: n = full; break;
        case CUSTOM_S32: n = full >= 0x80000000U ? (double)full - 4294967296.0 : full; break;
        case CUSTOM_F32: {
            _Static_assert(sizeof(float) == sizeof(uint32_t), "IEEE32 float required");
            float floating;
            memcpy(&floating, &full, sizeof(floating));
            n = floating;
            break;
        }
        case CUSTOM_BIT: n = (a >> d->bit_lsb) & 1U; break;
        case CUSTOM_BOOL: if (words[0] > 1) goto invalid; n = words[0]; break;
        default: goto invalid;
        }
        n = n * p->scale + p->offset;
        if (!isfinite(n) || fabs(n) > FLT_MAX) goto invalid;
        if (d->object_type == 13 && (n < 1 || n > d->state_count || floor(n) != n)) goto invalid;
        next.numeric = n;
    }
    next.raw_count = (uint8_t)count;
    memcpy(next.raw_words, words, count * sizeof(*words));
    next.quality = ATS_QUALITY_GOOD;
    next.quality_reason = "Validated custom-map value";
    next.last_good_ms = now;
    *value = next;
    return true;
invalid:
    value->quality = ATS_QUALITY_UNVERIFIED;
    value->quality_reason = "Invalid/nonfinite decoded value or out-of-range state";
    return false;
}

void custom_poll_init(custom_poll_t *s, const custom_map_t *map)
{
    memset(s, 0, sizeof(*s));
    for (size_t i = 0; i < map->count; ++i) {
        s->values[i].numeric = NAN;
        s->values[i].quality = ATS_QUALITY_COMM;
        s->values[i].quality_reason = "Not acquired";
    }
}

void custom_poll_offline(custom_poll_t *s, const custom_map_t *map, uint64_t now)
{
    (void)now;
    for (size_t i = 0; i < map->count; ++i) {
        s->values[i].quality = ATS_QUALITY_COMM;
        s->values[i].quality_reason = "Network unavailable or transport failed";
    }
}

uint64_t custom_map_stale_ms(const custom_map_t *map, size_t index)
{
    if (!map || index >= map->count) return 0;
    uint64_t stale = (uint64_t)map->points[index].poll_ms * 3U;
    uint64_t sweep = (uint64_t)map->count * 3U * 250U + 1200U;
    if (stale < sweep) stale = sweep;
    if (stale < 5000U) stale = 5000U;
    return stale;
}

void custom_poll_snapshot(const custom_poll_t *s, const custom_map_t *map,
                          uint64_t now, ats_value_t *values)
{
    memcpy(values, s->values, map->count * sizeof(*values));
    for (size_t i = 0; i < map->count; ++i) {
        uint64_t stale = custom_map_stale_ms(map, i);
        if (s->acquired[i] && now >= values[i].last_good_ms && now - values[i].last_good_ms > stale) {
            values[i].quality = ATS_QUALITY_COMM;
            values[i].quality_reason = "Custom point is stale";
        }
    }
}

bool custom_poll_step(custom_poll_t *s, const custom_map_t *map,
                       const char *host, uint16_t port, uint8_t unit, uint64_t now)
{
    if (!s || !map || now < s->next_request_ms) return false;
    size_t point = map->count;
    uint64_t earliest = UINT64_MAX;
    for (size_t i = 0; i < map->count; ++i) {
        if (s->next_due[i] <= now && s->next_due[i] < earliest) {
            earliest = s->next_due[i];
            point = i;
        }
    }
    if (point == map->count) return false;
    const ats_point_def_t *d = &map->defs[point];
    const custom_point_t *p = &map->points[point];
    uint16_t words[MB_MAX_REGISTERS];
    s->last_offset = d->offset;
    ++s->requests;
    mb_error_t error = mb_read_points(host, port, unit, p->function, d->offset,
        d->word_count, ++s->transaction_id, 1200, words, &s->last_result);
    uint64_t completed = now + s->last_result.elapsed_ms;
    s->next_request_ms = completed + 250;
    if (error == MB_OK) {
        s->consecutive_failures = 0;
        ++s->successes;
        s->last_success_ms = completed;
        if (custom_map_apply(map, point, words, d->word_count, completed, &s->values[point]))
            s->acquired[point] = true;
        s->next_due[point] = completed + p->poll_ms;
    } else {
        ++s->failures;
        if (s->consecutive_failures < UINT32_MAX) ++s->consecutive_failures;
        s->values[point].quality = ATS_QUALITY_COMM;
        s->values[point].quality_reason = "Modbus read failed";
        s->next_due[point] = completed + (p->poll_ms > 5000 ? p->poll_ms : 5000);
        if (!s->last_result.exception_code) custom_poll_offline(s, map, completed);
        uint32_t wait = s->consecutive_failures < 6 ? s->consecutive_failures * 5000 : 30000;
        s->next_request_ms = completed + wait;
    }
    return true;
}
