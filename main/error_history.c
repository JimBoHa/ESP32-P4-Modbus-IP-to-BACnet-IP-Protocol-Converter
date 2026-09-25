#include "error_history.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define HISTORY_MAGIC UINT32_C(0x4d424548)
#define HISTORY_VERSION 1U

typedef struct {
    uint64_t sequence, uptime_ms;
    int64_t utc_ms;
    uint32_t boot_id;
    error_history_request_t request;
    mb_result_t result;
} history_event_t;

/* This is a versioned, fixed-size local flash format, not a portable export.
 * Every new structure is zeroed so padding is deterministic. NVS commits the
 * entire blob atomically; an additional checksum catches malformed snapshots. */
typedef struct {
    uint32_t magic, version, size, checksum;
    uint32_t boot_id, count, next, reserved;
    uint64_t total_errors;
    history_event_t events[ERROR_HISTORY_CAPACITY];
} history_blob_t;

typedef struct {
    SemaphoreHandle_t lock;
    nvs_handle_t nvs;
    history_blob_t blob;
    bool initialized, writable, attempted;
    uint64_t generation, saved_generation, last_attempt_ms;
    char persistence_error[128];
} history_runtime_t;

static history_runtime_t history;

static uint32_t blob_checksum(const history_blob_t *blob)
{
    const unsigned char *bytes = (const unsigned char *)blob;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < sizeof(*blob); ++i) {
        unsigned char byte = (i >= offsetof(history_blob_t, checksum) &&
            i < offsetof(history_blob_t, checksum) + sizeof(blob->checksum)) ? 0 : bytes[i];
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
    }
    return ~crc;
}

static bool valid_blob(const history_blob_t *blob)
{
    if (blob->magic != HISTORY_MAGIC || blob->version != HISTORY_VERSION ||
        blob->size != sizeof(*blob) || blob->checksum != blob_checksum(blob) ||
        blob->count > ERROR_HISTORY_CAPACITY || blob->next >= ERROR_HISTORY_CAPACITY ||
        blob->total_errors < blob->count ||
        blob->count != (blob->total_errors < ERROR_HISTORY_CAPACITY ?
                       blob->total_errors : ERROR_HISTORY_CAPACITY) ||
        blob->next != blob->total_errors % ERROR_HISTORY_CAPACITY) return false;
    for (size_t age = 0; age < blob->count; ++age) {
        const history_event_t *event = &blob->events[
            (blob->next + ERROR_HISTORY_CAPACITY - 1U - age) % ERROR_HISTORY_CAPACITY];
        if (event->sequence != blob->total_errors - age || !event->boot_id ||
            event->boot_id > blob->boot_id || event->utc_ms < 0 ||
            event->request.host[sizeof(event->request.host) - 1] ||
            event->request.profile[sizeof(event->request.profile) - 1] ||
            event->request.phase[sizeof(event->request.phase) - 1] ||
            event->result.error <= MB_OK || event->result.error > MB_ERR_EXCEPTION)
            return false;
    }
    return true;
}

static void storage_error(const char *operation, esp_err_t error)
{
    snprintf(history.persistence_error, sizeof(history.persistence_error),
             "%s: 0x%x", operation, (unsigned)error);
}

/* Called by one worker only. Snapshot and write outside the short ring lock;
 * a new event arriving during a flash write remains dirty after that write. */
static void flush_pending(void)
{
    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000U;
    xSemaphoreTake(history.lock, portMAX_DELAY);
    if (!history.writable || history.saved_generation == history.generation ||
        (history.attempted && now_ms - history.last_attempt_ms < ERROR_HISTORY_FLUSH_INTERVAL_MS)) {
        xSemaphoreGive(history.lock);
        return;
    }
    history.attempted = true;
    history.last_attempt_ms = now_ms;
    xSemaphoreGive(history.lock);

    history_blob_t *snapshot = malloc(sizeof(*snapshot));
    if (!snapshot) {
        xSemaphoreTake(history.lock, portMAX_DELAY);
        storage_error("History snapshot allocation", ESP_ERR_NO_MEM);
        xSemaphoreGive(history.lock);
        return;
    }
    xSemaphoreTake(history.lock, portMAX_DELAY);
    *snapshot = history.blob;
    uint64_t generation = history.generation;
    xSemaphoreGive(history.lock);
    snapshot->checksum = blob_checksum(snapshot);
    esp_err_t err = nvs_set_blob(history.nvs, "history_v1", snapshot, sizeof(*snapshot));
    if (err == ESP_OK) err = nvs_commit(history.nvs);
    free(snapshot);

    xSemaphoreTake(history.lock, portMAX_DELAY);
    if (err == ESP_OK) {
        history.saved_generation = generation;
        history.persistence_error[0] = 0;
    } else storage_error("History persistence", err);
    xSemaphoreGive(history.lock);
}

