#pragma once
#include <pthread.h>
typedef pthread_mutex_t *SemaphoreHandle_t;
#define portMAX_DELAY 0
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
