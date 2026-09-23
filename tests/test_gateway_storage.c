/* Native partition/NVS/flash stubs: this test never opens a device or network. */
#include "gateway_storage.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static esp_flash_t chip = {1}, other_chip = {2};
esp_flash_t *esp_flash_default_chip = &chip;
static esp_partition_t partitions[40];
static size_t partition_count;
static uint32_t flash_size;
static esp_err_t size_error, read_error, register_error, open_read_error, open_write_error;
static esp_err_t get_error, set_error, commit_error;
static unsigned reads, registrations, read_opens, write_opens, sets, commits, closes;
static uint8_t region[GATEWAY_STORAGE_SIZE];
static uint8_t marker[32], pending[32];
static size_t marker_size, pending_size;
static bool marker_present;
static struct test_iterator { size_t index; bool alive; } iterator;

esp_err_t esp_flash_get_size(esp_flash_t *flash, uint32_t *size)
{
    assert(flash == &chip);
    if (size_error != ESP_OK) return size_error;
    *size = flash_size;
    return ESP_OK;
}

esp_err_t esp_flash_read(esp_flash_t *flash, void *buffer, uint32_t address, uint32_t length)
{
    assert(flash == &chip);
    assert(address >= GATEWAY_STORAGE_OTA_ADDRESS);
    assert((uint64_t)address + length <= GATEWAY_STORAGE_OTA_ADDRESS + GATEWAY_STORAGE_SIZE);
    ++reads;
    if (read_error != ESP_OK) return read_error;
    memcpy(buffer, region + (address - GATEWAY_STORAGE_OTA_ADDRESS), length);
    return ESP_OK;
}

esp_partition_iterator_t esp_partition_find(esp_partition_type_t type,
    esp_partition_subtype_t subtype, const char *label)
{
    assert(type == ESP_PARTITION_TYPE_ANY && subtype == ESP_PARTITION_SUBTYPE_ANY && !label);
    assert(!iterator.alive);
    iterator.index = 0; iterator.alive = partition_count != 0;
    return iterator.alive ? &iterator : NULL;
}
const esp_partition_t *esp_partition_get(esp_partition_iterator_t it)
{
    assert(it == &iterator && it->alive && it->index < partition_count);
    return &partitions[it->index];
}
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t it)
{
    assert(it == &iterator && it->alive);
    if (++it->index == partition_count) { it->alive = false; return NULL; }
    return it;
}
void esp_partition_iterator_release(esp_partition_iterator_t it)
{
    if (it) { assert(it == &iterator && it->alive); it->alive = false; }
}

static esp_partition_t *add(const char *label, unsigned type, unsigned subtype,
                             uint32_t address, uint32_t size)
{
    assert(partition_count < sizeof(partitions) / sizeof(partitions[0]));
    esp_partition_t *p = &partitions[partition_count++];
    *p = (esp_partition_t){.flash_chip=&chip, .type=type, .subtype=subtype,
                           .address=address, .size=size, .erase_size=4096};
    snprintf(p->label, sizeof(p->label), "%s", label);
    return p;
}

