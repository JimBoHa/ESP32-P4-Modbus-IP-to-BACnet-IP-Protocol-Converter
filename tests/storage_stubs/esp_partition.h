#ifndef TEST_STORAGE_ESP_PARTITION_H
#define TEST_STORAGE_ESP_PARTITION_H
#include <stdbool.h>
#include <stddef.h>
#include "esp_flash.h"
typedef unsigned esp_partition_type_t;
typedef unsigned esp_partition_subtype_t;
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_TYPE_ANY 0xff
#define ESP_PARTITION_SUBTYPE_APP_FACTORY 0
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 0x10
#define ESP_PARTITION_SUBTYPE_APP_OTA_1 0x11
#define ESP_PARTITION_SUBTYPE_DATA_OTA 0
#define ESP_PARTITION_SUBTYPE_DATA_PHY 1
#define ESP_PARTITION_SUBTYPE_DATA_NVS 2
#define ESP_PARTITION_SUBTYPE_ANY 0xff

typedef struct {
    esp_flash_t *flash_chip;
    esp_partition_type_t type;
    esp_partition_subtype_t subtype;
    uint32_t address, size, erase_size;
    char label[17];
    bool encrypted, readonly;
} esp_partition_t;
typedef struct test_iterator *esp_partition_iterator_t;
esp_partition_iterator_t esp_partition_find(esp_partition_type_t, esp_partition_subtype_t, const char *);
const esp_partition_t *esp_partition_get(esp_partition_iterator_t);
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t);
void esp_partition_iterator_release(esp_partition_iterator_t);
esp_err_t esp_partition_register_external(esp_flash_t *, size_t, size_t, const char *,
    esp_partition_type_t, esp_partition_subtype_t, const esp_partition_t **);
#endif