static void persistence_task(void *unused)
{
    (void)unused;
    for (;;) {
        flush_pending();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void error_history_init(void)
{
    if (history.initialized) return;
    history.initialized = true;
    history.blob.magic = HISTORY_MAGIC;
    history.blob.version = HISTORY_VERSION;
    history.blob.size = sizeof(history.blob);
    history.lock = xSemaphoreCreateMutex();
    if (!history.lock) {
        storage_error("History mutex allocation", ESP_ERR_NO_MEM);
        return;
    }

    esp_err_t err = nvs_open_from_partition("gateway_cfg", "mb_errors", NVS_READWRITE, &history.nvs);
    if (err != ESP_OK) storage_error("History storage open", err);
    else {
        size_t length = 0;
        err = nvs_get_blob(history.nvs, "history_v1", NULL, &length);
        if (err == ESP_ERR_NVS_NOT_FOUND) history.writable = true;
        else if (err != ESP_OK) storage_error("History storage read", err);
        else if (length != sizeof(history.blob))
            snprintf(history.persistence_error, sizeof(history.persistence_error),
                     "Stored history has incompatible size; preserved without writes");
        else {
            history_blob_t *stored = malloc(sizeof(*stored));
            if (!stored) storage_error("History load allocation", ESP_ERR_NO_MEM);
            else {
                err = nvs_get_blob(history.nvs, "history_v1", stored, &length);
                if (err != ESP_OK) storage_error("History storage read", err);
                else if (length != sizeof(*stored) || !valid_blob(stored))
                    snprintf(history.persistence_error, sizeof(history.persistence_error),
                             "Stored history is corrupt or incompatible; preserved without writes");
                else { history.blob = *stored; history.writable = true; }
                free(stored);
            }
        }
        if (!history.writable) nvs_close(history.nvs);
    }
    if (history.blob.boot_id == UINT32_MAX) {
        if (history.writable) nvs_close(history.nvs);
        history.writable = false;
        snprintf(history.persistence_error, sizeof(history.persistence_error), "History boot counter exhausted");
    } else ++history.blob.boot_id;
    ++history.generation; /* Persist the new boot sequence even without errors. */
    if (history.writable &&
        xTaskCreate(persistence_task, "modbus_history", 4096, NULL, 2, NULL) != pdPASS) {
        history.writable = false;
        storage_error("History worker allocation", ESP_ERR_NO_MEM);
        nvs_close(history.nvs);
    }
}

static void copy_text(char *destination, const char *source, size_t capacity)
{
    size_t i = 0;
    for (; i + 1 < capacity && source[i]; ++i) destination[i] = source[i];
    destination[i] = 0;
}

void error_history_record(const error_history_request_t *request,
                          const mb_result_t *result,
                          uint64_t uptime_ms, int64_t utc_ms)
{
    if (!request || !result || result->error <= MB_OK || result->error > MB_ERR_EXCEPTION ||
        !history.lock) return;
    xSemaphoreTake(history.lock, portMAX_DELAY);
    history_event_t *event = &history.blob.events[history.blob.next];
    memset(event, 0, sizeof(*event));
    event->sequence = ++history.blob.total_errors;
    event->uptime_ms = uptime_ms;
    event->utc_ms = utc_ms > 0 ? utc_ms : 0;
    event->boot_id = history.blob.boot_id;
    event->request.port = request->port;
    event->request.offset = request->offset;
    event->request.quantity = request->quantity;
    event->request.transaction_id = request->transaction_id;
    event->request.unit = request->unit;
    event->request.function = request->function;
    event->request.config_revision = request->config_revision;
    copy_text(event->request.host, request->host, sizeof(event->request.host));
    copy_text(event->request.profile, request->profile, sizeof(event->request.profile));
    copy_text(event->request.phase, request->phase, sizeof(event->request.phase));
    event->result.error = result->error;
    event->result.exception_code = result->exception_code;
    event->result.elapsed_ms = result->elapsed_ms;
    event->result.system_error = result->system_error;
    history.blob.next = (history.blob.next + 1U) % ERROR_HISTORY_CAPACITY;
    if (history.blob.count < ERROR_HISTORY_CAPACITY) ++history.blob.count;
    ++history.generation;
    xSemaphoreGive(history.lock);
}

cJSON *error_history_json(bool clock_synchronized)
{
    history_blob_t *snapshot = malloc(sizeof(*snapshot));
    if (!snapshot) return NULL;
    char persistence_error[sizeof(history.persistence_error)];
    if (history.lock) xSemaphoreTake(history.lock, portMAX_DELAY);
    *snapshot = history.blob;
    bool pending = history.generation != history.saved_generation;
    memcpy(persistence_error, history.persistence_error, sizeof(persistence_error));
    if (history.lock) xSemaphoreGive(history.lock);

    cJSON *json = cJSON_CreateObject();
    cJSON *events = json ? cJSON_AddArrayToObject(json, "events") : NULL;
    if (!events) goto allocation_failed;
#define JSON_REQUIRE(call) do { if (!(call)) goto allocation_failed; } while (0)
    JSON_REQUIRE(cJSON_AddNumberToObject(json, "capacity", ERROR_HISTORY_CAPACITY));
    JSON_REQUIRE(cJSON_AddNumberToObject(json, "count", snapshot->count));
    JSON_REQUIRE(cJSON_AddNumberToObject(json, "total_errors", (double)snapshot->total_errors));
    JSON_REQUIRE(cJSON_AddNumberToObject(json, "overwritten", (double)(snapshot->total_errors - snapshot->count)));
    JSON_REQUIRE(cJSON_AddNumberToObject(json, "boot_id", snapshot->boot_id));
    JSON_REQUIRE(cJSON_AddBoolToObject(json, "clock_synchronized", clock_synchronized));
    JSON_REQUIRE(cJSON_AddBoolToObject(json, "pending_persistence", pending));
    JSON_REQUIRE(cJSON_AddStringToObject(json, "persistence_error", persistence_error));
    for (size_t age = 0; age < snapshot->count; ++age) {
        const history_event_t *event = &snapshot->events[
            (snapshot->next + ERROR_HISTORY_CAPACITY - 1U - age) % ERROR_HISTORY_CAPACITY];
        cJSON *item = cJSON_CreateObject();
        if (!item) goto allocation_failed;
        if (!cJSON_AddItemToArray(events, item)) { cJSON_Delete(item); goto allocation_failed; }
        JSON_REQUIRE(cJSON_AddNumberToObject(item, "sequence", (double)event->sequence));
        JSON_REQUIRE(cJSON_AddNumberToObject(item, "boot_id", event->boot_id));
        JSON_REQUIRE(cJSON_AddNumberToObject(item, "uptime_ms", (double)event->uptime_ms));
        if (event->utc_ms) JSON_REQUIRE(cJSON_AddNumberToObject(item, "utc_ms", (double)event->utc_ms));
        else JSON_REQUIRE(cJSON_AddNullToObject(item, "utc_ms"));
        JSON_REQUIRE(cJSON_AddStringToObject(item, "host", event->request.host));
        JSON_REQUIRE(cJSON_AddNumberToObject(item, "port", event->request.port));
        JSON_REQUIRE(cJSON_AddNumberToObject(item, "unit", event->request.unit));
        JSON_REQUIRE(cJSON_AddNumberToObject(item, "function", event->request.function));
        JSON_REQUIRE(cJSON_AddNumberToObject(item, "offset", event->request.offset));
        JSON_REQUIRE(cJSON_AddNumberToObject(item, "quantity", event->request.quantity));
        JSON_REQUIRE(cJSON_AddNumberToObject(item, "transaction_id", event->request.transaction_id));
        JSON_REQUIRE(cJSON_AddNumberToObject(item, "config_revision", event->request.config_revision));
        JSON_REQUIRE(cJSON_AddStringToObject(item, "profile", event->request.profile));
        JSON_REQUIRE(cJSON_AddStringToObject(item, "phase", event->request.phase));
        JSON_REQUIRE(cJSON_AddStringToObject(item, "error", mb_error_string(event->result.error)));
        JSON_REQUIRE(cJSON_AddNumberToObject(item, "error_code", event->result.error));
        JSON_REQUIRE(cJSON_AddNumberToObject(item, "exception_code", event->result.exception_code));
        JSON_REQUIRE(cJSON_AddNumberToObject(item, "system_error", event->result.system_error));
        JSON_REQUIRE(cJSON_AddNumberToObject(item, "elapsed_ms", event->result.elapsed_ms));
    }
    free(snapshot);
    return json;

allocation_failed:
    free(snapshot);
    cJSON_Delete(json);
    return NULL;
#undef JSON_REQUIRE
}
