#pragma once

// The U2F / FIDO2 list (x4-design.md): a row per credential, kRowsPerPage a
// page, by site (A to Z, case not minded), then user. A row: the site, the
// user, then when it was made, the algorithm and the kind; a key is deleted
// from its row (Remove). Pure (host-tested).

#include <cstddef>
#include <cstdint>

#include "nav_logic.h"
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

// The row a point at `y` is on (rows of `height` from `top`), kNoRow off them.
inline constexpr uint8_t kNoRow = 0xff;

constexpr uint8_t RowAt(int32_t y, int32_t top, int32_t height, uint8_t rows) {
    if (y < top || height <= 0) {
        return kNoRow;
    }
    const int32_t i = (y - top) / height;
    return i < rows ? static_cast<uint8_t>(i) : kNoRow;
}

// Deleting a key: a swipe left on a row shows a trash can at its right end
// (one row at a time; a swipe right, a tap elsewhere, a page flip or a redraw
// takes it away). The can asks; No answers at once, Yes only held kHoldMs,
// filling in kHoldSteps (let go early: empty again). Held long enough: kDelete,
// the caller deletes the key and calls Clear().
inline constexpr uint32_t kHoldMs = 4000;
inline constexpr uint8_t kHoldSteps = 8;

enum class Stage : uint8_t {
    kNone,
    kTrash,   // the can on row()
    kAsk,     // the question over the list
    kDelete,  // Yes held long enough
};

class Remove {
   public:
    constexpr Stage stage() const { return stage_; }
    constexpr uint8_t row() const { return row_; }
    constexpr bool asking() const { return stage_ == Stage::kAsk || stage_ == Stage::kDelete; }

    // A swipe that started on `row` (kNoRow: off the rows). True when the can moved.
    constexpr bool Swipe(nav::Gesture g, uint8_t row) {
        if (asking()) {
            return false;
        }
        if (g == nav::Gesture::kLeft && row != kNoRow) {
            const bool moved = stage_ != Stage::kTrash || row_ != row;
            stage_ = Stage::kTrash;
            row_ = row;
            return moved;
        }
        return g == nav::Gesture::kRight && Clear();
    }

    // A tap on a row but the can, or the list redrawn: the can goes.
    constexpr bool Drop() { return stage_ == Stage::kTrash && Clear(); }

    // The can tapped: the question. True when asked.
    constexpr bool Trash() {
        if (stage_ != Stage::kTrash) {
            return false;
        }
        stage_ = Stage::kAsk;
        return true;
    }

    // No (or the list left): nothing to delete, no can. True when it changed.
    constexpr bool Clear() {
        const bool changed = stage_ != Stage::kNone;
        stage_ = Stage::kNone;
        row_ = kNoRow;
        holding_ = false;
        return changed;
    }

    // Yes pressed at `now` (ms), let go.
    constexpr void Press(uint32_t now) {
        if (stage_ == Stage::kAsk) {
            holding_ = true;
            from_ = now;
        }
    }
    constexpr void Release() { holding_ = false; }

    // Yes's fill at `now`, 0..kHoldSteps; kHoldSteps: held long enough (kDelete).
    constexpr uint8_t Fill(uint32_t now) {
        if (stage_ == Stage::kDelete) {
            return kHoldSteps;
        }
        if (!holding_) {
            return 0;
        }
        const uint32_t held = now - from_;
        if (held >= kHoldMs) {
            stage_ = Stage::kDelete;
            holding_ = false;
            return kHoldSteps;
        }
        return static_cast<uint8_t>(held * kHoldSteps / kHoldMs);
    }

   private:
    Stage stage_ = Stage::kNone;
    uint8_t row_ = kNoRow;
    bool holding_ = false;
    uint32_t from_ = 0;
};

}  // namespace ui::fido_list
