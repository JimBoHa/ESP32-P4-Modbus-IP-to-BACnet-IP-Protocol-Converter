#ifndef TEST_ESP_TIMER_H
#define TEST_ESP_TIMER_H
#include <stdint.h>
#include "esp_err.h"
typedef struct test_esp_timer *esp_timer_handle_t;
typedef struct { void (*callback)(void *); void *arg; const char *name; } esp_timer_create_args_t;
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout);
int64_t esp_timer_get_time(void);
#endif
