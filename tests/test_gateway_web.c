/* Production HTTP handlers with bounded in-memory ESP-IDF/NVS/timer adapters.
 * No listening sockets, network traffic, ESP flashing or real flash writes. */
#include "gateway_web.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Match IDF's embedded-text symbols and their trailing NUL. */
#ifdef __APPLE__
#define TEST_CONST_SECTION ".section __TEXT,__const\n"
#else
#define TEST_CONST_SECTION ".section .rodata\n"
#endif
__asm__(TEST_CONST_SECTION
    ".global _binary_index_html_start\n_binary_index_html_start:\n.asciz \"test-page\"\n"
    ".global _binary_index_html_end\n_binary_index_html_end:\n"
    ".global _binary_template_csv_start\n_binary_template_csv_start:\n.asciz \"test-template\"\n"
    ".global _binary_template_csv_end\n_binary_template_csv_end:\n.text\n");

static httpd_uri_t routes[12];
static size_t route_count;
static char *durable, *staged;
static size_t durable_size, staged_size;
static esp_err_t init_error, open_error, set_error, commit_error;
static bool grow_on_read;
static unsigned sets, commits, restarts, timer_starts, sequence, commit_sequence, timer_sequence;
static int64_t time_us = 1000000;
static struct test_esp_timer { void (*callback)(void *); void *arg; } timer_state;
static httpd_req_t *current_request;
static gateway_config_t active_config;
static custom_map_t active_map;
static const char fixture[] = CUSTOM_CSV_HEADER
    "\n42,Room-Temperature,AI,3,0,u16,AB,0.1,0,62,,,,1000,Temperature\n";

const char *esp_err_to_name(esp_err_t error) { return error == ESP_OK ? "ESP_OK" : "test-error"; }
void esp_restart(void) { ++restarts; }
int64_t esp_timer_get_time(void) { time_us += 100; return time_us; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer)
{
    timer_state.callback = args->callback; timer_state.arg = args->arg; *timer = &timer_state;
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout)
{
    assert(timer == &timer_state && timeout == 1500000);
    assert(current_request && current_request->response_sent && commits && !commit_error);
    assert(commit_sequence < current_request->sequence);
    ++timer_starts; timer_sequence = ++sequence;
    assert(current_request->sequence < timer_sequence);
    return ESP_OK;
}
esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    assert(config->max_uri_handlers >= 9); *handle = routes; return ESP_OK;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *handler)
{
    assert(handle == routes && route_count < sizeof(routes)/sizeof(routes[0]));
    routes[route_count++] = *handler; return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type)
{ snprintf(r->response_type, sizeof(r->response_type), "%s", type); return ESP_OK; }
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{ snprintf(r->status, sizeof(r->status), "%s", status); return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *name, const char *value)
{ (void)r; assert(name && value); return ESP_OK; }
esp_err_t httpd_resp_send(httpd_req_t *r, const char *body, ssize_t length)
{
    assert(!r->response_sent);
    if (length == HTTPD_RESP_USE_STRLEN) length = (ssize_t)strlen(body);
    assert(length >= 0);
    r->response = malloc((size_t)length + 1); assert(r->response);
    memcpy(r->response, body, (size_t)length); r->response[length] = 0;
    r->response_sent = true; r->sequence = ++sequence;
    return ESP_OK;
}
esp_err_t httpd_resp_send_err(httpd_req_t *r, int code, const char *message)
{
    snprintf(r->status, sizeof(r->status), "%d Error", code);
    return httpd_resp_send(r, message, HTTPD_RESP_USE_STRLEN);
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *name, char *out, size_t size)
{
    const char *value = !strcmp(name, "X-Gateway-Request") ? r->request_header :
        !strcmp(name, "Content-Type") ? r->content_type : NULL;
    if (!value || strlen(value) >= size) return ESP_FAIL;
    snprintf(out, size, "%s", value); return ESP_OK;
}
int httpd_req_recv(httpd_req_t *r, char *out, size_t length)
{
    if (r->fail_receive) return -1;
    if (length > r->body_length - r->received) length = r->body_length - r->received;
    if (r->chunk_size && length > r->chunk_size) length = r->chunk_size;
    memcpy(out, r->body + r->received, length); r->received += length;
    return (int)length;
}
esp_err_t nvs_flash_init_partition(const char *partition)
{ assert(!strcmp(partition, "gateway_cfg")); return init_error; }
esp_err_t nvs_open_from_partition(const char *partition, const char *name, int mode, nvs_handle_t *handle)
{
    assert(!strcmp(partition, "gateway_cfg") && !strcmp(name, "gateway") && mode == NVS_READWRITE);
    if (open_error) return open_error;
    *handle = 1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size)
{
    assert(handle == 1 && !strcmp(key, "settings_v1"));
    if (!durable) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *size = durable_size; return ESP_OK; }
    if (grow_on_read) { *size = durable_size + 8; return ESP_ERR_NVS_INVALID_LENGTH; }
    if (*size < durable_size) { *size = durable_size; return ESP_ERR_NVS_INVALID_LENGTH; }
    memcpy(out, durable, durable_size); *size = durable_size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    assert(handle == 1 && !strcmp(key, "settings_v1")); ++sets;
    if (set_error) return set_error;
    free(staged); staged = malloc(size); assert(staged); memcpy(staged, data, size); staged_size = size;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1); ++commits;
    if (commit_error) return commit_error;
    free(durable); durable = staged; durable_size = staged_size; staged = NULL; staged_size = 0;
    commit_sequence = ++sequence; return ESP_OK;
}
void nvs_close(nvs_handle_t handle)
{ assert(handle == 1); free(staged); staged = NULL; staged_size = 0; }

