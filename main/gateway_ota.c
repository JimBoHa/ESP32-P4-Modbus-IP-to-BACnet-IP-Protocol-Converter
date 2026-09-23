/* Signed HTTPS upload/authentication and health gating adapted from
 * JimBoHa/esp32-p4-bacnet-switches main/ota_server.c at 9b2f89c6b582.
 * Keeps the existing certificate/token identity and software signature policy.
 * Never changes the partition table, bootloader, security configuration or eFuses. */
#include "gateway_ota.h"
#include "sdkconfig.h"

#if CONFIG_GW_OTA_ENABLED
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE || !CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT || !CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME
#error "HTTPS OTA requires bootloader rollback and software RSA-signed updates"
#endif
#if CONFIG_SECURE_BOOT || CONFIG_SECURE_FLASH_ENC_ENABLED || CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK
#error "Hardware security provisioning is outside this gateway's OTA policy"
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_secure_boot.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "ota_auth.h"
#include "ota_health.h"

#define RECEIVE_BYTES 4096U
#define RECEIVE_DEADLINE_US (5LL * 60LL * 1000000LL)
#define IMAGE_MIN_BYTES 288U
#define IMAGE_MAX_BYTES 0x400000U
#define HEALTH_DELAY_MS 10000U
#define HEALTH_DEADLINE_MS 60000U
#define HEALTH_SAMPLES 5U
#define SHA_BYTES 32U
#define SHA_TEXT_BYTES 65U
#ifndef GATEWAY_SOURCE_REVISION
#define GATEWAY_SOURCE_REVISION "unknown"
#endif

extern const unsigned char certificate_start[] asm("_binary_ota_server_cert_pem_start");
extern const unsigned char certificate_end[] asm("_binary_ota_server_cert_pem_end");
extern const unsigned char key_start[] asm("_binary_ota_server_key_pem_start");
extern const unsigned char key_end[] asm("_binary_ota_server_key_pem_end");
extern const unsigned char admin_start[] asm("_binary_ota_token_txt_start");
extern const unsigned char admin_end[] asm("_binary_ota_token_txt_end");
extern const unsigned char viewer_start[] asm("_binary_ota_viewer_token_txt_start");
extern const unsigned char viewer_end[] asm("_binary_ota_viewer_token_txt_end");

static const char *TAG = "gateway_ota";
static httpd_handle_t server;
static atomic_bool ready, upload_busy, restart_pending, validation_finished;
static char admin_token[OTA_TOKEN_MAX_LENGTH + 1], viewer_token[OTA_TOKEN_MAX_LENGTH + 1];
static char image_hash[SHA_TEXT_BYTES], signing_hash[SHA_TEXT_BYTES];
static uint32_t boot_count;
static bool (*health_callback)(void);
static esp_timer_handle_t validation_deadline;
static bool validation_started;

static void hex_digest(const uint8_t *digest, char *text)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < SHA_BYTES; ++i) { text[i*2] = hex[digest[i] >> 4]; text[i*2+1] = hex[digest[i] & 15]; }
    text[64] = 0;
}

static const char *state_name(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
    }
}

static esp_err_t json_response(httpd_req_t *request, const char *status, cJSON *json)
{
    char *text = json ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    if (!text) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    esp_err_t result = httpd_resp_send(request, text, HTTPD_RESP_USE_STRLEN);
    free(text); return result;
}

static void deny(httpd_req_t *request, const char *status, const char *message)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", false);
    cJSON_AddStringToObject(reply, "error", message);
    (void)json_response(request, status, reply);
}

static ota_role_t request_role(httpd_req_t *request)
{
    size_t length = httpd_req_get_hdr_value_len(request, "Authorization");
    if (!length) return ota_authorization_role(NULL, 0, admin_token, viewer_token);
    if (length > OTA_AUTHORIZATION_MAX_LENGTH) return OTA_ROLE_NONE;
    char header[OTA_AUTHORIZATION_MAX_LENGTH + 1];
    if (httpd_req_get_hdr_value_str(request, "Authorization", header, sizeof(header)) != ESP_OK) return OTA_ROLE_NONE;
    return ota_authorization_role(header, length, admin_token, viewer_token);
}

