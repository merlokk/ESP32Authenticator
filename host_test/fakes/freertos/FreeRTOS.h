#pragma once

// The FreeRTOS names the code under test uses. The host tests are single
// threaded: a mutex is always free.

#include <cstdint>

typedef uint32_t TickType_t;
typedef int BaseType_t;

#define pdTRUE 1
#define portMAX_DELAY ((TickType_t)0xffffffffUL)