esp_err_t esp_partition_register_external(esp_flash_t *flash, size_t address, size_t size,
    const char *label, esp_partition_type_t type, esp_partition_subtype_t subtype,
    const esp_partition_t **result)
{
    ++registrations;
    assert(flash == &chip && address == GATEWAY_STORAGE_OTA_ADDRESS && size == GATEWAY_STORAGE_SIZE);
    assert(type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_DATA_NVS);
    assert(!strcmp(label, GATEWAY_STORAGE_LABEL));
    /* New-NVS initialization may follow registration, so durable ownership
     * must already exist even when registration itself subsequently fails. */
    assert(marker_present);
    if (register_error != ESP_OK) { *result = NULL; return register_error; }
    *result = add(label, type, subtype, (uint32_t)address, (uint32_t)size);
    return ESP_OK;
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(namespace_name, "gw_migration"));
    if (mode == NVS_READONLY) {
        ++read_opens;
        if (open_read_error != ESP_OK) return open_read_error;
    } else {
        assert(mode == NVS_READWRITE); ++write_opens;
        assert(reads == GATEWAY_STORAGE_SIZE / 512U);
        if (open_write_error != ESP_OK) return open_write_error;
    }
    *handle = 7;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size)
{
    assert(handle == 7 && !strcmp(key, "owned_v1"));
    if (get_error != ESP_OK) return get_error;
    if (!marker_present) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < marker_size) { *size = marker_size; return ESP_ERR_NVS_INVALID_LENGTH; }
    memcpy(value, marker, marker_size); *size = marker_size;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size)
{
    assert(handle == 7 && !strcmp(key, "owned_v1") && size == 16);
    ++sets;
    if (set_error != ESP_OK) return set_error;
    memcpy(pending, value, size); pending_size = size;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 7); ++commits;
    if (commit_error != ESP_OK) return commit_error;
    memcpy(marker, pending, pending_size); marker_size = pending_size; marker_present = true;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 7); ++closes; }

static void reset(void)
{
    esp_flash_default_chip = &chip;
    partition_count = 0; iterator.alive = false; flash_size = 32U * 1024U * 1024U;
    size_error = read_error = register_error = open_read_error = open_write_error = ESP_OK;
    get_error = set_error = commit_error = ESP_OK;
    reads = registrations = read_opens = write_opens = sets = commits = closes = 0;
    memset(region, 0xff, sizeof(region));
    marker_present = false; marker_size = pending_size = 0;
}
static void dual(void)
{
    add("nvs", 1, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x9000, 0x6000);
    add("otadata", 1, ESP_PARTITION_SUBTYPE_DATA_OTA, 0xf000, 0x2000);
    add("phy_init", 1, ESP_PARTITION_SUBTYPE_DATA_PHY, 0x11000, 0x1000);
    add("ota_0", 0, ESP_PARTITION_SUBTYPE_APP_OTA_0, 0x20000, 0x400000);
    add("ota_1", 0, ESP_PARTITION_SUBTYPE_APP_OTA_1, 0x420000, 0x400000);
}
static void factory(void)
{
    add("nvs", 1, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x9000, 0x6000);
    add("phy_init", 1, ESP_PARTITION_SUBTYPE_DATA_PHY, 0xf000, 0x1000);
    add("factory", 0, ESP_PARTITION_SUBTYPE_APP_FACTORY, 0x10000, 0x400000);
    add("gateway_cfg", 1, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x410000, 0x40000);
}
static void untouched(void)
{
    assert(!registrations && !reads && !read_opens && !write_opens && !sets && !commits);
    assert(!iterator.alive);
}

static void test_existing_and_migration(void)
{
    reset(); factory();
    assert(gateway_storage_prepare() == ESP_OK); untouched();
    assert(gateway_storage_prepare() == ESP_OK); untouched();
    partitions[3].readonly = true;
    assert(gateway_storage_prepare() == ESP_ERR_INVALID_STATE); untouched();
    reset(); factory(); partitions[3].subtype = ESP_PARTITION_SUBTYPE_DATA_PHY;
    assert(gateway_storage_prepare() == ESP_ERR_INVALID_STATE); untouched();
    reset(); factory(); partitions[3].address += 4096;
    assert(gateway_storage_prepare() == ESP_ERR_INVALID_STATE); untouched();
    reset(); factory(); partitions[3].size -= 4096;
    assert(gateway_storage_prepare() == ESP_ERR_INVALID_STATE); untouched();
    reset(); factory(); partitions[3].encrypted = true;
    assert(gateway_storage_prepare() == ESP_ERR_INVALID_STATE); untouched();
    reset(); dual();
    assert(gateway_storage_prepare() == ESP_OK);
    assert(registrations == 1 && reads == GATEWAY_STORAGE_SIZE / 512U);
    assert(marker_present && sets == 1 && commits == 1 && partition_count == 6);
    assert(marker_size == 16 && !memcmp(marker, "GWCF", 4));
    unsigned previous_reads = reads, previous_opens = read_opens;
    assert(gateway_storage_prepare() == ESP_OK);
    assert(registrations == 1 && reads == previous_reads && read_opens == previous_opens);
    /* Reboot drops the RAM descriptor, while committed default-NVS marker and
     * acquired gateway data persist. No blank scan or erasure on reuse. */
    partition_count = 5; reads = registrations = read_opens = write_opens = sets = commits = 0;
    region[0] = 0x42;
    assert(gateway_storage_prepare() == ESP_OK);
    assert(registrations == 1 && !reads && read_opens == 1 && !write_opens && !sets && !commits);
    reset(); dual(); add("gateway_cfg", 1, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x820000, 0x40000);
    assert(gateway_storage_prepare() == ESP_OK); untouched();
}

