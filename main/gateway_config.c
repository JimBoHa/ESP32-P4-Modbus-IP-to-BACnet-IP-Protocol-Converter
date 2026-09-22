#include "gateway_config.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "lwip/inet.h"
#include "lwip/sockets.h"
#else
#include <arpa/inet.h>
#endif

const char *gateway_profile_id(gw_profile_t profile)
{
    switch (profile) {
    case GW_PROFILE_FULL: return "mpac1500_full";
    case GW_PROFILE_ELECTRICAL: return "mpac1500_electrical";
    case GW_PROFILE_CUSTOM: return "custom";
    default: return "invalid";
    }
}

void gateway_config_defaults(gateway_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->profile = GW_PROFILE_FULL;
    snprintf(c->modbus_host, sizeof(c->modbus_host), "192.0.2.81");
    c->modbus_port = 502;
    c->modbus_unit = 41;
    c->bacnet_port = 47808;
    c->device_instance = 75181;
    c->revision = 1;
    c->expected_firmware = 515;
    snprintf(c->device_name, sizeof(c->device_name), "Kohler-MPAC1500-Gateway");
}

static bool fail(char *error, size_t size, const char *message)
{
    if (error && size) snprintf(error, size, "%s", message);
    return false;
}

static bool number(const cJSON *j, const char *key, double low, double high, double *value)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
        v->valuedouble < low || v->valuedouble > high || floor(v->valuedouble) != v->valuedouble)
        return false;
    *value = v->valuedouble;
    return true;
}

static bool string(const cJSON *j, const char *key, char *out, size_t capacity)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsString(v) || !v->valuestring[0] || strlen(v->valuestring) >= capacity) return false;
    for (const unsigned char *p = (const unsigned char *)v->valuestring; *p; ++p)
        if (*p < 32 || *p > 126) return false;
    snprintf(out, capacity, "%s", v->valuestring);
    return true;
}

bool gateway_json_has_nul(const char *text, size_t length)
{
    if (memchr(text, 0, length)) return true;
    for (size_t i = 0; i < length; ++i) if (text[i] == '\\' && i + 1 < length) {
        if (i + 5 < length && !memcmp(text + i, "\\u0000", 6)) return true;
        ++i;
    }
    return false;
}

bool gateway_json_is_flat(const char *text, size_t length)
{
    /* Both HTTP inputs contain only scalar fields. Reject nesting before
     * recursive cJSON parsing can exhaust the embedded HTTP task stack. */
    bool quoted = false;
    unsigned depth = 0;
    for (size_t i = 0; i < length; ++i) {
        if (quoted && text[i] == '\\') { ++i; continue; }
        if (text[i] == '"') { quoted = !quoted; continue; }
        if (quoted) continue;
        if (text[i] == '{' || text[i] == '[') {
            if (++depth > 1) return false;
        } else if (text[i] == '}' || text[i] == ']') {
            if (!depth) return false;
            --depth;
        }
    }
    return !quoted && !depth;
}