static cJSON *empty_json(void) { return cJSON_CreateObject(); }
static char *config_text(const gateway_config_t *c, bool csv)
{
    cJSON *j = gateway_config_json(c, csv); assert(j);
    char *text = cJSON_PrintUnformatted(j); cJSON_Delete(j); assert(text); return text;
}
static httpd_req_t request(const char *path, const char *body)
{
    return (httpd_req_t){.uri=path, .body=body, .body_length=body ? strlen(body) : 0,
        .content_len=body ? strlen(body) : 0, .request_header="1", .content_type="application/json",
        .chunk_size=7, .status="200 OK"};
}
static void dispatch(httpd_req_t *r, int method, unsigned expected)
{
    current_request = r;
    for (size_t i = 0; i < route_count; ++i) {
        if (routes[i].method == method && !strcmp(routes[i].uri, r->uri)) {
            (void)routes[i].handler(r);
            assert(r->response_sent && (unsigned)atoi(r->status) == expected);
            current_request = NULL;
            return;
        }
    }
    assert(false);
}
static void release(httpd_req_t *r) { free(r->response); r->response = NULL; }
static void save_expect(const char *text, unsigned status)
{
    httpd_req_t r = request("/api/config", text); dispatch(&r, HTTP_POST, status); release(&r);
}
static void seed_storage(const char *text)
{
    free(durable); durable_size = strlen(text); durable = malloc(durable_size); assert(durable);
    memcpy(durable, text, durable_size);
}

static void test_boot_load_errors(void)
{
    gateway_config_t *c = malloc(sizeof(*c)), *before = malloc(sizeof(*before));
    custom_map_t *map = calloc(1, sizeof(*map)); assert(c && before && map);
    gateway_config_defaults(c); *before = *c;
    char error[192];
    gateway_web_load(c, map, error, sizeof(error)); assert(!error[0]);
    init_error = ESP_FAIL; gateway_web_load(c, map, error, sizeof(error)); assert(error[0]); init_error = ESP_OK;
    open_error = ESP_FAIL; gateway_web_load(c, map, error, sizeof(error)); assert(error[0]); open_error = ESP_OK;
    seed_storage("{}"); gateway_web_load(c, map, error, sizeof(error)); assert(error[0]);
    assert(!memcmp(before, c, sizeof(*c)) && map->count == 0);
    char *text = config_text(c, true); seed_storage(text); free(text);
    grow_on_read = true; gateway_web_load(c, map, error, sizeof(error)); assert(error[0]); grow_on_read = false;
    assert(!memcmp(before, c, sizeof(*c)) && map->count == 0);
    gateway_web_load(c, map, error, sizeof(error)); assert(!error[0]);
    free(c); free(before); free(map);
    puts("web boot: missing/corrupt storage, init/open failure, read-size growth and valid reload passed");
}

