#pragma once

#include "freertos/FreeRTOS.h"

typedef struct {
    int taken;
} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;

inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) {
    storage->taken = 0;
    return storage;
}

inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) {
    (void)wait;
    s->taken = 1;
    return pdTRUE;
}

inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    s->taken = 0;
    return pdTRUE;
}
