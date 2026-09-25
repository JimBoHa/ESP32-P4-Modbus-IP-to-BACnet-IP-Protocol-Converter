/* Compile the production module here to control its worker's flush boundary.
 * pthread mutexes and an atomic NVS fixture retain the actual locking, ring,
 * snapshot, validation and JSON paths. No device or network is opened. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../main/error_history.c"

struct history_test_mutex { pthread_mutex_t mutex; };
static unsigned char durable[sizeof(history_blob_t) + 16], staged[sizeof(durable)];
static size_t durable_size, staged_size;
static bool present, fail_mutex, fail_task;
static esp_err_t open_error, read_error, set_error, commit_error;
static unsigned sets, commits, tasks, closes;
static uint64_t clock_ms;
static void (*during_set)(void);

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    if (fail_mutex) return NULL;
    SemaphoreHandle_t lock = malloc(sizeof(*lock));
    assert(lock);
    pthread_mutexattr_t attr;
    assert(!pthread_mutexattr_init(&attr));
    assert(!pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK));
    assert(!pthread_mutex_init(&lock->mutex, &attr));
    assert(!pthread_mutexattr_destroy(&attr));
    return lock;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t lock, uint32_t wait)
{
    assert(lock && wait == portMAX_DELAY);
    assert(!pthread_mutex_lock(&lock->mutex));
    return pdPASS;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t lock)
{
    assert(lock && !pthread_mutex_unlock(&lock->mutex));
    return pdPASS;
}

BaseType_t xTaskCreate(void (*fn)(void *), const char *name, unsigned stack,
                       void *argument, unsigned priority, void *handle)
{
    assert(fn == persistence_task && !strcmp(name, "modbus_history"));
    assert(stack >= 4096 && !argument && priority == 2 && !handle);
    ++tasks;
    return fail_task ? 0 : pdPASS;
}

void vTaskDelay(uint32_t ticks) { (void)ticks; assert(!"Worker loop is driven by test flushes"); }
int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }

esp_err_t nvs_open_from_partition(const char *partition, const char *name, int mode,
                                   nvs_handle_t *handle)
{
    assert(!strcmp(partition, "gateway_cfg") && !strcmp(name, "mb_errors"));
    assert(mode == NVS_READWRITE);
    if (open_error) return open_error;
    *handle = 11;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size)
{
    assert(handle == 11 && !strcmp(key, "history_v1"));
    if (read_error) return read_error;
    if (!present) return ESP_ERR_NVS_NOT_FOUND;
    if (value && *size < durable_size) { *size = durable_size; return ESP_ERR_NVS_INVALID_LENGTH; }
    if (value) memcpy(value, durable, durable_size);
    *size = durable_size;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size)
{
    assert(handle == 11 && !strcmp(key, "history_v1") && size <= sizeof(staged));
    ++sets;
    if (during_set) { void (*hook)(void) = during_set; during_set = NULL; hook(); }
    if (set_error) return set_error;
    memcpy(staged, value, size);
    staged_size = size;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 11 && staged_size);
    ++commits;
    if (commit_error) return commit_error;
    memcpy(durable, staged, staged_size);
    durable_size = staged_size;
    present = true;
    staged_size = 0;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { assert(handle == 11); ++closes; }

static const error_history_request_t request = {
    .host = "192.0.2.81", .port = 502, .unit = 41, .function = 3,
    .offset = 1201, .quantity = 2, .transaction_id = 65535,
    .config_revision = 4, .profile = "kohler-full", .phase = "data"
};
static const mb_result_t timeout_result = {
    .error = MB_ERR_TIMEOUT, .elapsed_ms = 1200, .system_error = 60
};

static cJSON *field(cJSON *object, const char *name)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    assert(value);
    return value;
}

static double number(cJSON *object, const char *name)
{
    cJSON *value = field(object, name);
    assert(cJSON_IsNumber(value));
    return value->valuedouble;
}

static cJSON *event_at(cJSON *object, int index)
{
    cJSON *event = cJSON_GetArrayItem(field(object, "events"), index);
    assert(event && cJSON_IsObject(event));
    return event;
}

static void simulate_reboot(void)
{
    if (history.lock) { assert(!pthread_mutex_destroy(&history.lock->mutex)); free(history.lock); }
    memset(&history, 0, sizeof(history));
    staged_size = 0;
    clock_ms = 0;
    error_history_init();
}

static void test_metadata_and_unsynchronized_time(void)
{
    error_history_init();
    error_history_init();
    assert(tasks == 1 && sets == 0);
    cJSON *json = error_history_json(false);
    assert(number(json, "capacity") == 32 && number(json, "boot_id") == 1);
    assert(number(json, "count") == 0 && cJSON_IsTrue(field(json, "pending_persistence")));
    assert(!*field(json, "persistence_error")->valuestring);
    cJSON_Delete(json);
    flush_pending();
    assert(commits == 1 && valid_blob((const history_blob_t *)durable));
    mb_result_t success = {.error = MB_OK};
    error_history_record(&request, &success, 100, 0);
    error_history_record(NULL, &timeout_result, 100, 0);
    error_history_record(&request, NULL, 100, 0);
    error_history_record(&request, &timeout_result, 1234, 0);
    assert(sets == 1); /* Recording never writes flash. */
    json = error_history_json(false);
    assert(number(json, "count") == 1 && number(json, "total_errors") == 1);
    assert(cJSON_IsFalse(field(json, "clock_synchronized")));
    assert(cJSON_IsTrue(field(json, "pending_persistence")));
    cJSON *event = event_at(json, 0);
    assert(number(event, "sequence") == 1 && number(event, "boot_id") == 1);
    assert(number(event, "uptime_ms") == 1234 && cJSON_IsNull(field(event, "utc_ms")));
    assert(!strcmp(field(event, "host")->valuestring, request.host));
    assert(number(event, "port") == 502 && number(event, "unit") == 41);
    assert(number(event, "function") == 3 && number(event, "offset") == 1201);
    assert(number(event, "quantity") == 2 && number(event, "transaction_id") == 65535);
    assert(number(event, "config_revision") == 4);
    assert(!strcmp(field(event, "profile")->valuestring, "kohler-full"));
    assert(!strcmp(field(event, "phase")->valuestring, "data"));
    assert(!strcmp(field(event, "error")->valuestring, mb_error_string(MB_ERR_TIMEOUT)));
    assert(number(event, "error_code") == MB_ERR_TIMEOUT && number(event, "exception_code") == 0);
    assert(number(event, "system_error") == 60 && number(event, "elapsed_ms") == 1200);
    cJSON_Delete(json);
}