static void test_layout_rejections(void)
{
    reset(); dual(); flash_size = 0x820000;
    assert(gateway_storage_prepare() == ESP_ERR_INVALID_SIZE); untouched();
    reset(); dual(); flash_size = 0x860000;
    assert(gateway_storage_prepare() == ESP_OK); /* Exact end is in bounds. */
    reset(); dual(); partitions[3].address += 4096;
    assert(gateway_storage_prepare() != ESP_OK); untouched();
    reset(); dual(); partitions[4].size -= 4096;
    assert(gateway_storage_prepare() == ESP_ERR_NOT_SUPPORTED); untouched();
    reset(); dual(); partitions[1].size -= 4096;
    assert(gateway_storage_prepare() == ESP_ERR_NOT_SUPPORTED); untouched();
    reset(); dual(); partitions[0].readonly = true;
    assert(gateway_storage_prepare() == ESP_ERR_NOT_SUPPORTED); untouched();
    reset(); dual(); partitions[0].address = UINT32_MAX - 4095U; partitions[0].size = 0x10000;
    assert(gateway_storage_prepare() == ESP_ERR_INVALID_SIZE); untouched();
    reset(); dual(); add("unknown", 1, 0x80, 0x830000, 4096);
    assert(gateway_storage_prepare() == ESP_ERR_INVALID_STATE); untouched();
    reset(); dual(); add("unknown", 1, 0x80, 0x81f000, 0x2000);
    assert(gateway_storage_prepare() == ESP_ERR_INVALID_STATE); untouched();
    reset(); dual(); add("gateway_cfg", 1, 2, 0x410000, 0x40000);
    assert(gateway_storage_prepare() == ESP_ERR_INVALID_STATE); untouched();
    reset(); dual(); add("gateway_cfg", 1, 2, 0x820000, 0x40000)->flash_chip = &other_chip;
    assert(gateway_storage_prepare() == ESP_ERR_INVALID_STATE); untouched();
    reset(); factory(); add("gateway_cfg", 1, 2, 0x820000, 0x40000);
    assert(gateway_storage_prepare() == ESP_ERR_INVALID_STATE); untouched();
    reset(); dual(); add("external", 1, 2, 0x820000, 0x40000)->flash_chip = &other_chip;
    assert(gateway_storage_prepare() == ESP_OK); /* Different chips do not overlap. */
    reset(); dual(); add("after", 1, 0x80, 0x860000, 4096);
    assert(gateway_storage_prepare() == ESP_OK); /* Adjacent, not overlapping. */
    reset(); dual();
    for (unsigned i = 0; i < 28; ++i) add("extra", 1, 0x80, 0x900000 + i * 4096, 4096);
    assert(gateway_storage_prepare() == ESP_ERR_INVALID_SIZE); untouched();
    reset(); factory(); --partition_count;
    assert(gateway_storage_prepare() == ESP_ERR_NOT_SUPPORTED); untouched();
    reset();
    assert(gateway_storage_prepare() == ESP_ERR_NOT_SUPPORTED); untouched();
    reset(); dual(); esp_flash_default_chip = NULL;
    assert(gateway_storage_prepare() == ESP_ERR_INVALID_STATE); untouched();
}

