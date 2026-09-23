#ifndef TEST_OTA_ESP_ERR_H
#define TEST_OTA_ESP_ERR_H
#include <assert.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_OTA_VALIDATE_FAILED 0x1503
#define ESP_ERROR_CHECK(x) assert((x) == ESP_OK)
#endif