static void test_ring_retention_and_reboot(void)
{
    error_history_init();
    for (unsigned i = 1; i <= 40; ++i)
        error_history_record(&request, &timeout_result, i * 1000U, INT64_C(1790290000000) + i);
    cJSON *json = error_history_json(true);
    assert(number(json, "count") == 32 && number(json, "total_errors") == 40);
    assert(number(json, "overwritten") == 8 && cJSON_IsTrue(field(json, "clock_synchronized")));
    for (unsigned age = 0; age < 32; ++age) {
        cJSON *event = event_at(json, (int)age);
        assert(number(event, "sequence") == 40 - age);
        assert(number(event, "utc_ms") == (double)(INT64_C(1790290000000) + 40 - age));
    }
    cJSON_Delete(json);
    flush_pending();
    simulate_reboot();
    json = error_history_json(false);
    assert(number(json, "boot_id") == 2 && number(json, "total_errors") == 40);
    assert(number(event_at(json, 0), "boot_id") == 1);
    assert(number(event_at(json, 31), "sequence") == 9);
    assert(!cJSON_IsNull(field(event_at(json, 0), "utc_ms")));
    cJSON_Delete(json);
    error_history_record(&request, &timeout_result, 500, 0);
    flush_pending();
    simulate_reboot();
    json = error_history_json(false);
    assert(number(json, "boot_id") == 3 && number(json, "total_errors") == 41);
    assert(number(event_at(json, 0), "boot_id") == 2);
    assert(number(event_at(json, 0), "sequence") == 41);
    assert(cJSON_IsNull(field(event_at(json, 0), "utc_ms")));
    assert(number(event_at(json, 1), "boot_id") == 1);
    cJSON_Delete(json);
}

static void record_during_flash_write(void)
{
    /* An error arriving after the snapshot must not be marked durable by it.
     * The real error-checking mutex also rejects holding the ring lock here. */
    error_history_record(&request, &timeout_result, 1500, 0);
}

