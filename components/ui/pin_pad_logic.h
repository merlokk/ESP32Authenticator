#pragma once

// The pure half of the PIN pad (host-tested): the keys of the 3x4 pad, the
// digits typed and the steps. A store that exists takes the PIN once; no
// store yet: a new PIN, typed twice (a typo would lock the store away).
// Six digits are checked as soon as the sixth is typed. The digits are wiped
// as soon as they are not needed.

#include <cstddef>
#include <cstdint>

namespace ui::pinpad {

inline constexpr uint8_t kDigits = 6;

enum class Key : uint8_t { kNone, kDigit, kBack, kClear };

struct Press {
    Key key;
    char digit;  // kDigit: '0'..'9'
};

// lv_buttonmatrix button ids: 1 2 3 / 4 5 6 / 7 8 9 / clear 0 back.
inline constexpr uint32_t kKeyCount = 12;

constexpr Press KeyAt(uint32_t id) {
    if (id < 9) {
        return {Key::kDigit, static_cast<char>('1' + id)};
    }
    switch (id) {
        case 9: return {Key::kClear, 0};
        case 10: return {Key::kDigit, '0'};
        case 11: return {Key::kBack, 0};
        default: return {Key::kNone, 0};
    }
}

enum class Stage : uint8_t {
    kEnter,     // the store's PIN
    kNew,       // no store: a new PIN
    kRepeat,    // the new PIN again
    kChecking,  // Pin() is being checked; keys are ignored
};

enum class Step : uint8_t {
    kNone,      // nothing changed
    kTyped,     // the dots changed
    kCheck,     // six digits: check Pin() (kChecking until Done)
    kRepeat,    // the new PIN once: type it again
    kMismatch,  // the two new PINs differ: a new one from the start
};

class Pad {
   public:
    void Start(bool new_store) {
        Wipe();
        stage_ = new_store ? Stage::kNew : Stage::kEnter;
    }

    Stage stage() const { return stage_; }
    uint8_t count() const { return count_; }
    // The six digits, NUL-terminated, while kChecking.
    const char *Pin() const { return typed_; }

    Step Apply(Press p) {
        if (stage_ == Stage::kChecking || p.key == Key::kNone) {
            return Step::kNone;
        }
        if (p.key == Key::kClear || p.key == Key::kBack) {
            if (count_ == 0) {
                return Step::kNone;
            }
            count_ = p.key == Key::kClear ? 0 : static_cast<uint8_t>(count_ - 1);
            Erase(typed_ + count_, kDigits - count_);
            return Step::kTyped;
        }
        if (p.digit < '0' || p.digit > '9' || count_ >= kDigits) {
            return Step::kNone;
        }
        typed_[count_++] = p.digit;
        if (count_ < kDigits) {
            return Step::kTyped;
        }
        switch (stage_) {
            case Stage::kNew:
                Copy(first_, typed_);
                ClearTyped();
                stage_ = Stage::kRepeat;
                return Step::kRepeat;
            case Stage::kRepeat:
                if (!Same(first_, typed_)) {
                    Wipe();
                    stage_ = Stage::kNew;
                    return Step::kMismatch;
                }
                Erase(first_, sizeof first_);
                stage_ = Stage::kChecking;
                return Step::kCheck;
            default:
                stage_ = Stage::kChecking;
                return Step::kCheck;
        }
    }

    // After the check (a right PIN closes the pad): the digits wiped, and
    // the PIN asked for again, as a new one while there is still no store.
    void Done(bool new_store) { Start(new_store); }

    void Wipe() {
        Erase(typed_, sizeof typed_);
        Erase(first_, sizeof first_);
        count_ = 0;
    }

   private:
    // Through a volatile pointer: the compiler may not drop it.
    static void Erase(char *p, uint32_t n) {
        volatile char *v = p;
        for (uint32_t i = 0; i < n; ++i) {
            v[i] = 0;
        }
    }
    static void Copy(char *to, const char *from) {
        for (uint8_t i = 0; i <= kDigits; ++i) {
            to[i] = from[i];
        }
    }
    static bool Same(const char *a, const char *b) {
        uint8_t diff = 0;
        for (uint8_t i = 0; i < kDigits; ++i) {
            diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
        }
        return diff == 0;
    }
    void ClearTyped() {
        Erase(typed_, sizeof typed_);
        count_ = 0;
    }

    char typed_[kDigits + 1] = {};
    char first_[kDigits + 1] = {};
    uint8_t count_ = 0;
    Stage stage_ = Stage::kEnter;
};

// What the pad says under the dots after a wrong PIN: the tries left before
// the lock (`failures` of `max` so far), or the lock's minutes, rounded up.
constexpr uint8_t TriesLeft(uint8_t failures, uint8_t max) {
    return failures < max ? static_cast<uint8_t>(max - failures) : 0;
}
constexpr uint32_t MinutesUp(uint32_t seconds) { return (seconds + 59) / 60; }

// A new PIN makes a store only where there never was one: a device that took
// a vault.bin (`kept_version`, NVS) whose file is gone now (the card lent to a
// USB host, pulled out) asks for the PIN as ever (the vault says why it fails).
constexpr bool NewStore(size_t file_size, uint32_t kept_version) {
    return file_size == 0 && kept_version == 0;
}

// Nothing typed yet: the pad may change its mind (the file came or went).
constexpr bool Untouched(Stage stage, uint8_t count) {
    return (stage == Stage::kEnter || stage == Stage::kNew) && count == 0;
}

}  // namespace ui::pinpad