static void test_read_and_preview(void)
{
    httpd_req_t r = request("/api/profiles", NULL); dispatch(&r, HTTP_GET, 200);
    cJSON *json = cJSON_Parse(r.response); assert(cJSON_IsArray(json) && cJSON_GetArraySize(json) == 3);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(json, 2), "id")->valuestring, "custom"));
    cJSON_Delete(json); release(&r);
    r = request("/api/config", NULL); dispatch(&r, HTTP_GET, 200);
    json = cJSON_Parse(r.response); assert(cJSON_IsObject(json));
    assert(!cJSON_HasObjectItem(json, "csv") && cJSON_GetObjectItemCaseSensitive(json, "revision")->valuedouble == 1);
    cJSON_Delete(json); release(&r);
    r = request("/", NULL); dispatch(&r, HTTP_GET, 200); assert(!strcmp(r.response, "test-page")); release(&r);
    r = request("/api/template.csv", NULL); dispatch(&r, HTTP_GET, 200); assert(!strcmp(r.response, "test-template")); release(&r);
    json = cJSON_CreateObject(); cJSON_AddStringToObject(json, "csv", fixture);
    char *text = cJSON_PrintUnformatted(json); cJSON_Delete(json); assert(text);
    r = request("/api/validate", text); dispatch(&r, HTTP_POST, 200);
    json = cJSON_Parse(r.response); assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "ok")));
    assert(cJSON_GetObjectItemCaseSensitive(json, "point_count")->valuedouble == 1);
    cJSON_Delete(json); release(&r); free(text);
    r = request("/api/validate", "{\"csv\":\"bad\",\"csv\":\"bad\"}"); dispatch(&r, HTTP_POST, 400); release(&r);
    r = request("/api/validate", "{\"csv\":\"bad\\u0000suffix\"}"); dispatch(&r, HTTP_POST, 400); release(&r);
    r = request("/api/validate", "{\"csv\":\"invalid\"} extra"); dispatch(&r, HTTP_POST, 400); release(&r);
    r = request("/api/validate", "{\"csv\":{\"nested\":1}}"); dispatch(&r, HTTP_POST, 400); release(&r);
    char deep[20003];
    memset(deep, '[', 10000); deep[10000] = '0'; memset(deep + 10001, ']', 10000); deep[20001] = 0;
    r = request("/api/validate", deep); dispatch(&r, HTTP_POST, 400); release(&r);
    r = request("/api/config", deep); dispatch(&r, HTTP_POST, 400); release(&r);
    const char literal[] = CUSTOM_CSV_HEADER
        "\n42,{Room}[Temperature],AI,3,0,u16,AB,0.1,0,62,,,,1000,Literal \\u0000 {braces} [array]\n";
    json = cJSON_CreateObject(); cJSON_AddStringToObject(json, "csv", literal);
    text = cJSON_PrintUnformatted(json); cJSON_Delete(json); assert(text);
    r = request("/api/validate", text); dispatch(&r, HTTP_POST, 200);
    json = cJSON_Parse(r.response); assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "ok")));
    cJSON_Delete(json); release(&r); free(text);
    assert(!sets && !commits && !timer_starts);
    puts("web: registered GET routes, profile list, settings, embedded assets and transactional CSV preview passed");
}