static void test_flush_generation_and_rate_limit(void)
{
    error_history_init();
    error_history_record(&request, &timeout_result, 1000, 0);
    during_set = record_during_flash_write;
    flush_pending();
    cJSON *json = error_history_json(false);
    assert(number(json, "total_errors") == 2 && cJSON_IsTrue(field(json, "pending_persistence")));
    assert(((const history_blob_t *)durable)->total_errors == 1 && commits == 1);
    cJSON_Delete(json);
    clock_ms = ERROR_HISTORY_FLUSH_INTERVAL_MS - 1;
    flush_pending();
    assert(commits == 1);
    clock_ms = ERROR_HISTORY_FLUSH_INTERVAL_MS;
    flush_pending();
    json = error_history_json(false);
    assert(cJSON_IsFalse(field(json, "pending_persistence")) && commits == 2);
    assert(((const history_blob_t *)durable)->total_errors == 2);
    cJSON_Delete(json);
    clock_ms += 100000;
    flush_pending();
    assert(commits == 2); /* Clean state does not consume flash writes. */
}

static void test_write_failure_and_recovery(void)
{
    error_history_init();
    error_history_record(&request, &timeout_result, 1000, 0);
    set_error = ESP_FAIL;
    flush_pending();
    assert(sets == 1 && commits == 0 && !present);
    cJSON *json = error_history_json(false);
    assert(cJSON_IsTrue(field(json, "pending_persistence")));
    assert(strstr(field(json, "persistence_error")->valuestring, "History persistence"));
    cJSON_Delete(json);
    set_error = ESP_OK;
    clock_ms = 1000;
    flush_pending();
    assert(sets == 1);
    commit_error = ESP_FAIL;
    clock_ms = ERROR_HISTORY_FLUSH_INTERVAL_MS;
    flush_pending();
    assert(sets == 2 && commits == 1 && !present);
    commit_error = ESP_OK;
    clock_ms += ERROR_HISTORY_FLUSH_INTERVAL_MS;
    flush_pending();
    json = error_history_json(false);
    assert(cJSON_IsFalse(field(json, "pending_persistence")));
    assert(!*field(json, "persistence_error")->valuestring && present);
    cJSON_Delete(json);
    simulate_reboot();
    json = error_history_json(false);
    assert(number(json, "count") == 1 && number(event_at(json, 0), "sequence") == 1);
    cJSON_Delete(json);
}

static void assert_blocked_storage_preserved(void)
{
    unsigned char before[sizeof(durable)];
    memcpy(before, durable, sizeof(before));
    size_t before_size = durable_size;
    unsigned before_sets = sets;
    simulate_reboot();
    error_history_record(&request, &timeout_result, 1500, 0);
    flush_pending();
    cJSON *json = error_history_json(false);
    assert(number(json, "count") == 1 && cJSON_IsTrue(field(json, "pending_persistence")));
    assert(*field(json, "persistence_error")->valuestring);
    assert(sets == before_sets && durable_size == before_size);
    assert(!memcmp(durable, before, sizeof(before)));
    cJSON_Delete(json);
}

static void test_unknown_corrupt_and_truncated_storage(void)
{
    error_history_init();
    error_history_record(&request, &timeout_result, 1000, 0);
    flush_pending();
    history_blob_t valid;
    memcpy(&valid, durable, sizeof(valid));
    history_blob_t modified = valid;
    modified.version = HISTORY_VERSION + 1;
    modified.checksum = blob_checksum(&modified);
    memcpy(durable, &modified, sizeof(modified));
    assert_blocked_storage_preserved();
    memcpy(durable, &valid, sizeof(valid));
    durable[offsetof(history_blob_t, events)] ^= 0x10;
    assert_blocked_storage_preserved();
    memcpy(durable, &valid, sizeof(valid));
    --durable_size;
    assert_blocked_storage_preserved();
    durable_size = sizeof(valid);
    modified = valid;
    modified.next = ERROR_HISTORY_CAPACITY;
    modified.checksum = blob_checksum(&modified);
    memcpy(durable, &modified, sizeof(modified));
    assert_blocked_storage_preserved();
    modified = valid;
    modified.events[0].request.host[15] = 'X';
    modified.checksum = blob_checksum(&modified);
    memcpy(durable, &modified, sizeof(modified));
    assert_blocked_storage_preserved();
}

