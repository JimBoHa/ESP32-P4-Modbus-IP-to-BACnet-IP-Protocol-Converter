#ifndef TEST_OTA_HTTPS_H
#define TEST_OTA_HTTPS_H
#include "esp_http_server.h"
typedef struct {
    httpd_config_t httpd;
    unsigned port_secure;
    const unsigned char *servercert, *prvtkey_pem;
    size_t servercert_len, prvtkey_len;
} httpd_ssl_config_t;
#define HTTPD_SSL_CONFIG_DEFAULT() ((httpd_ssl_config_t){0})
esp_err_t httpd_ssl_start(httpd_handle_t *, const httpd_ssl_config_t *);
esp_err_t httpd_ssl_stop(httpd_handle_t);
#endif
