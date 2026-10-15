#pragma once

// esp_timer_get_time: microseconds since boot; fake::now_us, set by tests.

#include <cstdint>

namespace fake {
extern int64_t now_us;
}  // namespace fake

inline int64_t esp_timer_get_time(void) { return fake::now_us; }
