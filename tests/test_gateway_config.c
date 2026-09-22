/* Production settings parser and cJSON: local-only, no persistence or sockets. */
#include "gateway_config.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char fixture[] = CUSTOM_CSV_HEADER
    "\n42,Room-Temperature,AI,3,0,u16,AB,0.1,0,62,,,,1000,Temperature\n";
static gateway_config_t base, result;
static custom_map_t parsed;
static char error[192];

static char *settings_text(const gateway_config_t *settings, bool csv)
{
    cJSON *json = gateway_config_json(settings, csv);
    assert(json);
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json); assert(text);
    return text;
}

static void assert_rejected(const char *text, size_t length)
{
    gateway_config_t *before = malloc(sizeof(*before));
    custom_map_t *old_map = malloc(sizeof(*old_map));
    assert(before && old_map);
    *before = result; *old_map = parsed;
    memset(error, 0, sizeof(error));
    assert(!gateway_config_parse(text, length, &base, &result, &parsed, error, sizeof(error)));
    assert(error[0]);
    assert(!memcmp(before, &result, sizeof(result)));
    assert(!memcmp(old_map, &parsed, sizeof(parsed)));
    free(before); free(old_map);
}

static void reject_json(cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    assert(text);
    assert_rejected(text, strlen(text));
    free(text); cJSON_Delete(json);
}

static cJSON *defaults_json(void)
{
    cJSON *json = gateway_config_json(&base, true);
    assert(json); return json;
}

static void replace_number(cJSON *json, const char *key, double value)
{
    assert(cJSON_ReplaceItemInObjectCaseSensitive(json, key, cJSON_CreateNumber(value)));
}

static void replace_string(cJSON *json, const char *key, const char *value)
{
    assert(cJSON_ReplaceItemInObjectCaseSensitive(json, key, cJSON_CreateString(value)));
}

static void test_roundtrip(void)
{
    char *text = settings_text(&base, true);
    assert(gateway_config_parse(text, strlen(text), &base, &result, &parsed, error, sizeof(error)));
    assert(!memcmp(&result, &base, sizeof(base)) && !parsed.count && !error[0]);
    free(text);
    gateway_config_t custom = base;
    custom.profile = GW_PROFILE_CUSTOM;
    snprintf(custom.csv, sizeof(custom.csv), "%s", fixture);
    snprintf(custom.device_name, sizeof(custom.device_name), "Custom-Device");
    custom.modbus_unit = 1; custom.device_instance = 74100;
    text = settings_text(&custom, true);
    assert(gateway_config_parse(text, strlen(text), &base, &result, &parsed, error, sizeof(error)));
    assert(!memcmp(&result, &custom, sizeof(custom)) && parsed.count == 1);
    assert(!strcmp(parsed.defs[0].name, "Room-Temperature"));
    assert(parsed.defs[0].name >= parsed.strings && parsed.defs[0].name < parsed.strings + sizeof(parsed.strings));
    /* Boot-time loader passes the same object as base and destination. */
    assert(gateway_config_parse(text, strlen(text), &custom, &custom, &parsed, error, sizeof(error)));
    assert(custom.profile == GW_PROFILE_CUSTOM && parsed.count == 1);
    free(text);
    puts("settings: preset/custom JSON roundtrip, rebased catalog pointers and in-place load passed");
}

static void test_syntax_and_structure(void)
{
    const char *bad[] = {"", "{", "[]", "null", "42", "{} trailing", "{}{}", "{\"x\":1}",
        "{\"device_name\":\"bad\\u0000suffix\"}", "{\"profile\\u0000suffix\":\"custom\"}"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) assert_rejected(bad[i], strlen(bad[i]));
    const char nul[] = "{\"device_name\":\"a\0b\"}";
    assert_rejected(nul, sizeof(nul) - 1);
    cJSON *json = defaults_json();
    cJSON_AddStringToObject(json, "profile", "custom"); reject_json(json);
    json = defaults_json(); cJSON_AddStringToObject(json, "unexpected", "x"); reject_json(json);
    const char *required[] = {"profile", "modbus_host", "modbus_port", "modbus_unit", "device_instance",
        "device_name", "bacnet_port", "expected_firmware", "expected_mac_fragment", "revision"};
    for (size_t i = 0; i < sizeof(required)/sizeof(required[0]); ++i) {
        json = defaults_json(); cJSON_DeleteItemFromObjectCaseSensitive(json, required[i]); reject_json(json);
    }
    char *text = settings_text(&base, false);
    size_t length = strlen(text);
    char *trailing = malloc(length + 8); assert(trailing);
    memcpy(trailing, text, length); memcpy(trailing + length, " \r\n\t", 5);
    assert(gateway_config_parse(trailing, length + 4, &base, &result, &parsed, error, sizeof(error)));
    memcpy(trailing + length, " {}", 4); assert_rejected(trailing, length + 3);
    free(text); free(trailing);
    char *oversized = malloc(GW_CONFIG_MAX_JSON + 1); assert(oversized);
    memset(oversized, ' ', GW_CONFIG_MAX_JSON + 1);
    assert_rejected(oversized, GW_CONFIG_MAX_JSON + 1); free(oversized);
    puts("settings: malformed/trailing JSON, NUL, duplicate/unknown/missing keys and size limits reject atomically passed");
}

