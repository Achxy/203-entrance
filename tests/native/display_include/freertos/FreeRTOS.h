#pragma once
#include <pthread.h>
#include <stddef.h>
typedef pthread_mutex_t *SemaphoreHandle_t;
#define portMAX_DELAY 0u
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define tskIDLE_PRIORITY 0
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xTaskCreate(void (*task)(void *), const char *, size_t, void *, unsigned, void *);
void vTaskDelay(unsigned ticks);
