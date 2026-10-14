#pragma once

// PIN rules that need no hardware (host-tested): the lockout after wrong PINs
// and the text form of the wipe PIN's salted hash (config.json `pin.wipe`).
//
// Lockout: 10 wrong PINs in a row lock PIN entry for `lock_seconds`; then
// 10 more tries. The count and the deadline (UTC) are in NVS, so a reboot
// does not reset them. At boot the deadline is believed only with a known
// clock and only if it is not further away than a full lock (a clock moved
// back); otherwise the full lock starts again. While running, the lock counts
// on the uptime clock (a `date set` does not shorten it).
//
// Wipe PIN: "<salt: 32 hex>:<SHA-256(salt || PIN): 64 hex>", lower case.

#include <cstddef>
#include <cstdint>

namespace vault {

inline constexpr uint8_t kMaxPinTries = 10;
inline constexpr size_t kWipeSaltSize = 16;
inline constexpr size_t kWipeHashSize = 32;
inline constexpr size_t kWipeTextSize = 2 * kWipeSaltSize + 1 + 2 * kWipeHashSize + 1;

// Seconds of lock left at boot. `deadline_utc`, `now_utc`: 0 when unknown.
uint32_t LockSecondsAtBoot(uint8_t failures, int64_t deadline_utc, int64_t now_utc,
                           uint32_t lock_seconds);

bool ParseWipe(const char *text, uint8_t salt[kWipeSaltSize], uint8_t hash[kWipeHashSize]);
void FormatWipe(const uint8_t salt[kWipeSaltSize], const uint8_t hash[kWipeHashSize],
                char out[kWipeTextSize]);

}  // namespace vault
