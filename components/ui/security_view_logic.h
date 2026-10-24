#pragma once

// The Security window of Settings (x4-design.md): the vault and the FIDO2
// store, Change the PIN, the lock after wrong PINs, PIN complexity, the wipe
// PIN, Lock now. The PINs are typed on a pad of the window's own (Flow over
// pin_pad_logic.h): the current PIN, then a new one twice; or a wipe PIN
// twice. The device fills View and carries out the changes (ui::SecurityHooks).
// Pure (host-tested).

#include <cstddef>
#include <cstdint>

#include "pin_pad_logic.h"

namespace ui::security_view {

// --- the lock after kMaxPinTries wrong PINs (pin.lockMinutes, 1..1440)

struct Option {
    uint16_t value;
    const char *label;
};

inline constexpr Option kLock[] = {{1, "1 min"},   {5, "5 min"},  {10, "10 min"},
                                   {30, "30 min"}, {60, "1 hour"}, {1440, "A day"}};
inline constexpr uint8_t kLockCount = sizeof kLock / sizeof kLock[0];

constexpr int LockIndex(uint16_t minutes) {
    for (uint8_t i = 0; i < kLockCount; ++i) {
        if (kLock[i].value == minutes) {
            return i;
        }
    }
    return -1;
}

// --- typing the PINs

enum class Flow : uint8_t {
    kChangePin,  // the current PIN, then a new one twice
    kSetWipe,    // a wipe PIN twice
};

enum class Result : uint8_t {
    kNone,      // nothing changed
    kTyped,     // the dots changed
    kNext,      // the next PIN: a new title, empty dots
    kMismatch,  // the two differ: the new one from the start
    kSubmit,    // all typed: old() (kChangePin) and fresh() are ready
};

class PinFlow {
   public:
    void Start(Flow f) {
        Wipe();
        flow_ = f;
        have_old_ = false;
        pad_.Start(f == Flow::kSetWipe);  // the wipe PIN: a new one twice
    }

    Flow flow() const { return flow_; }
    uint8_t count() const { return pad_.count(); }
    const char *old() const { return old_; }
    const char *fresh() const { return fresh_; }

    const char *Title() const {
        const pinpad::Stage s = pad_.stage();
        if (flow_ == Flow::kSetWipe) {
            return s == pinpad::Stage::kRepeat ? "Repeat the wipe PIN" : "The wipe PIN";
        }
        return !have_old_                    ? "The current PIN"
               : s == pinpad::Stage::kRepeat ? "Repeat the new PIN"
                                             : "A new PIN";
    }

    Result Apply(pinpad::Press p) {
        switch (pad_.Apply(p)) {
            case pinpad::Step::kNone: return Result::kNone;
            case pinpad::Step::kTyped: return Result::kTyped;
            case pinpad::Step::kRepeat: return Result::kNext;
            case pinpad::Step::kMismatch: return Result::kMismatch;
            case pinpad::Step::kCheck: break;
        }
        if (flow_ == Flow::kChangePin && !have_old_) {
            Copy(old_, pad_.Pin());
            have_old_ = true;
            pad_.Start(true);  // the new one, twice
            return Result::kNext;
        }
        Copy(fresh_, pad_.Pin());
        pad_.Wipe();
        return Result::kSubmit;
    }

    void Wipe() {
        pad_.Wipe();
        Erase(old_, sizeof old_);
        Erase(fresh_, sizeof fresh_);
    }

   private:
    static void Erase(char *p, size_t n) {
        volatile char *v = p;
        for (size_t i = 0; i < n; ++i) {
            v[i] = 0;
        }
    }
    static void Copy(char *to, const char *from) {
        for (uint8_t i = 0; i < pinpad::kDigits; ++i) {
            to[i] = from[i];
        }
        to[pinpad::kDigits] = '\0';
    }

    pinpad::Pad pad_;
    Flow flow_ = Flow::kChangePin;
    bool have_old_ = false;
    char old_[pinpad::kDigits + 1] = {};
    char fresh_[pinpad::kDigits + 1] = {};
};

// --- what the window shows

struct View {
    bool open;             // the vault
    uint16_t records;
    bool fido_known;       // the store answered (not while a request waits)
    uint16_t fido_keys;
    uint16_t fido_max;
    bool fido_pin;         // FIDO2's own PIN is set
    uint8_t wrong;         // wrong PINs in a row
    uint8_t max_wrong;     // vault::kMaxPinTries
    uint16_t lock_minutes;
    bool wipe;             // a wipe PIN is set
    bool complexity;
};

constexpr bool Equal(const View &a, const View &b) {
    return a.open == b.open && a.records == b.records && a.fido_known == b.fido_known &&
           a.fido_keys == b.fido_keys && a.fido_max == b.fido_max && a.fido_pin == b.fido_pin &&
           a.wrong == b.wrong && a.max_wrong == b.max_wrong && a.lock_minutes == b.lock_minutes &&
           a.wipe == b.wipe && a.complexity == b.complexity;
}

}  // namespace ui::security_view
