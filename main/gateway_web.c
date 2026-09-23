#include "gateway_web.h"
#include "gateway_storage.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if CONFIG_GW_OTA_ENABLED
#include "gateway_ota.h"
#include "dashboard_redirect.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "gateway_web";
static const gateway_config_t *active;
static const custom_map_t *saved_map;
static const char *boot_error;
static cJSON *(*status_json)(void), *(*points_json)(void);
static bool restart_pending;
static esp_timer_handle_t restart_timer;
extern const unsigned char index_start[] asm("_binary_index_html_start");
extern const unsigned char index_end[] asm("_binary_index_html_end");
extern const unsigned char template_start[] asm("_binary_template_csv_start");
extern const unsigned char template_end[] asm("_binary_template_csv_end");

void gateway_web_load(gateway_config_t *c, custom_map_t *map, char *error, size_t size)
{
    error[0] = 0;
    esp_err_t err = gateway_storage_prepare();
    if (err == ESP_OK) err = nvs_flash_init_partition("gateway_cfg");
    if (err != ESP_OK) { snprintf(error, size, "Configuration storage: %s", esp_err_to_name(err)); return; }
    nvs_handle_t nvs;
    err = nvs_open_from_partition("gateway_cfg", "gateway", NVS_READWRITE, &nvs);
    if (err != ESP_OK) { snprintf(error, size, "Configuration open: %s", esp_err_to_name(err)); return; }
    size_t length = 0;
    err = nvs_get_blob(nvs, "settings_v1", NULL, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) { nvs_close(nvs); return; }
    if (err != ESP_OK || !length || length > GW_CONFIG_MAX_JSON) {
        snprintf(error, size, "Stored configuration unreadable; save a valid configuration to repair");
        nvs_close(nvs); return;
    }
    const size_t capacity = length;
    char *text = malloc(capacity + 1);
    if (!text) { snprintf(error, size, "Out of memory loading configuration"); nvs_close(nvs); return; }
    err = nvs_get_blob(nvs, "settings_v1", text, &length);
    nvs_close(nvs);
    if (err != ESP_OK || length > capacity) snprintf(error, size, "Configuration read: %s", esp_err_to_name(err));
    else {
        text[length] = 0;
        gateway_config_parse(text, length, c, c, map, error, size);
    }
    free(text);
}

static esp_err_t json_reply(httpd_req_t *r, cJSON *json)
{
    char *text = json ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    if (!text) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    esp_err_t err = httpd_resp_send(r, text, HTTPD_RESP_USE_STRLEN);
    free(text);
    return err;
}

static esp_err_t json_error(httpd_req_t *r, const char *status, const char *message)
{
    httpd_resp_set_status(r, status);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", false);
    cJSON_AddStringToObject(j, "error", message);
    return json_reply(r, j);
}

static esp_err_t page_handler(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(r, "Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'");
    return httpd_resp_send(r, (const char *)index_start, index_end - index_start - 1);
}

static esp_err_t config_handler(httpd_req_t *r)
{
    cJSON *j = gateway_config_json(active, false);
    cJSON_AddNumberToObject(j, "custom_point_count", saved_map->count);
    cJSON_AddStringToObject(j, "config_error", boot_error);
#if CONFIG_GW_OTA_ENABLED
    cJSON_AddBoolToObject(j, "authentication_required", true);
#else
    cJSON_AddBoolToObject(j, "authentication_required", false);
#endif
    return json_reply(r, j);
}

static esp_err_t profiles_handler(httpd_req_t *r)
{
    const char *names[] = {"Kohler MPAC 1500 ATS — full", "Kohler MPAC 1500 ATS — electrical", "Custom CSV point map"};
    const char *descriptions[] = {
        "Older Section 13 map: electrical values, status, timers, settings and history. Controller identity checked before reading.",
        "First 24 status/electrical points from the same verified ATS map, with sensing quality checks retained.",
        "Read-only FC01/02/03/04 points defined by your device's documented register map. No automatic model detection."
    };
    const size_t counts[] = {ATS_POINT_COUNT, GW_ELECTRICAL_POINT_COUNT, saved_map->count};
    cJSON *j = cJSON_CreateArray();
    for (size_t i = 0; i < 3; ++i) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "id", gateway_profile_id((gw_profile_t)i));
        cJSON_AddStringToObject(p, "name", names[i]);
        cJSON_AddStringToObject(p, "description", descriptions[i]);
        cJSON_AddNumberToObject(p, "point_count", counts[i]);
        cJSON_AddItemToArray(j, p);
    }
    return json_reply(r, j);
}