static void test_rejected_saves(void)
{
    char *text = config_text(&active_config, false);
    httpd_req_t r = request("/api/config", text); r.request_header = NULL;
    dispatch(&r, HTTP_POST, 403); release(&r);
    r = request("/api/config", text); r.content_type = "text/plain";
    dispatch(&r, HTTP_POST, 415); release(&r);
    r = request("/api/config", text); r.content_len = GW_CONFIG_MAX_JSON + 1;
    dispatch(&r, HTTP_POST, 413); release(&r);
    r = request("/api/config", text); r.fail_receive = true;
    dispatch(&r, HTTP_POST, 408); release(&r);
    r = request("/api/config", text); r.content_len += 2;
    dispatch(&r, HTTP_POST, 408); release(&r);
    save_expect("{}", 400);
    gateway_config_t modified = active_config; modified.revision = 2;
    char *stale = config_text(&modified, false); save_expect(stale, 409); free(stale);
    modified = active_config; modified.profile = GW_PROFILE_CUSTOM;
    char *bad = config_text(&modified, false); save_expect(bad, 400); free(bad);
    assert(!sets && !commits && !timer_starts && active_config.revision == 1 && !active_map.count);
    unsigned old_revision = active_config.revision;
    active_config.revision = UINT32_MAX;
    char *max = config_text(&active_config, false); save_expect(max, 409); free(max);
    active_config.revision = old_revision;
    set_error = ESP_FAIL; save_expect(text, 500); set_error = ESP_OK;
    assert(sets == 1 && commits == 0 && !timer_starts);
    commit_error = ESP_FAIL; save_expect(text, 500); commit_error = ESP_OK;
    assert(sets == 2 && commits == 1 && !timer_starts && !restarts);
    free(text);
    puts("web save: required headers, bounds, incomplete upload, invalid/stale/exhausted config and storage failures avoid restart passed");
}

static void test_persisted_custom_save(void)
{
    gateway_config_t *candidate = malloc(sizeof(*candidate)); assert(candidate);
    *candidate = active_config; candidate->profile = GW_PROFILE_CUSTOM;
    snprintf(candidate->csv, sizeof(candidate->csv), "%s", fixture);
    snprintf(candidate->device_name, sizeof(candidate->device_name), "Uploaded-Device");
    char *text = config_text(candidate, true);
    httpd_req_t r = request("/api/config", text); dispatch(&r, HTTP_POST, 200);
    cJSON *json = cJSON_Parse(r.response);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "ok")));
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "restart")));
    assert(cJSON_GetObjectItemCaseSensitive(json, "revision")->valuedouble == 2);
    cJSON_Delete(json); release(&r);
    assert(timer_starts == 1 && !restarts && active_config.profile == GW_PROFILE_FULL && !active_map.count);
    unsigned stored_sets = sets, stored_commits = commits;
    save_expect(text, 409); assert(sets == stored_sets && commits == stored_commits && timer_starts == 1);
    gateway_config_t *loaded = malloc(sizeof(*loaded)); custom_map_t *map = calloc(1, sizeof(*map));
    assert(loaded && map); gateway_config_defaults(loaded); char error[192];
    gateway_web_load(loaded, map, error, sizeof(error));
    assert(!error[0] && loaded->profile == GW_PROFILE_CUSTOM && loaded->revision == 2);
    assert(!strcmp(loaded->csv, fixture) && map->count == 1 && map->defs[0].instance == 42);
    assert(!strcmp(map->defs[0].name, "Room-Temperature"));
    candidate->revision = 2; assert(!memcmp(candidate, loaded, sizeof(*candidate)));
    timer_state.callback(timer_state.arg); assert(restarts == 1);
    free(candidate); free(loaded); free(map); free(text);
    puts("web save: commit before reply before delayed reboot; pending save blocked; persisted CSV reload recreates identical catalog passed");
}

int main(void)
{
    gateway_config_defaults(&active_config);
    test_boot_load_errors();
    gateway_web_start(&active_config, &active_map, "", empty_json, empty_json);
    assert(route_count == 9);
    test_read_and_preview(); test_rejected_saves(); test_persisted_custom_save();
    free(durable); free(staged);
    puts("production gateway HTTP/NVS shim tests passed; no network or flash I/O");
    return 0;
}
