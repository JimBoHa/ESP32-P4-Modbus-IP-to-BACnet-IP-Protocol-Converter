#ifndef TEST_OTA_PARTITION_H
#define TEST_OTA_PARTITION_H
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 0x10
#define ESP_PARTITION_SUBTYPE_APP_OTA_1 0x11
typedef struct { int type, subtype; uint32_t address, size; char label[17]; } esp_partition_t;
esp_err_t esp_partition_get_sha256(const esp_partition_t *, uint8_t *);
#endif
