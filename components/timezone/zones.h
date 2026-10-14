#pragma once

// Named time zones -> POSIX TZ rules. Pure (host-tested); applying a rule is
// timezone.h. Ported from approver-esp32.
//
// The clock is UTC; a zone is presentation only. libc knows POSIX rules, not
// IANA names, and there is no tzdata in ESP-IDF: this is a curated table. A
// raw POSIX rule is accepted wherever a name is (a zone missing here, or one
// whose rules changed), so nobody waits for a release.

#include <cstddef>

// `tz`, not `timezone`: libc owns that word (a variable and a struct).
namespace tz {

struct Zone {
    const char *name;   // IANA-style, e.g. "Europe/Kyiv"
    const char *posix;  // what libc is given
};

// Longest name and rule in the table (+ NUL fits), for buffers.
inline constexpr size_t kMaxNameLength = 40;
inline constexpr size_t kMaxPosixLength = 40;

size_t Count();
const Zone &At(size_t index);  // past the end: the first row (UTC)

// The rule for a name, or nullptr. Case-insensitive, follows aliases
// (Europe/Kiev, Asia/Calcutta, GMT, ...).
const char *Lookup(const char *name);

// The table's first name for a rule, or nullptr (one rule serves many zones).
const char *NameFor(const char *posix);

// A cheap check that a string is a POSIX rule and not a misspelled name:
// libc would read a misspelled zone as UTC and say nothing.
bool LooksLikePosix(const char *value);

// What a config value means: a table name's rule, a raw rule as is, or nullptr.
const char *Resolve(const char *zone);

}  // namespace tz
