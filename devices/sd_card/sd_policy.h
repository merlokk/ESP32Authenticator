#pragma once

// The pure half of the SD card (host-tested, with sd_policy.cpp): the mount
// retry plan, paths under the mount point, sizes for people. The mount
// sequence is from FreeInk
// (docs/xteink-x4pro-support.md, SdmmcBlockDevice::begin): power-cycle the
// card before every attempt, validate with a real sector read, keep the power
// on afterwards.

#include <cstddef>
#include <cstdint>

namespace sd_card {

// The power enable (active low) before each attempt: off this long, then on
// this long before the first command.
inline constexpr uint32_t kPowerOffMs = 80;
inline constexpr uint32_t kPowerOnMs = 120;

inline constexpr int kMaxAttempts = 4;

// What an attempt ran into, as the driver sees it.
enum class Outcome : uint8_t {
    kOk,
    kNoAnswer,      // the card does not answer (none inserted, or not yet)
    kNoFilesystem,  // a card, but no FAT on it: never formatted here
    kFailed,        // anything else: a marginal data path, a bad read
};

// Clock for attempt `i` (0-based): 40 MHz twice, then 20 MHz.
constexpr uint32_t AttemptKhz(int i) { return i < 2 ? 40000 : 20000; }

// Whether to try again after attempt `i` ended with `o`, given how many
// attempts in a row the card did not answer. A missing card costs two
// attempts; a card without FAT none (retrying does not make one).
constexpr bool Retry(int i, Outcome o, int no_answer_in_a_row) {
    if (o == Outcome::kOk || o == Outcome::kNoFilesystem || i + 1 >= kMaxAttempts) {
        return false;
    }
    return o != Outcome::kNoAnswer || no_answer_in_a_row < 2;
}

constexpr const char *Name(Outcome o) {
    switch (o) {
        case Outcome::kOk: return "mounted";
        case Outcome::kNoAnswer: return "no card (no answer)";
        case Outcome::kNoFilesystem: return "no FAT file system (left as is)";
        case Outcome::kFailed: return "failed";
    }
    return "?";
}

// `base` + "/" + `rel` into `out`. Refuses ".." components (nothing outside
// the card), a too long result, and control characters. A leading "/" in
// `rel` is the card's root; "" or "/" is the root itself. No trailing "/".
bool JoinPath(const char *base, const char *rel, char *out, size_t size);

// 1023 B, 1.5 KB, 31.9 MB, 29.7 GB (binary units, one decimal).
void FormatBytes(uint64_t bytes, char *out, size_t size);

}  // namespace sd_card