static void test_field_limits(void)
{
    struct { const char *key; double value; } cases[] = {
        {"modbus_port",0}, {"modbus_port",65536}, {"modbus_unit",-1}, {"modbus_unit",256},
        {"bacnet_port",0}, {"bacnet_port",65536}, {"device_instance",-1}, {"device_instance",4194303},
        {"expected_firmware",-1}, {"expected_firmware",65536}, {"expected_mac_fragment",-1},
        {"expected_mac_fragment",32768}, {"revision",0}, {"revision",4294967296.0},
        {"modbus_port",502.5}, {"device_instance",INFINITY}
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        cJSON *json = defaults_json(); replace_number(json, cases[i].key, cases[i].value); reject_json(json);
    }
    const char *hosts[] = {"0.0.0.0", "0.1.2.3", "224.0.0.1", "255.255.255.255", "example.com", "192.168.1.999", ""};
    for (size_t i = 0; i < sizeof(hosts)/sizeof(hosts[0]); ++i) {
        cJSON *json = defaults_json(); replace_string(json, "modbus_host", hosts[i]); reject_json(json);
    }
    const char *names[] = {"", "Gateway-Ethernet", "Bad\nName", "Meter\x7f", "Meter\xc3\xa9"};
    for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); ++i) {
        cJSON *json = defaults_json(); replace_string(json, "device_name", names[i]); reject_json(json);
    }
    char too_long[97]; memset(too_long, 'n', sizeof(too_long)); too_long[96] = 0;
    cJSON *json = defaults_json(); replace_string(json, "device_name", too_long); reject_json(json);
    json = defaults_json(); replace_string(json, "profile", "made_up_generator"); reject_json(json);
    json = defaults_json(); replace_string(json, "modbus_port", "502"); reject_json(json);
    json = defaults_json(); assert(cJSON_ReplaceItemInObjectCaseSensitive(json, "csv", cJSON_CreateNumber(123))); reject_json(json);
    char *big_csv = malloc(CUSTOM_MAX_CSV_BYTES + 2); assert(big_csv);
    memset(big_csv, 'x', CUSTOM_MAX_CSV_BYTES + 1); big_csv[CUSTOM_MAX_CSV_BYTES + 1] = 0;
    json = defaults_json(); replace_string(json, "csv", big_csv); reject_json(json); free(big_csv);
    gateway_config_t maximum = base;
    maximum.modbus_port = maximum.bacnet_port = maximum.expected_firmware = 65535;
    maximum.modbus_unit = 255; maximum.device_instance = 4194302; maximum.expected_mac_fragment = 32767;
    maximum.revision = UINT32_MAX;
    memset(maximum.device_name, 'n', sizeof(maximum.device_name) - 1); maximum.device_name[95] = 0;
    char *text = settings_text(&maximum, false);
    assert(gateway_config_parse(text, strlen(text), &base, &result, &parsed, error, sizeof(error)));
    assert(!memcmp(&maximum, &result, sizeof(maximum))); free(text);
    puts("settings: numeric/IP/name/profile/type boundaries and maximum accepted settings passed");
}

