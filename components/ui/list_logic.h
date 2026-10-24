#pragma once

// The password list (x4-design.md): a row per record, kRowsPerPage a page,
// sorted as the vault keeps them (site, then user). A row shows the site,
// the user, the password hidden and the TOTP code if the record has one; it
// is cut in four across, a tap in a quarter: type the user, type the
// password, show the password, type the TOTP code. Pure (host-tested).

#include <cstddef>
#include <cstdint>

namespace ui::list {

inline constexpr uint8_t kRowsPerPage = 6;
inline constexpr uint32_t kRevealMs = 20 * 1000;  // a shown password hides again

enum class Zone : uint8_t {
    kTypeUser,
    kTypePassword,
    kShowPassword,
    kTypeTotp,
};

inline constexpr const char *kZoneNames[4] = {"user", "password", "show", "TOTP"};

// The quarter of a row `width` wide that `x` (from the row's left) is in.
constexpr Zone ZoneAt(int32_t x, int32_t width) {
    if (width <= 0 || x < 0) {
        return Zone::kTypeUser;
    }
    const int32_t q = x * 4 / width;
    return static_cast<Zone>(q > 3 ? 3 : q);
}

// The records on `page`: list positions first .. first + count - 1.
struct Span {
    size_t first;
    uint8_t count;
};

constexpr Span PageSpan(size_t records, uint8_t page) {
    const size_t first = static_cast<size_t>(page) * kRowsPerPage;
    if (first >= records) {
        return {first, 0};
    }
    const size_t left = records - first;
    return {first, static_cast<uint8_t>(left < kRowsPerPage ? left : kRowsPerPage)};
}

// "123 456" (6 digits, leading zeros) into `out` (8 bytes at least).
constexpr void FormatCode(uint32_t code, char *out) {
    code %= 1000000;
    for (int i = 6; i >= 0; --i) {
        if (i == 3) {
            out[i] = ' ';
            continue;
        }
        out[i] = static_cast<char>('0' + code % 10);
        code /= 10;
    }
    out[7] = '\0';
}

// A TOTP window number: the codes change when it does.
constexpr int64_t Window(int64_t now, uint32_t step = 30) {
    return now < 0 ? -1 : now / step;
}

}  // namespace ui::list
