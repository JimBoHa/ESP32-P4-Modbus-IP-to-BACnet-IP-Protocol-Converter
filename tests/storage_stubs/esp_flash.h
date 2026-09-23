#ifndef TEST_STORAGE_ESP_FLASH_H
#define TEST_STORAGE_ESP_FLASH_H
#include <stdint.h>
#include "esp_err.h"
typedef struct { unsigned id; } esp_flash_t;
extern esp_flash_t *esp_flash_default_chip;
esp_err_t esp_flash_get_size(esp_flash_t *, uint32_t *);
esp_err_t esp_flash_read(esp_flash_t *, void *, uint32_t, uint32_t);
#endif