bool gateway_ota_authorize_mutation(httpd_req_t *request)
{
    ota_role_t role = request_role(request);
    if (role != OTA_ROLE_ADMIN) {
        if (role == OTA_ROLE_VIEWER) deny(request, "403 Forbidden", "Admin token required; viewer is read-only");
        else {
            httpd_resp_set_hdr(request, "WWW-Authenticate", "Bearer realm=\"ESP32 Management\"");
            deny(request, "401 Unauthorized", "Valid admin bearer token required");
        }
        return false;
    }
    if (atomic_load(&upload_busy) || atomic_load(&restart_pending)) {
        deny(request, "409 Conflict", "Firmware upload or restart already in progress"); return false;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) {
        deny(request, "500 Internal Server Error", "Running OTA state unavailable"); return false;
    }
    if (state != ESP_OTA_IMG_VALID) {
        deny(request, "409 Conflict", "Running firmware must finish health validation first"); return false;
    }
    return true;
}

void gateway_ota_note_restart_pending(void) { atomic_store(&restart_pending, true); }
bool gateway_ota_ready(void) { return atomic_load(&ready); }

static esp_err_t status_get(httpd_req_t *request)
{
    if (!ota_role_allows(request_role(request), true)) {
        deny(request, "401 Unauthorized", "Invalid bearer token"); return ESP_FAIL;
    }
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (!app || !running || esp_ota_get_state_partition(running, &state) != ESP_OK) {
        deny(request, "500 Internal Server Error", "Running firmware state unavailable"); return ESP_FAIL;
    }
    cJSON *j = cJSON_CreateObject(), *security = cJSON_CreateObject(), *policy = cJSON_CreateObject();
    cJSON *system = cJSON_CreateObject(), *firmware = cJSON_CreateObject();
    if (!j || !security || !policy || !system || !firmware) {
        cJSON_Delete(j); cJSON_Delete(security); cJSON_Delete(policy); cJSON_Delete(system); cJSON_Delete(firmware);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddItemToObject(j, "security", security); cJSON_AddItemToObject(j, "ota_policy", policy);
    cJSON_AddItemToObject(j, "system", system); cJSON_AddItemToObject(j, "firmware", firmware);
    cJSON_AddBoolToObject(j, "ota", true); cJSON_AddStringToObject(j, "project", app->project_name);
    cJSON_AddStringToObject(j, "version", app->version); cJSON_AddStringToObject(j, "idf_version", app->idf_ver);
    cJSON_AddStringToObject(j, "git_revision", GATEWAY_SOURCE_REVISION);
    cJSON_AddStringToObject(j, "partition", running->label); cJSON_AddStringToObject(j, "state", state_name(state));
    cJSON_AddNumberToObject(j, "port", CONFIG_GW_OTA_PORT); cJSON_AddStringToObject(j, "image_sha256", image_hash);
    cJSON_AddBoolToObject(security, "software_signature_verification", true);
    cJSON_AddBoolToObject(security, "https_management", true); cJSON_AddBoolToObject(security, "mutations_require_admin", true);
    cJSON_AddBoolToObject(security, "anonymous_read_only", true); cJSON_AddBoolToObject(security, "viewer_admin_separation", true);
    cJSON_AddBoolToObject(security, "secure_boot_enabled", false); cJSON_AddBoolToObject(security, "flash_encryption_enabled", false);
    cJSON_AddBoolToObject(security, "application_anti_rollback_enabled", false);
    cJSON_AddBoolToObject(policy, "signature_required", true); cJSON_AddStringToObject(policy, "signature_scheme", "rsa-pss-3072-sha256");
    cJSON_AddStringToObject(policy, "signing_key_sha256", signing_hash);
    cJSON_AddNumberToObject(policy, "minimum_secure_version", app->secure_version);
    cJSON_AddNumberToObject(policy, "minimum_image_bytes", IMAGE_MIN_BYTES);
    cJSON_AddNumberToObject(policy, "maximum_image_bytes", next ? next->size : 0);
    cJSON_AddNumberToObject(system, "boot_count", boot_count);
    cJSON_AddNumberToObject(system, "uptime_ms", esp_timer_get_time() / 1000);
    cJSON_AddBoolToObject(firmware, "rollback_enabled", true);
    cJSON_AddStringToObject(firmware, "running_partition", running->label);
    cJSON_AddNumberToObject(firmware, "running_address", running->address);
    cJSON_AddStringToObject(firmware, "boot_partition", boot ? boot->label : "unavailable");
    cJSON_AddStringToObject(firmware, "next_update_partition", next ? next->label : "unavailable");
    return json_response(request, "200 OK", j);
}

static bool matches_header(httpd_req_t *request, const char *key, const char *expected, bool case_sensitive)
{
    char value[64];
    size_t length = httpd_req_get_hdr_value_len(request, key);
    if (!length || length >= sizeof(value) || strlen(expected) != length ||
        httpd_req_get_hdr_value_str(request, key, value, sizeof(value)) != ESP_OK) return false;
    return case_sensitive ? !strcmp(value, expected) : !strcasecmp(value, expected);
}

static esp_err_t upload_post(httpd_req_t *request)
{
    if (!gateway_ota_authorize_mutation(request)) return ESP_FAIL;
    const esp_app_desc_t *running_app = esp_app_get_description();
    if (!running_app || !matches_header(request, "X-Firmware-Project", running_app->project_name, true)) {
        deny(request, "400 Bad Request", "Missing or incorrect X-Firmware-Project header"); return ESP_FAIL;
    }
    if (!matches_header(request, "Content-Type", "application/octet-stream", false)) {
        deny(request, "415 Unsupported Media Type", "Content-Type must be application/octet-stream"); return ESP_FAIL;
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!target || target == running || target->type != ESP_PARTITION_TYPE_APP ||
        (target->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 && target->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1)) {
        deny(request, "500 Internal Server Error", "No inactive OTA slot"); return ESP_FAIL;
    }
    if (request->content_len < IMAGE_MIN_BYTES || request->content_len > target->size || request->content_len > IMAGE_MAX_BYTES) {
        deny(request, request->content_len > target->size || request->content_len > IMAGE_MAX_BYTES ?
             "413 Content Too Large" : "400 Bad Request", "Invalid firmware image size"); return ESP_FAIL;
    }
    if (atomic_exchange(&upload_busy, true)) { deny(request, "409 Conflict", "Upload already in progress"); return ESP_FAIL; }
    uint8_t *buffer = malloc(RECEIVE_BYTES);
    esp_ota_handle_t handle = 0;
    bool begun = false;
    const char *failure = "Firmware update failed";
    const char *status = "500 Internal Server Error";
    if (!buffer) { failure = "Out of memory"; goto fail; }
    if (esp_ota_begin(target, request->content_len, &handle) != ESP_OK) { failure = "Could not begin OTA write"; goto fail; }
    begun = true;
    size_t remaining = request->content_len;
    unsigned timeouts = 0;
    int64_t deadline = esp_timer_get_time() + RECEIVE_DEADLINE_US;
    while (remaining) {
        if (esp_timer_get_time() >= deadline) { status = "408 Request Timeout"; failure = "Upload deadline exceeded"; goto fail; }
        size_t wanted = remaining < RECEIVE_BYTES ? remaining : RECEIVE_BYTES;
        int received = httpd_req_recv(request, (char *)buffer, wanted);
        if (received == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 5) continue;
        if (received <= 0 || (size_t)received > wanted) { status = "400 Bad Request"; failure = "Upload ended early"; goto fail; }
        timeouts = 0;
        esp_err_t result = esp_ota_write(handle, buffer, received);
        if (result != ESP_OK) {
            status = result == ESP_ERR_OTA_VALIDATE_FAILED ? "400 Bad Request" : "500 Internal Server Error";
            failure = "Firmware write or image validation failed"; goto fail;
        }
        remaining -= received;
    }
    free(buffer); buffer = NULL;
    esp_err_t result = esp_ota_end(handle); begun = false;
    if (result != ESP_OK) { status = "400 Bad Request"; failure = "Firmware hash, signature or compatibility validation failed"; goto fail; }
    esp_app_desc_t candidate;
    if (esp_ota_get_partition_description(target, &candidate) != ESP_OK ||
        strnlen(candidate.project_name, sizeof(candidate.project_name)) == sizeof(candidate.project_name) ||
        strnlen(candidate.version, sizeof(candidate.version)) == sizeof(candidate.version) || !candidate.version[0]) {
        status = "400 Bad Request"; failure = "Invalid firmware descriptor"; goto fail;
    }
    if (strcmp(candidate.project_name, running_app->project_name)) {
        status = "400 Bad Request"; failure = "Firmware belongs to a different project"; goto fail;
    }
    if (candidate.secure_version < running_app->secure_version) {
        status = "400 Bad Request"; failure = "Firmware secure version is older"; goto fail;
    }
    uint8_t digest[SHA_BYTES]; char candidate_hash[SHA_TEXT_BYTES];
    if (esp_partition_get_sha256(target, digest) != ESP_OK) { failure = "Firmware image hash failed"; goto fail; }
    hex_digest(digest, candidate_hash);
    if (esp_ota_set_boot_partition(target) != ESP_OK) { failure = "Could not select candidate boot slot"; goto fail; }
    atomic_store(&restart_pending, true);
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "accepted", true); cJSON_AddBoolToObject(reply, "rebooting", true);
    cJSON_AddStringToObject(reply, "version", candidate.version); cJSON_AddStringToObject(reply, "partition", target->label);
    cJSON_AddStringToObject(reply, "image_sha256", candidate_hash);
    (void)json_response(request, "202 Accepted", reply);
    vTaskDelay(pdMS_TO_TICKS(1500)); esp_restart(); return ESP_OK;
fail:
    if (begun) (void)esp_ota_abort(handle);
    free(buffer); atomic_store(&upload_busy, false);
    deny(request, status, failure);
    return ESP_FAIL; /* Close unread bodies instead of draining untrusted lengths. */
}

