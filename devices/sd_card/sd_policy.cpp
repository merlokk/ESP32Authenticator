#include "sd_policy.h"

#include <cstdio>
#include <cstring>

namespace sd_card {

bool JoinPath(const char *base, const char *rel, char *out, size_t size) {
    while (*rel == '/') {
        ++rel;
    }
    for (const char *p = rel; *p != '\0'; ++p) {
        if (static_cast<unsigned char>(*p) < 0x20) {
            return false;
        }
        const bool starts = p == rel || p[-1] == '/';
        if (starts && p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
            return false;
        }
    }
    const int n = *rel == '\0' ? snprintf(out, size, "%s", base)
                               : snprintf(out, size, "%s/%s", base, rel);
    if (n < 0 || static_cast<size_t>(n) >= size) {
        return false;
    }
    // No trailing "/" (opendir and stat do not need one).
    for (size_t len = strlen(out); len > strlen(base) && out[len - 1] == '/'; --len) {
        out[len - 1] = '\0';
    }
    return true;
}

void FormatBytes(uint64_t bytes, char *out, size_t size) {
    static constexpr const char *kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    if (bytes < 1024) {
        snprintf(out, size, "%u B", static_cast<unsigned>(bytes));
        return;
    }
    size_t unit = 0;
    uint64_t scaled10 = bytes * 10;  // tenths
    while (scaled10 >= 1024 * 10 && unit + 1 < sizeof kUnits / sizeof kUnits[0]) {
        scaled10 = (scaled10 + 512) / 1024;
        ++unit;
    }
    snprintf(out, size, "%u.%u %s", static_cast<unsigned>(scaled10 / 10),
             static_cast<unsigned>(scaled10 % 10), kUnits[unit]);
}

}  // namespace sd_card
