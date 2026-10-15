#pragma once

// heap_caps_calloc: the host heap (no PSRAM).

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_SPIRAM (1 << 10)

inline void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    (void)caps;
    return std::calloc(n, size);
}