static esp_err_t reboot_post(httpd_req_t *request)
{
    if (!gateway_ota_authorize_mutation(request)) return ESP_FAIL;
    if (request->content_len) { deny(request, "400 Bad Request", "Reboot request body must be empty"); return ESP_FAIL; }
    atomic_store(&restart_pending, true);
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "accepted", true); cJSON_AddBoolToObject(reply, "rebooting", true);
    cJSON_AddNumberToObject(reply, "boot_count", boot_count); cJSON_AddStringToObject(reply, "image_sha256", image_hash);
    (void)json_response(request, "202 Accepted", reply);
    vTaskDelay(pdMS_TO_TICKS(1000)); esp_restart(); return ESP_OK;
}

static int hardware_random(void *context, unsigned char *output, size_t length)
{ (void)context; esp_fill_random(output, length); return 0; }

static esp_err_t validate_tls(void)
{
    mbedtls_x509_crt certificate; mbedtls_pk_context key;
    mbedtls_x509_crt_init(&certificate); mbedtls_pk_init(&key);
    int result = mbedtls_x509_crt_parse(&certificate, certificate_start, certificate_end - certificate_start);
    if (!result) result = mbedtls_pk_parse_key(&key, key_start, key_end - key_start, NULL, 0, hardware_random, NULL);
    if (!result) result = mbedtls_pk_check_pair(&certificate.pk, &key, hardware_random, NULL);
    mbedtls_pk_free(&key); mbedtls_x509_crt_free(&certificate);
    return result ? ESP_ERR_INVALID_ARG : ESP_OK;
}