static void test_flat_json_preflight(void)
{
    assert_rejected("{\"profile\":{\"nested\":1}}", strlen("{\"profile\":{\"nested\":1}}"));
    assert_rejected("{\"profile\":[1]}", strlen("{\"profile\":[1]}"));
    char deep[20003];
    memset(deep, '[', 10000); deep[10000] = '0'; memset(deep + 10001, ']', 10000); deep[20001] = 0;
    assert_rejected(deep, 20001);
    gateway_config_t custom = base; custom.profile = GW_PROFILE_CUSTOM;
    snprintf(custom.csv, sizeof(custom.csv), "%s\n42,{Room}[Temperature],AI,3,0,u16,AB,0.1,0,62,,,,1000,Literal \\u0000 {braces} [array]\n", CUSTOM_CSV_HEADER);
    char *text = settings_text(&custom, true);
    assert(gateway_json_is_flat(text, strlen(text)) && !gateway_json_has_nul(text, strlen(text)));
    assert(gateway_config_parse(text, strlen(text), &base, &result, &parsed, error, sizeof(error)));
    assert(!strcmp(parsed.defs[0].name, "{Room}[Temperature]"));
    assert(!strcmp(parsed.defs[0].description, "Literal \\u0000 {braces} [array]"));
    free(text);
    puts("settings: deep/nested JSON rejected before recursion; quoted braces and literal backslash-u0000 preserved passed");
}

static void test_retention_and_collisions(void)
{
    snprintf(base.csv, sizeof(base.csv), "%s", fixture);
    cJSON *json = gateway_config_json(&base, false); assert(json);
    char *text = cJSON_PrintUnformatted(json); cJSON_Delete(json); assert(text);
    assert(gateway_config_parse(text, strlen(text), &base, &result, &parsed, error, sizeof(error)));
    assert(result.profile == GW_PROFILE_FULL && parsed.count == 1 && !strcmp(result.csv, fixture));
    free(text);
    json = gateway_config_json(&base, false); replace_string(json, "profile", "custom");
    text = cJSON_PrintUnformatted(json); cJSON_Delete(json); assert(text);
    assert(gateway_config_parse(text, strlen(text), &base, &result, &parsed, error, sizeof(error)));
    assert(result.profile == GW_PROFILE_CUSTOM && parsed.count == 1); free(text);
    json = defaults_json(); replace_string(json, "csv", "invalid,header\n"); reject_json(json);
    json = defaults_json(); replace_string(json, "device_name", "Room-Temperature"); reject_json(json);
    json = defaults_json(); replace_string(json, "profile", "custom");
    replace_string(json, "device_name", "Room-Temperature"); reject_json(json);
    json = defaults_json(); replace_string(json, "device_name", ats_points[0].name); reject_json(json);
    json = defaults_json(); replace_string(json, "profile", "mpac1500_electrical");
    replace_string(json, "device_name", ats_points[0].name); reject_json(json);
    json = defaults_json(); replace_string(json, "profile", "mpac1500_electrical");
    replace_string(json, "device_name", ats_points[30].name);
    text = cJSON_PrintUnformatted(json); cJSON_Delete(json); assert(text);
    assert(gateway_config_parse(text, strlen(text), &base, &result, &parsed, error, sizeof(error)));
    assert(result.profile == GW_PROFILE_ELECTRICAL); free(text);
    json = defaults_json(); replace_string(json, "csv", "");
    text = cJSON_PrintUnformatted(json); cJSON_Delete(json); assert(text);
    assert(gateway_config_parse(text, strlen(text), &base, &result, &parsed, error, sizeof(error)));
    assert(result.csv[0] == 0 && !parsed.count); free(text);
    json = defaults_json(); replace_string(json, "profile", "custom"); replace_string(json, "csv", ""); reject_json(json);
    base.csv[0] = 0;
    json = defaults_json(); replace_string(json, "profile", "custom"); reject_json(json);
    puts("settings: saved CSV retained across profiles, explicit clear, active/inactive map validation and name collisions passed");
}

int main(int argc, char **argv)
{
    gateway_config_defaults(&base);
    memset(&result, 0xa5, sizeof(result)); memset(&parsed, 0x5a, sizeof(parsed));
    test_roundtrip(); test_syntax_and_structure(); test_field_limits(); test_flat_json_preflight(); test_retention_and_collisions();
    if (argc == 2) {
        FILE *file = fopen(argv[1], "rb"); assert(file);
        char *csv = malloc(CUSTOM_MAX_CSV_BYTES + 1); assert(csv);
        size_t length = fread(csv, 1, CUSTOM_MAX_CSV_BYTES + 1, file);
        assert(!ferror(file) && feof(file)); fclose(file);
        assert(custom_map_parse(&parsed, csv, length, error, sizeof(error)));
        assert(parsed.count == 6);
        free(csv);
        puts("shipped downloadable CSV template passes production validation");
    }
    puts("gateway configuration tests passed");
    return 0;
}
