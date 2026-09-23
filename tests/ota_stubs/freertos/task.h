#ifndef TEST_OTA_TASK_H
#define TEST_OTA_TASK_H
#include "freertos/FreeRTOS.h"
void vTaskDelay(uint32_t);
void vTaskDelete(void *);
BaseType_t xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, void *);
#endif
