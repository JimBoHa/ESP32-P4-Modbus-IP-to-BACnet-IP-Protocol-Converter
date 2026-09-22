#ifndef GATEWAY_CONFIG_H
#define GATEWAY_CONFIG_H
#include "custom_map.h"
#include "cJSON.h"

#define GW_CONFIG_MAX_JSON (96u * 1024u)
#define GW_ELECTRICAL_POINT_COUNT 24u

typedef enum { GW_PROFILE_FULL, GW_PROFILE_ELECTRICAL, GW_PROFILE_CUSTOM } gw_profile_t;
typedef struct {
    gw_profile_t profile;
    char modbus_host[16];
    uint16_t modbus_port, bacnet_port;
    uint8_t modbus_unit;
    uint32_t device_instance, revision;
    char device_name[96];
    uint16_t expected_firmware, expected_mac_fragment;
    char csv[CUSTOM_MAX_CSV_BYTES + 1];
} gateway_config_t;

const char *gateway_profile_id(gw_profile_t profile);
void gateway_config_defaults(gateway_config_t *config);
bool gateway_json_has_nul(const char *text, size_t length);
bool gateway_json_is_flat(const char *text, size_t length);
/* Strict complete settings payload, with optional csv (retains base CSV).
 * Output and map are unchanged on error. No I/O or state mutation. */
bool gateway_config_parse(const char *json, size_t length,
                          const gateway_config_t *base,
                          gateway_config_t *out, custom_map_t *map,
                          char *error, size_t error_size);
cJSON *gateway_config_json(const gateway_config_t *config, bool include_csv);
#endif
