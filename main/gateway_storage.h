#ifndef GATEWAY_STORAGE_H
#define GATEWAY_STORAGE_H
#include "esp_err.h"

#define GATEWAY_STORAGE_LABEL "gateway_cfg"
#define GATEWAY_STORAGE_FACTORY_ADDRESS 0x410000U
#define GATEWAY_STORAGE_OTA_ADDRESS 0x820000U
#define GATEWAY_STORAGE_SIZE 0x40000U

/* Call after nvs_flash_init(), before initializing gateway_cfg NVS, from one
 * startup task. Existing factory-table storage is validated and reused.
 * For the recognized two-slot OTA layout, register gateway_cfg in RAM only;
 * never modify the physical partition table, bootloader, OTA slots or otadata.
 * First migration requires an entirely blank region, then commits an ownership
 * marker in a dedicated default-NVS namespace. Subsequent boots require the
 * same marker before reusing off-table storage. No unknown region is erased.
 * Any error must disable polling and must prevent marking pending OTA valid. */
esp_err_t gateway_storage_prepare(void);
#endif
