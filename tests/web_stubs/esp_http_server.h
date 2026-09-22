#ifndef TEST_ESP_HTTP_SERVER_H
#define TEST_ESP_HTTP_SERVER_H
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include "esp_err.h"
#define HTTP_GET 0
#define HTTP_POST 1
#define HTTPD_RESP_USE_STRLEN -1
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
/* Only fields accessed by gateway_web.c plus in-memory transport state. */
typedef struct {
    const char *uri;
    size_t content_len;
    const char *body, *request_header, *content_type;
    size_t body_length, received, chunk_size;
    bool fail_receive, response_sent;
    char status[64], response_type[64];
    char *response;
    unsigned sequence;
} httpd_req_t;
typedef void *httpd_handle_t;
typedef struct {
    const char *uri;
    int method;
    esp_err_t (*handler)(httpd_req_t *request);
} httpd_uri_t;
typedef struct {
    unsigned stack_size, max_open_sockets, max_uri_handlers;
    bool lru_purge_enable;
    unsigned recv_wait_timeout, send_wait_timeout;
} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *handler);
esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *name, const char *value);
esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status);
esp_err_t httpd_resp_send(httpd_req_t *request, const char *data, ssize_t length);
esp_err_t httpd_resp_send_err(httpd_req_t *request, int code, const char *message);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *request, const char *name, char *out, size_t size);
int httpd_req_recv(httpd_req_t *request, char *out, size_t length);
#endif