static esp_err_t status_handler(httpd_req_t *r) { return json_reply(r, status_json()); }
static esp_err_t points_handler(httpd_req_t *r) { return json_reply(r, points_json()); }

static esp_err_t csv_handler(httpd_req_t *r)
{
    bool example = !strcmp(r->uri, "/api/template.csv");
    const char *text = example ? (const char *)template_start : active->csv;
    size_t length = example ? (size_t)(template_end - template_start - 1) : strlen(text);
    httpd_resp_set_type(r, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "Content-Disposition", example ? "attachment; filename=example-points.csv" : "attachment; filename=points.csv");
    return httpd_resp_send(r, text, length);
}

static char *request_body(httpd_req_t *r)
{
#if CONFIG_GW_OTA_ENABLED
    if (!gateway_ota_authorize_mutation(r)) return NULL;
#endif
    char header[64];
    /* Requiring a non-simple custom header and JSON prevents browser forms
     * and cross-origin fetch from changing config. No CORS grants are sent. */
    if (httpd_req_get_hdr_value_str(r, "X-Gateway-Request", header, sizeof(header)) != ESP_OK || strcmp(header, "1")) {
        json_error(r, "403 Forbidden", "Use the gateway configuration page"); return NULL;
    }
    if (httpd_req_get_hdr_value_str(r, "Content-Type", header, sizeof(header)) != ESP_OK ||
        (strcmp(header, "application/json") && strcmp(header, "application/json; charset=utf-8"))) {
        json_error(r, "415 Unsupported Media Type", "Expected application/json"); return NULL;
    }
    if (!r->content_len || r->content_len > GW_CONFIG_MAX_JSON) {
        json_error(r, "413 Content Too Large", "Configuration body must be 1-98304 bytes"); return NULL;
    }
    char *body = malloc(r->content_len + 1);
    if (!body) { json_error(r, "500 Internal Server Error", "Out of memory"); return NULL; }
    size_t used = 0;
    int64_t deadline = esp_timer_get_time() + 10000000;
    while (used < r->content_len && esp_timer_get_time() < deadline) {
        int got = httpd_req_recv(r, body + used, r->content_len - used);
        if (got <= 0) { free(body); json_error(r, "408 Request Timeout", "Upload incomplete; saved configuration unchanged"); return NULL; }
        used += got;
    }
    if (used != r->content_len) { free(body); json_error(r, "408 Request Timeout", "Upload timed out"); return NULL; }
    body[used] = 0;
    return body;
}

static esp_err_t validate_handler(httpd_req_t *r)
{
    char *body = request_body(r);
    if (!body) return ESP_FAIL; /* Close unread/failed request bodies. */
    if (!gateway_json_is_flat(body, r->content_len)) {
        free(body); return json_error(r, "400 Bad Request", "Expected a flat JSON object");
    }
    /* Use the same full configuration decoder for preview and save, so NUL,
     * duplicate keys and CSV rules cannot disagree between the two routes. */
    const char *end = NULL;
    cJSON *request = cJSON_ParseWithLengthOpts(body, r->content_len, &end, false);
    if (end) while (end < body + r->content_len && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
    cJSON *csv = cJSON_GetObjectItemCaseSensitive(request, "csv");
    if (!cJSON_IsObject(request) || cJSON_GetArraySize(request) != 1 || !cJSON_IsString(csv) ||
        end != body + r->content_len || gateway_json_has_nul(body, r->content_len)) {
        cJSON_Delete(request); free(body);
        return json_error(r, "400 Bad Request", "Expected one JSON csv string without NUL");
    }
    custom_map_t *map = calloc(1, sizeof(*map));
    if (!map) { cJSON_Delete(request); free(body); return json_error(r, "500 Internal Server Error", "Out of memory"); }
    char error[192];
    bool ok = custom_map_parse(map, csv->valuestring, strlen(csv->valuestring), error, sizeof(error));
    cJSON_Delete(request); free(body);
    if (!ok) { free(map); return json_error(r, "400 Bad Request", error); }
    cJSON *j = cJSON_CreateObject(), *points = cJSON_AddArrayToObject(j, "points");
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddNumberToObject(j, "point_count", map->count);
    for (size_t i = 0; i < map->count; ++i) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", map->defs[i].name);
        cJSON_AddNumberToObject(p, "object_type", map->defs[i].object_type);
        cJSON_AddNumberToObject(p, "instance", map->defs[i].instance);
        cJSON_AddItemToArray(points, p);
    }
    free(map);
    return json_reply(r, j);
}

