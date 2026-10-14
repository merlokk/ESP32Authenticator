#pragma once

// Applies a time zone to libc (`TZ` + tzset): localtime_r, strftime and mktime
// follow it. The clock itself stays UTC. Names and rules: zones.h.

#include <ctime>

#include "esp_err.h"
#include "zones.h"

namespace tz {

// A table name or a raw POSIX rule (zones.h Resolve). ESP_ERR_INVALID_ARG for
// anything else, and the current zone is kept.
esp_err_t ApplyZone(const char *zone);

// The POSIX rule in force ("UTC0" until a zone is applied).
const char *Current();

// Offset from UTC in seconds at `when` (0: now), DST included. Positive east
// of Greenwich: the opposite of the sign inside a POSIX rule.
int OffsetSeconds(time_t when = 0);

bool IsDaylightSaving(time_t when = 0);

}  // namespace tz
