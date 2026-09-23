#ifndef GATEWAY_OTA_H
#define GATEWAY_OTA_H
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* Arm before fallible startup work, once the application's health callback can
 * safely inspect its initially-false readiness/heartbeat fields. A pending
 * image resets by 60 seconds even if startup or the health task stalls. */
esp_err_t gateway_ota_begin_validation(bool (*healthy)(void));
/* Registers the three OTA routes and application web routes on one HTTPS
 * listener. Callback returns an error if ANY application route fails. */
esp_err_t gateway_ota_start(esp_err_t (*register_web)(httpd_handle_t));
bool gateway_ota_ready(void);
/* On denial sends 401/403/409/500. Caller returns ESP_FAIL to close unread body.
 * In a non-OTA build returns true; caller follows its normal HTTP policy. */
bool gateway_ota_authorize_mutation(httpd_req_t *request);
/* Config-save code calls this after commit, before sending its reboot reply. */
void gateway_ota_note_restart_pending(void);
#endif
