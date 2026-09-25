#ifndef TEST_HISTORY_SEMPHR_H
#define TEST_HISTORY_SEMPHR_H
#include <stdint.h>
#include "freertos/FreeRTOS.h"
typedef struct history_test_mutex *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, uint32_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
#endif
