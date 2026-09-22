#ifndef GATEWAY_WEB_H
#define GATEWAY_WEB_H
#include "gateway_config.h"

/* Load a single versioned NVS blob. Does not erase invalid saved data.
 * Nonempty error disables polling until a valid config is saved via web. */
void gateway_web_load(gateway_config_t *config, custom_map_t *map,
                      char *error, size_t error_size);
void gateway_web_start(const gateway_config_t *config, const custom_map_t *map,
                       const char *config_error, cJSON *(*status)(void),
                       cJSON *(*points)(void));
#endif