bool gateway_config_parse(const char *text, size_t length, const gateway_config_t *base,
                          gateway_config_t *out, custom_map_t *map, char *error, size_t size)
{
    if (!text || !base || !out || !map || !length || length > GW_CONFIG_MAX_JSON || memchr(text, 0, length))
        return fail(error, size, "Invalid or oversized configuration JSON");
    /* cJSON stores strings as C strings: reject embedded NUL escape anywhere,
     * including property names, before decoding; never silently truncate. */
    if (gateway_json_has_nul(text, length)) return fail(error, size, "NUL characters are not supported");
    if (!gateway_json_is_flat(text, length)) return fail(error, size, "Configuration must contain scalar fields only");
    const char *end = NULL;
    cJSON *j = cJSON_ParseWithLengthOpts(text, length, &end, false);
    if (end) while (end < text + length && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t')) ++end;
    if (!cJSON_IsObject(j) || end != text + length) {
        cJSON_Delete(j);
        return fail(error, size, "Expected one complete JSON object");
    }
    const char *keys[] = {"profile", "modbus_host", "modbus_port", "modbus_unit", "device_instance",
        "device_name", "bacnet_port", "expected_firmware", "expected_mac_fragment", "revision", "csv"};
    unsigned seen = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, j) {
        size_t n;
        for (n = 0; n < sizeof(keys)/sizeof(keys[0]); ++n) if (!strcmp(item->string, keys[n])) break;
        if (n == sizeof(keys)/sizeof(keys[0]) || (seen & (1u << n))) {
            cJSON_Delete(j);
            return fail(error, size, "Unknown or duplicate configuration property");
        }
        seen |= 1u << n;
    }
    gateway_config_t *candidate = malloc(sizeof(*candidate));
    if (!candidate) { cJSON_Delete(j); return fail(error, size, "Out of memory"); }
    *candidate = *base;
    bool ok = false;
    const char *why = "Missing or invalid configuration field";
    char profile[32];
    double value;
    if (!string(j, "profile", profile, sizeof(profile))) goto done;
    if (!strcmp(profile, "mpac1500_full")) candidate->profile = GW_PROFILE_FULL;
    else if (!strcmp(profile, "mpac1500_electrical")) candidate->profile = GW_PROFILE_ELECTRICAL;
    else if (!strcmp(profile, "custom")) candidate->profile = GW_PROFILE_CUSTOM;
    else { why = "Unknown device profile"; goto done; }
    struct in_addr ip;
    if (!string(j, "modbus_host", candidate->modbus_host, sizeof(candidate->modbus_host)) ||
        inet_pton(AF_INET, candidate->modbus_host, &ip) != 1 ||
        ntohl(ip.s_addr) == 0 || (ntohl(ip.s_addr) >> 24) == 0 ||
        (ntohl(ip.s_addr) >> 24) >= 224) { why = "Modbus host must be a unicast IPv4 address"; goto done; }
    if (!string(j, "device_name", candidate->device_name, sizeof(candidate->device_name)) ||
        !strncmp(candidate->device_name, "Gateway-", 8)) {
        why = "Device name must be 1-95 printable ASCII characters and cannot begin Gateway-"; goto done;
    }
#define FIELD(name, low, high) do { if (!number(j, #name, low, high, &value)) { why = "Invalid " #name; goto done; } candidate->name = value; } while (0)
    FIELD(modbus_port, 1, 65535);
    FIELD(modbus_unit, 0, 255);
    FIELD(bacnet_port, 1, 65535);
    FIELD(device_instance, 0, 4194302);
    FIELD(expected_firmware, 0, 65535);
    FIELD(expected_mac_fragment, 0, 32767);
    FIELD(revision, 1, UINT32_MAX);
#undef FIELD
    item = cJSON_GetObjectItemCaseSensitive(j, "csv");
    if (item) {
        if (!cJSON_IsString(item) || strlen(item->valuestring) > CUSTOM_MAX_CSV_BYTES) {
            why = "CSV exceeds 32768 bytes or is not a string"; goto done;
        }
        snprintf(candidate->csv, sizeof(candidate->csv), "%s", item->valuestring);
    }
    if (candidate->profile == GW_PROFILE_CUSTOM && !candidate->csv[0]) {
        why = "Upload a CSV point map before selecting Custom CSV"; goto done;
    }
    /* All saved CSV content is checked, including retained inactive maps. */
    if (candidate->csv[0]) {
        custom_map_t *scratch = calloc(1, sizeof(*scratch));
        if (!scratch) { why = "Out of memory"; goto done; }
        if (!custom_map_parse(scratch, candidate->csv, strlen(candidate->csv), error, size)) {
            free(scratch); why = NULL; goto done;
        }
        for (size_t i = 0; i < scratch->count; ++i) {
            if (!strcmp(scratch->defs[i].name, candidate->device_name)) {
                free(scratch); why = "Device name duplicates a CSV point name"; goto done;
            }
        }
        free(scratch);
    }
    if (candidate->profile != GW_PROFILE_CUSTOM) {
        size_t count = candidate->profile == GW_PROFILE_FULL ? ATS_POINT_COUNT : GW_ELECTRICAL_POINT_COUNT;
        for (size_t i = 0; i < count; ++i) if (!strcmp(ats_points[i].name, candidate->device_name)) {
            why = "Device name duplicates a preset point name"; goto done;
        }
    }
    /* Parse into final destination only once no later validation can fail;
     * parser rebases string pointers and itself preserves output on failure. */
    if (candidate->csv[0] && !custom_map_parse(map, candidate->csv, strlen(candidate->csv), error, size)) {
        why = NULL; goto done;
    }
    if (!candidate->csv[0]) memset(map, 0, sizeof(*map));
    *out = *candidate;
    ok = true;
done:
    if (!ok && why) fail(error, size, why);
    if (ok && error && size) error[0] = 0;
    free(candidate);
    cJSON_Delete(j);
    return ok;
}

cJSON *gateway_config_json(const gateway_config_t *c, bool include_csv)
{
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
#define ADD(call) do { if (!(call)) { cJSON_Delete(j); return NULL; } } while (0)
    ADD(cJSON_AddStringToObject(j, "profile", gateway_profile_id(c->profile)));
    ADD(cJSON_AddStringToObject(j, "modbus_host", c->modbus_host));
    ADD(cJSON_AddStringToObject(j, "device_name", c->device_name));
#define NUM(name) ADD(cJSON_AddNumberToObject(j, #name, c->name))
    NUM(modbus_port); NUM(modbus_unit); NUM(device_instance); NUM(bacnet_port);
    NUM(expected_firmware); NUM(expected_mac_fragment); NUM(revision);
#undef NUM
    if (include_csv) ADD(cJSON_AddStringToObject(j, "csv", c->csv));
#undef ADD
    return j;
}
