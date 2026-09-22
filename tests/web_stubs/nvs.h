#ifndef TEST_NVS_H
#define TEST_NVS_H
#include <stddef.h>
#include "esp_err.h"
typedef unsigned nvs_handle_t;
#define NVS_READWRITE 1
esp_err_t nvs_open_from_partition(const char *partition, const char *name, int mode, nvs_handle_t *handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);
#endif