static esp_err_t cache_identity(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    uint8_t digest[SHA_BYTES];
    if (!running) return ESP_ERR_NOT_FOUND;
    esp_err_t result = esp_partition_get_sha256(running, digest);
    if (result != ESP_OK) return result;
    hex_digest(digest, image_hash);
    esp_image_sig_public_key_digests_t keys = {0};
    result = esp_secure_boot_get_signature_blocks_for_running_app(true, &keys);
    if (result != ESP_OK) return result;
    if (keys.num_digests != 1) return ESP_ERR_INVALID_STATE;
    hex_digest(keys.key_digests[0], signing_hash);
    nvs_handle_t nvs;
    result = nvs_open("gw_system", NVS_READWRITE, &nvs);
    if (result != ESP_OK) return result;
    uint32_t previous = 0;
    result = nvs_get_u32(nvs, "boots", &previous);
    if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
    if (result == ESP_OK) result = nvs_set_u32(nvs, "boots", previous + 1);
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    if (result == ESP_OK) boot_count = previous + 1;
    return result;
}

esp_err_t gateway_ota_start(esp_err_t (*register_web)(httpd_handle_t))
{
    if (server) return ESP_OK;
    if (!register_web || !ota_copy_embedded_token(admin_start, admin_end - admin_start, admin_token, sizeof(admin_token)) ||
        !ota_copy_embedded_token(viewer_start, viewer_end - viewer_start, viewer_token, sizeof(viewer_token)) ||
        !ota_role_tokens_valid(admin_token, viewer_token)) return ESP_ERR_INVALID_ARG;
    esp_err_t result = validate_tls();
    if (result != ESP_OK) return result;
    result = cache_identity(); if (result != ESP_OK) return result;
    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.port_secure = CONFIG_GW_OTA_PORT;
    config.httpd.max_uri_handlers = 20; config.httpd.max_resp_headers = 10;
    config.httpd.ctrl_port = 32768; config.httpd.max_open_sockets = 2;
    config.httpd.stack_size = 12288; config.httpd.lru_purge_enable = true;
    config.httpd.recv_wait_timeout = 5; config.httpd.send_wait_timeout = 5;
    config.servercert = certificate_start; config.servercert_len = certificate_end - certificate_start;
    config.prvtkey_pem = key_start; config.prvtkey_len = key_end - key_start;
    httpd_handle_t created = NULL;
    result = httpd_ssl_start(&created, &config); if (result != ESP_OK) return result;
    const httpd_uri_t handlers[] = {
        {.uri="/ota/status", .method=HTTP_GET, .handler=status_get},
        {.uri="/ota", .method=HTTP_POST, .handler=upload_post},
        {.uri="/system/reboot", .method=HTTP_POST, .handler=reboot_post},
    };
    for (size_t i = 0; result == ESP_OK && i < sizeof(handlers)/sizeof(handlers[0]); ++i)
        result = httpd_register_uri_handler(created, &handlers[i]);
    if (result == ESP_OK) result = register_web(created);
    if (result != ESP_OK) { (void)httpd_ssl_stop(created); return result; }
    server = created; atomic_store(&ready, true);
    ESP_LOGI(TAG, "Signed HTTPS OTA and web configuration ready on port %u", CONFIG_GW_OTA_PORT);
    return ESP_OK;
}