static void test_ownership_and_errors(void)
{
    reset(); dual(); region[sizeof(region) - 1] = 0;
    assert(gateway_storage_prepare() == ESP_ERR_INVALID_STATE);
    assert(reads == GATEWAY_STORAGE_SIZE / 512U && !sets && !commits && !registrations && !write_opens);
    reset(); dual(); region[0] = 0;
    assert(gateway_storage_prepare() == ESP_ERR_INVALID_STATE);
    assert(reads == 1 && !sets && !commits && !registrations && !write_opens);
    reset(); dual(); open_read_error = ESP_ERR_NVS_NOT_FOUND;
    assert(gateway_storage_prepare() == ESP_OK);
    assert(marker_present && write_opens == 1 && sets == 1 && commits == 1);
}

static void test_errors(void)
{
    reset(); dual(); size_error = ESP_FAIL;
    assert(gateway_storage_prepare() == ESP_FAIL); untouched();
    reset(); dual(); open_read_error = ESP_FAIL;
    assert(gateway_storage_prepare() == ESP_FAIL); assert(!reads && !sets && !registrations);
    reset(); dual(); get_error = ESP_FAIL;
    assert(gateway_storage_prepare() == ESP_FAIL); assert(closes == 1 && !reads && !sets && !registrations);
    reset(); dual(); marker_present = true; marker_size = 16; memset(marker, 0, sizeof(marker));
    assert(gateway_storage_prepare() == ESP_ERR_INVALID_STATE); assert(!reads && !sets && !registrations);
    reset(); dual(); marker_present = true; marker_size = 8;
    assert(gateway_storage_prepare() == ESP_ERR_INVALID_STATE); assert(!reads && !sets && !registrations);
    reset(); dual(); marker_present = true; marker_size = 32;
    assert(gateway_storage_prepare() == ESP_ERR_NVS_INVALID_LENGTH); assert(!reads && !sets && !registrations);
    reset(); dual(); read_error = ESP_FAIL;
    assert(gateway_storage_prepare() == ESP_FAIL); assert(reads == 1 && !sets && !registrations);
    reset(); dual(); open_write_error = ESP_FAIL;
    assert(gateway_storage_prepare() == ESP_FAIL); assert(write_opens == 1 && !sets && !registrations);
    reset(); dual(); set_error = ESP_FAIL;
    assert(gateway_storage_prepare() == ESP_FAIL); assert(sets == 1 && !commits && !registrations);
    reset(); dual(); commit_error = ESP_FAIL;
    assert(gateway_storage_prepare() == ESP_FAIL); assert(commits == 1 && !registrations);
    reset(); dual(); register_error = ESP_ERR_INVALID_ARG;
    assert(gateway_storage_prepare() == ESP_ERR_INVALID_ARG);
    assert(marker_present && registrations == 1 && partition_count == 5);
    reset(); dual(); register_error = ESP_ERR_NO_MEM;
    assert(gateway_storage_prepare() == ESP_ERR_NO_MEM);
    assert(marker_present && registrations == 1 && partition_count == 5);
    /* Retrying after registration OOM preserves ownership and does not rescan. */
    register_error = ESP_OK; reads = 0;
    assert(gateway_storage_prepare() == ESP_OK);
    assert(registrations == 2 && reads == 0 && sets == 1 && commits == 1);
}

int main(void)
{
    test_existing_and_migration(); test_layout_rejections(); test_ownership_and_errors(); test_errors();
    puts("Storage migration: exact layout/bounds/overlap checks; blank-only ownership claim; persistent reuse; commit/registration failures passed");
    return 0;
}
