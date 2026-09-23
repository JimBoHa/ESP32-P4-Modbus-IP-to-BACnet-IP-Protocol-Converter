#include "gateway_storage.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_flash.h"
#include "esp_partition.h"
#include "nvs.h"

#define MAX_PRIMARY_PARTITIONS 32U
#define SECTOR_SIZE 4096U
#define MARKER_NAMESPACE "gw_migration"
#define MARKER_KEY "owned_v1"

/* Fixed byte encoding: GWCF, schema1, little-endian offset0x820000,size0x40000.
 * NVS itself supplies blob integrity checking and atomic commit behavior. */
static const uint8_t ownership_marker[] = {
    'G', 'W', 'C', 'F', 1, 0, 0, 0,
    0, 0, 0x82, 0, 0, 0, 4, 0
};

typedef struct {
    const esp_partition_t *items[MAX_PRIMARY_PARTITIONS];
    size_t count;
    const esp_partition_t *gateway;
} layout_t;

static bool overlap(uint32_t a, uint32_t as, uint32_t b, uint32_t bs)
{
    return (uint64_t)a < (uint64_t)b + bs && (uint64_t)b < (uint64_t)a + as;
}

static esp_err_t inventory(layout_t *layout, uint32_t flash_size)
{
    memset(layout, 0, sizeof(*layout));
    esp_partition_iterator_t iterator = esp_partition_find(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    esp_err_t error = ESP_OK;
    while (iterator) {
        const esp_partition_t *p = esp_partition_get(iterator);
        if (!p) { error = ESP_ERR_INVALID_STATE; break; }
        if (!strcmp(p->label, GATEWAY_STORAGE_LABEL)) {
            if (layout->gateway || p->flash_chip != esp_flash_default_chip) {
                error = ESP_ERR_INVALID_STATE; break;
            }
            layout->gateway = p;
        }
        if (p->flash_chip == esp_flash_default_chip) {
            if (!p->size || (p->address % SECTOR_SIZE) || (p->size % SECTOR_SIZE) ||
                (uint64_t)p->address + p->size > flash_size ||
                layout->count == MAX_PRIMARY_PARTITIONS) {
                error = ESP_ERR_INVALID_SIZE; break;
            }
            for (size_t i = 0; i < layout->count; ++i) {
                const esp_partition_t *other = layout->items[i];
                if (overlap(p->address, p->size, other->address, other->size)) {
                    error = ESP_ERR_INVALID_STATE; break;
                }
            }
            if (error != ESP_OK) break;
            layout->items[layout->count++] = p;
        }
        iterator = esp_partition_next(iterator);
    }
    esp_partition_iterator_release(iterator);
    return error;
}

static bool exact(const layout_t *layout, esp_partition_type_t type,
                  esp_partition_subtype_t subtype, uint32_t address,
                  uint32_t size, const char *label)
{
    unsigned matches = 0;
    for (size_t i = 0; i < layout->count; ++i) {
        const esp_partition_t *p = layout->items[i];
        if (p->type == type && p->subtype == subtype) {
            /* The default NVS and configuration NVS share a subtype. */
            if (label && strcmp(p->label, label)) continue;
            if (p->address != address || p->size != size || p->readonly) return false;
            ++matches;
        }
    }
    return matches == 1;
}

static bool factory_layout(const layout_t *layout)
{
    return exact(layout, ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY,
                  0x10000U, 0x400000U, NULL) &&
        exact(layout, ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS,
              0x9000U, 0x6000U, "nvs") &&
        exact(layout, ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_PHY,
              0xf000U, 0x1000U, NULL);
}

static bool ota_layout(const layout_t *layout)
{
    /* Exact deployed layout; labels on OTA/application/PHY partitions may vary. */
    return exact(layout, ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0,
                  0x20000U, 0x400000U, NULL) &&
        exact(layout, ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1,
              0x420000U, 0x400000U, NULL) &&
        exact(layout, ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS,
              0x9000U, 0x6000U, "nvs") &&
        exact(layout, ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA,
              0xf000U, 0x2000U, NULL) &&
        exact(layout, ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_PHY,
              0x11000U, 0x1000U, NULL);
}

static esp_err_t check_marker(bool *owned)
{
    *owned = false;
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(MARKER_NAMESPACE, NVS_READONLY, &nvs);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (error != ESP_OK) return error;
    uint8_t marker[sizeof(ownership_marker)];
    size_t size = sizeof(marker);
    error = nvs_get_blob(nvs, MARKER_KEY, marker, &size);
    nvs_close(nvs);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (error != ESP_OK) return error;
    if (size != sizeof(marker) || memcmp(marker, ownership_marker, sizeof(marker)))
        return ESP_ERR_INVALID_STATE;
    *owned = true;
    return ESP_OK;
}

static esp_err_t claim_blank_region(void)
{
    uint8_t buffer[512];
    for (uint32_t offset = 0; offset < GATEWAY_STORAGE_SIZE; offset += sizeof(buffer)) {
        esp_err_t error = esp_flash_read(esp_flash_default_chip, buffer,
            GATEWAY_STORAGE_OTA_ADDRESS + offset, sizeof(buffer));
        if (error != ESP_OK) return error;
        for (size_t i = 0; i < sizeof(buffer); ++i)
            if (buffer[i] != 0xff) return ESP_ERR_INVALID_STATE;
    }
    /* Commit ownership before initializing new NVS. A power failure after this
     * commit is safe: only this verified blank region has been claimed. */
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(MARKER_NAMESPACE, NVS_READWRITE, &nvs);
    if (error != ESP_OK) return error;
    error = nvs_set_blob(nvs, MARKER_KEY, ownership_marker, sizeof(ownership_marker));
    if (error == ESP_OK) error = nvs_commit(nvs);
    nvs_close(nvs);
    return error;
}

esp_err_t gateway_storage_prepare(void)
{
    if (!esp_flash_default_chip) return ESP_ERR_INVALID_STATE;
    uint32_t flash_size;
    esp_err_t error = esp_flash_get_size(esp_flash_default_chip, &flash_size);
    if (error != ESP_OK) return error;
    layout_t layout;
    error = inventory(&layout, flash_size);
    if (error != ESP_OK) return error;
    if (layout.gateway) {
        const esp_partition_t *p = layout.gateway;
        if (p->type != ESP_PARTITION_TYPE_DATA || p->subtype != ESP_PARTITION_SUBTYPE_DATA_NVS ||
            p->size != GATEWAY_STORAGE_SIZE || p->readonly || p->encrypted)
            return ESP_ERR_INVALID_STATE;
        if (p->address == GATEWAY_STORAGE_FACTORY_ADDRESS && factory_layout(&layout)) return ESP_OK;
        if (p->address == GATEWAY_STORAGE_OTA_ADDRESS && ota_layout(&layout)) return ESP_OK;
        return ESP_ERR_INVALID_STATE;
    }
    if (!ota_layout(&layout)) return ESP_ERR_NOT_SUPPORTED;
    if ((uint64_t)GATEWAY_STORAGE_OTA_ADDRESS + GATEWAY_STORAGE_SIZE > flash_size)
        return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < layout.count; ++i) {
        const esp_partition_t *p = layout.items[i];
        if (overlap(GATEWAY_STORAGE_OTA_ADDRESS, GATEWAY_STORAGE_SIZE, p->address, p->size))
            return ESP_ERR_INVALID_STATE;
    }
    bool owned;
    error = check_marker(&owned);
    if (error != ESP_OK) return error;
    if (!owned) {
        error = claim_blank_region();
        if (error != ESP_OK) return error;
    }
    /* Supported IDF API: NULL or esp_flash_default_chip designates primary
     * flash. It performs a second same-chip overlap check under its list lock.
     * Registration adds a RAM descriptor; it writes no partition table bytes. */
    const esp_partition_t *registered = NULL;
    error = esp_partition_register_external(esp_flash_default_chip,
        GATEWAY_STORAGE_OTA_ADDRESS, GATEWAY_STORAGE_SIZE, GATEWAY_STORAGE_LABEL,
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, &registered);
    if (error != ESP_OK) return error;
    if (!registered || registered->address != GATEWAY_STORAGE_OTA_ADDRESS ||
        registered->size != GATEWAY_STORAGE_SIZE || registered->flash_chip != esp_flash_default_chip ||
        registered->type != ESP_PARTITION_TYPE_DATA || registered->subtype != ESP_PARTITION_SUBTYPE_DATA_NVS ||
        registered->readonly || registered->encrypted || strcmp(registered->label, GATEWAY_STORAGE_LABEL))
        return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}