static void hard_validation_deadline(void *unused)
{
    (void)unused;
    /* Independent of startup and health-callback progress. The existing
     * rollback-enabled bootloader aborts a still-pending image on reset. */
    if (!atomic_load(&validation_finished)) esp_restart();
}

static void validate_task(void *unused)
{
    (void)unused;
    vTaskDelay(pdMS_TO_TICKS(HEALTH_DELAY_MS));
    ota_health_gate_t gate = {0};
    while (!atomic_load(&validation_finished)) {
        if (ota_health_gate_sample(&gate, gateway_ota_ready() && health_callback && health_callback(), HEALTH_SAMPLES)) {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                atomic_store(&validation_finished, true);
                (void)esp_timer_stop(validation_deadline);
                ESP_LOGI(TAG, "Firmware validated after five consecutive healthy samples");
            } else esp_restart();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL);
}

esp_err_t gateway_ota_begin_validation(bool (*healthy)(void))
{
    if (validation_started) return ESP_OK;
    if (!healthy) return ESP_ERR_INVALID_ARG;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return ESP_ERR_NOT_FOUND;
    esp_ota_img_states_t state;
    esp_err_t result = esp_ota_get_state_partition(running, &state);
    if (result == ESP_ERR_NOT_FOUND) {
        /* Fresh USB installation with erased OTA metadata. Select the signed
         * running OTA image through IDF's verified path, then let the existing
         * bootloader move NEW -> PENDING on reboot. Never mark it valid here. */
        const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
        if (running->type != ESP_PARTITION_TYPE_APP ||
            (running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
             running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1) ||
            !next || next == running || next->type != ESP_PARTITION_TYPE_APP ||
            next->size != running->size || running->size != IMAGE_MAX_BYTES ||
            (next->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
             next->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1) ||
            next->subtype == running->subtype) return ESP_ERR_INVALID_STATE;
        result = esp_ota_set_boot_partition(running);
        if (result != ESP_OK) return result;
        esp_restart();
        return ESP_OK;
    }
    if (result != ESP_OK) return result;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return ESP_OK;
    health_callback = healthy;
    esp_timer_create_args_t args = {.callback=hard_validation_deadline, .name="ota_deadline"};
    result = esp_timer_create(&args, &validation_deadline); if (result != ESP_OK) return result;
    result = esp_timer_start_once(validation_deadline, HEALTH_DEADLINE_MS * 1000ULL);
    if (result != ESP_OK) { (void)esp_timer_delete(validation_deadline); validation_deadline = NULL; return result; }
    validation_started = true;
    if (xTaskCreate(validate_task, "ota_health", 4096, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
#else
esp_err_t gateway_ota_begin_validation(bool (*healthy)(void)) { (void)healthy; return ESP_OK; }
esp_err_t gateway_ota_start(esp_err_t (*register_web)(httpd_handle_t)) { (void)register_web; return ESP_ERR_NOT_SUPPORTED; }
bool gateway_ota_ready(void) { return false; }
bool gateway_ota_authorize_mutation(httpd_req_t *request) { (void)request; return true; }
void gateway_ota_note_restart_pending(void) { }
#endif
