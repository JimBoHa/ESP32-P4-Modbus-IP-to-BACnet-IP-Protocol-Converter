#ifndef DASHBOARD_REDIRECT_H
#define DASHBOARD_REDIRECT_H

#include <stdint.h>
#include "esp_http_server.h"

/* Port 80 serves only GET/HEAD redirects, never management handlers or data. */
esp_err_t dashboard_redirect_start(uint16_t https_port);
void dashboard_redirect_stop(void);
esp_err_t dashboard_root_redirect_handler(httpd_req_t *request);

#endif
