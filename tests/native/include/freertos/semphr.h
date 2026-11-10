#pragma once
#include "FreeRTOS.h"
static inline int xSemaphoreTake(SemaphoreHandle_t m, unsigned timeout) { (void)timeout; return pthread_mutex_lock(m)==0; }
static inline void xSemaphoreGive(SemaphoreHandle_t m) { pthread_mutex_unlock(m); }
