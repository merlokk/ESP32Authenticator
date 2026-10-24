#pragma once

// The U2F / FIDO2 list (x4-design.md): a row per credential, kRowsPerPage a
// page, by site (A to Z, case not minded), then user. A row: the site, the
// user, then when it was made, the algorithm and the kind. Pure (host-tested).

#include <cstddef>
#include <cstdint>

#include "status_logic.h"

namespace ui::fido_list {

inline constexpr uint8_t kRowsPerPage = 6;

constexpr char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// -1, 0, 1: a before, as, after b, case not minded.
constexpr int Compare(const char *a, const char *b) {
    for (;; ++a, ++b) {
        const char x = Lower(*a);
        const char y = Lower(*b);
        if (x != y) {
            return static_cast<unsigned char>(x) < static_cast<unsigned char>(y) ? -1 : 1;
        }
        if (x == '\0') {
            return 0;
        }
    }
}

// `order` (n positions) sorted by `less(i, j)` on the positions; equal ones
// keep their order (insertion: the store holds 125 at most).
template <typename Less>
constexpr void Sort(uint16_t *order, size_t n, Less less) {
    for (size_t i = 0; i < n; ++i) {
        order[i] = static_cast<uint16_t>(i);
    }
    for (size_t i = 1; i < n; ++i) {
        const uint16_t v = order[i];
        size_t j = i;
        for (; j > 0 && less(v, order[j - 1]); --j) {
            order[j] = order[j - 1];
        }
        order[j] = v;
    }
}

// The second line: "name (display name)", either alone, or "-".
constexpr void User(const char *name, const char *display, char *out, size_t size) {
    status::Text t(out, size);
    t.Add(name);
    if (display[0] != '\0' && Compare(display, name) != 0) {
        if (name[0] != '\0') {
            t.Add(" (").Add(display).Add(")");
        } else {
            t.Add(display);
        }
    }
    if (t.size() == 0) {
        t.Add("-");
    }
}

constexpr const char *AlgName(int32_t alg) {
    return alg == -7 ? "ES256" : alg == -8 ? "EdDSA" : alg == -19 ? "Ed25519" : "?";
}

// The third line: "made 2026-10-24, ES256, resident" (no "made" while the
// date is unknown: year 0); U2F keys say "U2F" for the kind.
constexpr void Details(uint16_t year, uint8_t month, uint8_t day, int32_t alg, bool u2f,
                       bool discoverable, char *out, size_t size) {
    status::Text t(out, size);
    if (year != 0) {
        t.Add("made ").Number(year, 4).Add("-").Number(month, 2).Add("-").Number(day, 2).Add(", ");
    }
    t.Add(AlgName(alg)).Add(", ").Add(u2f ? "U2F" : discoverable ? "resident" : "non-resident");
}

}  // namespace ui::fido_list