static void test_storage_and_worker_unavailable(void)
{
    open_error = ESP_FAIL;
    error_history_init();
    assert(tasks == 0);
    error_history_record(&request, &timeout_result, 50, 0);
    cJSON *json = error_history_json(false);
    assert(number(json, "count") == 1);
    assert(strstr(field(json, "persistence_error")->valuestring, "storage open"));
    cJSON_Delete(json);
    open_error = ESP_OK;
    read_error = ESP_FAIL;
    simulate_reboot();
    error_history_record(&request, &timeout_result, 50, 0);
    json = error_history_json(false);
    assert(number(json, "count") == 1);
    assert(strstr(field(json, "persistence_error")->valuestring, "storage read"));
    cJSON_Delete(json);
    read_error = ESP_OK;
    fail_task = true;
    simulate_reboot();
    error_history_record(&request, &timeout_result, 50, 0);
    json = error_history_json(false);
    assert(number(json, "count") == 1);
    assert(strstr(field(json, "persistence_error")->valuestring, "worker allocation"));
    assert(cJSON_IsTrue(field(json, "pending_persistence")) && sets == 0);
    cJSON_Delete(json);
    fail_task = false;
    fail_mutex = true;
    simulate_reboot();
    error_history_record(&request, &timeout_result, 50, 0);
    json = error_history_json(false);
    assert(number(json, "count") == 0);
    assert(strstr(field(json, "persistence_error")->valuestring, "mutex allocation"));
    cJSON_Delete(json);
}

static void test_unterminated_strings_and_exception(void)
{
    error_history_init();
    error_history_request_t long_request = request;
    memset(long_request.host, 'H', sizeof(long_request.host));
    memset(long_request.profile, 'P', sizeof(long_request.profile));
    memset(long_request.phase, 'F', sizeof(long_request.phase));
    mb_result_t exception = {.error = MB_ERR_EXCEPTION, .exception_code = 6, .elapsed_ms = 2};
    error_history_record(&long_request, &exception, 800, -1);
    cJSON *json = error_history_json(true);
    cJSON *event = event_at(json, 0);
    assert(strlen(field(event, "host")->valuestring) == 15);
    assert(strlen(field(event, "profile")->valuestring) == 31);
    assert(strlen(field(event, "phase")->valuestring) == 15);
    assert(number(event, "exception_code") == 6 && cJSON_IsNull(field(event, "utc_ms")));
    cJSON_Delete(json);
    flush_pending();
    simulate_reboot();
    json = error_history_json(false);
    assert(number(json, "count") == 1 && !*field(json, "persistence_error")->valuestring);
    cJSON_Delete(json);
}

static unsigned json_allocations, json_fail_at, json_live_allocations;

static void *json_test_malloc(size_t size)
{
    if (++json_allocations == json_fail_at) return NULL;
    void *allocation = malloc(size);
    if (allocation) ++json_live_allocations;
    return allocation;
}

static void json_test_free(void *allocation)
{
    if (allocation) { assert(json_live_allocations); --json_live_allocations; free(allocation); }
}

static void test_json_allocation_failure(void)
{
    error_history_init();
    error_history_record(&request, &timeout_result, 1000, 0);
    error_history_record(&request, &timeout_result, 2000, INT64_C(1790290000000));
    cJSON_Hooks hooks = {.malloc_fn = json_test_malloc, .free_fn = json_test_free};
    cJSON_InitHooks(&hooks);
    cJSON *json = error_history_json(true);
    assert(json);
    unsigned required = json_allocations;
    cJSON_Delete(json);
    assert(!json_live_allocations);
    /* Fail each allocation separately, including key/value allocation inside
     * Add* helpers: return HTTP-500-compatible NULL, never a partial success. */
    for (unsigned fail_at = 1; fail_at <= required; ++fail_at) {
        json_allocations = 0;
        json_fail_at = fail_at;
        json = error_history_json(true);
        assert(!json && !json_live_allocations);
    }
    json_fail_at = 0;
    json = error_history_json(true);
    assert(number(json, "count") == 2 && number(json, "total_errors") == 2);
    cJSON_Delete(json);
    assert(!json_live_allocations);
    cJSON_InitHooks(NULL);
}

int main(void)
{
    void (*cases[])(void) = {
        test_metadata_and_unsynchronized_time, test_ring_retention_and_reboot,
        test_flush_generation_and_rate_limit, test_write_failure_and_recovery,
        test_unknown_corrupt_and_truncated_storage, test_storage_and_worker_unavailable,
        test_unterminated_strings_and_exception, test_json_allocation_failure
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        pid_t child = fork();
        assert(child >= 0);
        if (!child) { cases[i](); _exit(0); }
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    puts("Modbus error history: 8 persistence/ring/concurrency/clock/allocation scenarios passed");
    return 0;
}
