#ifndef TEST_HISTORY_TASK_H
#define TEST_HISTORY_TASK_H
#include "freertos/FreeRTOS.h"
void vTaskDelay(uint32_t);
BaseType_t xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, void *);
#endif