static void restart_callback(void *unused) { (void)unused; esp_restart(); }

static esp_err_t save_handler(httpd_req_t *r)
{
    char *body = request_body(r);
    if (!body) return ESP_FAIL;
    if (restart_pending) { free(body); return json_error(r, "409 Conflict", "Restart already scheduled"); }
    gateway_config_t *candidate = malloc(sizeof(*candidate));
    custom_map_t *map = calloc(1, sizeof(*map));
    if (!candidate || !map) {
        free(body); free(candidate); free(map);
        return json_error(r, "500 Internal Server Error", "Out of memory");
    }
    char error[192];
    bool valid = gateway_config_parse(body, r->content_len, active, candidate, map, error, sizeof(error));
    free(body); free(map);
    if (!valid) { free(candidate); return json_error(r, "400 Bad Request", error); }
    if (candidate->revision != active->revision) {
        free(candidate); return json_error(r, "409 Conflict", "Configuration changed; reload before saving");
    }
    if (active->revision == UINT32_MAX) {
        free(candidate); return json_error(r, "409 Conflict", "Configuration revision exhausted");
    }
    ++candidate->revision;
    cJSON *json = gateway_config_json(candidate, true);
    char *stored = json ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json); free(candidate);
    if (!stored) return json_error(r, "500 Internal Server Error", "Out of memory");
    nvs_handle_t nvs;
    esp_err_t err = nvs_open_from_partition("gateway_cfg", "gateway", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, "settings_v1", stored, strlen(stored));
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    free(stored);
    if (err != ESP_OK) return json_error(r, "500 Internal Server Error", "Configuration storage failed; restart to verify saved state before retrying");
    restart_pending = true;
#if CONFIG_GW_OTA_ENABLED
    gateway_ota_note_restart_pending();
#endif
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", true);
    cJSON_AddBoolToObject(reply, "restart", true);
    cJSON_AddNumberToObject(reply, "revision", active->revision + 1);
    esp_err_t response = json_reply(r, reply);
    /* Complete response before restarting; active tasks continue using the
     * immutable current map until reboot. No mixed-schema BACnet objects. */
    ESP_ERROR_CHECK(esp_timer_start_once(restart_timer, 1500000));
    return response;
}

static esp_err_t register_web(httpd_handle_t http)
{
    const httpd_uri_t handlers[] = {
        {.uri="/", .method=HTTP_GET, .handler=page_handler},
        {.uri="/api/config", .method=HTTP_GET, .handler=config_handler},
        {.uri="/api/profiles", .method=HTTP_GET, .handler=profiles_handler},
        {.uri="/api/status", .method=HTTP_GET, .handler=status_handler},
        {.uri="/api/points", .method=HTTP_GET, .handler=points_handler},
        {.uri="/api/map.csv", .method=HTTP_GET, .handler=csv_handler},
        {.uri="/api/template.csv", .method=HTTP_GET, .handler=csv_handler},
        {.uri="/api/validate", .method=HTTP_POST, .handler=validate_handler},
        {.uri="/api/config", .method=HTTP_POST, .handler=save_handler},
    };
    for (size_t i = 0; i < sizeof(handlers)/sizeof(handlers[0]); ++i) {
        esp_err_t error = httpd_register_uri_handler(http, &handlers[i]);
        if (error != ESP_OK) return error;
    }
    return ESP_OK;
}

void gateway_web_start(const gateway_config_t *c, const custom_map_t *map,
                       const char *error, cJSON *(*status)(void), cJSON *(*points)(void))
{
    active = c; saved_map = map; boot_error = error; status_json = status; points_json = points;
    esp_timer_create_args_t timer = {.callback = restart_callback, .name = "config_restart"};
    ESP_ERROR_CHECK(esp_timer_create(&timer, &restart_timer));
#if CONFIG_GW_OTA_ENABLED
    ESP_ERROR_CHECK(gateway_ota_start(register_web));
    ESP_ERROR_CHECK(dashboard_redirect_start(CONFIG_GW_OTA_PORT));
    ESP_LOGI(TAG, "Profile selection, CSV upload and signed updates available over HTTPS");
#else
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 12288; hc.max_open_sockets = 3; hc.max_uri_handlers = 10;
    hc.lru_purge_enable = true; hc.recv_wait_timeout = 3; hc.send_wait_timeout = 3;
    httpd_handle_t http;
    ESP_ERROR_CHECK(httpd_start(&http, &hc));
    ESP_ERROR_CHECK(register_web(http));
    ESP_LOGI(TAG, "Profile selection and CSV upload available over HTTP");
#endif
}
