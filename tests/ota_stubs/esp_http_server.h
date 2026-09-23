#ifndef TEST_OTA_HTTP_H
#define TEST_OTA_HTTP_H
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include "esp_err.h"
#define HTTP_GET 0
#define HTTP_POST 1
#define HTTPD_RESP_USE_STRLEN -1
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define HTTPD_SOCK_ERR_TIMEOUT -11
typedef void *httpd_handle_t;
typedef struct {
    const char *uri;
    int method;
    size_t content_len;
    const char *body, *authorization, *content_type, *project;
    size_t body_length, received, chunk_size;
    unsigned receive_timeouts;
    bool response_sent;
    char status[64], response_type[64];
    char *response;
} httpd_req_t;
typedef struct { const char *uri; int method; esp_err_t (*handler)(httpd_req_t *); } httpd_uri_t;
typedef struct {
    unsigned max_uri_handlers, max_resp_headers, ctrl_port, max_open_sockets, stack_size;
    bool lru_purge_enable;
    unsigned recv_wait_timeout, send_wait_timeout;
} httpd_config_t;
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *);
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *);
esp_err_t httpd_resp_send(httpd_req_t *, const char *, ssize_t);
esp_err_t httpd_resp_send_err(httpd_req_t *, int, const char *);
size_t httpd_req_get_hdr_value_len(httpd_req_t *, const char *);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *, size_t);
int httpd_req_recv(httpd_req_t *, char *, size_t);
#endif
