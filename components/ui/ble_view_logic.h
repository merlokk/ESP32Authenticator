#pragma once

// The Bluetooth window of Settings (x4-design.md): the switch, the state and
// name, the paired hosts (a tap: use only this one / any of them, forget);
// "Pair a new host" opens the pairing window and shows the passkey the host
// asks for. The device fills View and Pairing (ui::BleHooks). Pure
// (host-tested).

#include <cstddef>
#include <cstdint>

#include "status_logic.h"

namespace ui::ble_view {

inline constexpr uint8_t kMaxHosts = 8;  // CONFIG_BT_NIMBLE_MAX_BONDS
inline constexpr size_t kAddrSize = 18;  // "aa:bb:cc:dd:ee:ff"
inline constexpr uint32_t kPairSeconds = 60;

struct Host {
    char addr[kAddrSize];
    bool connected;
    bool target;  // the only one let in (ble::UseBond)
};

// What the window shows; drawn again when it changes.
struct View {
    bool active;     // config ble.active
    bool on;         // the stack runs
    char state[24];  // "connected", "waiting", ...
    char name[32];
    uint8_t host_count;
    Host hosts[kMaxHosts];
};

constexpr bool Same(const char *a, const char *b) {
    for (; *a != '\0' && *a == *b; ++a, ++b) {
    }
    return *a == *b;
}

constexpr bool Equal(const View &a, const View &b) {
    if (a.active != b.active || a.on != b.on || !Same(a.state, b.state) ||
        !Same(a.name, b.name) || a.host_count != b.host_count) {
        return false;
    }
    for (uint8_t i = 0; i < a.host_count && i < kMaxHosts; ++i) {
        if (!Same(a.hosts[i].addr, b.hosts[i].addr) || a.hosts[i].connected != b.hosts[i].connected ||
            a.hosts[i].target != b.hosts[i].target) {
            return false;
        }
    }
    return true;
}

// The pairing page: what happened inside the window (ble::PairingEvent).
enum class Pairing : uint8_t {
    kWaiting,  // for a host to ask
    kPasskey,  // the host asks for passkey()
    kPaired,
    kFailed,
    kClosed,   // the window ran out
};

class PairPage {
   public:
    constexpr Pairing state() const { return state_; }
    constexpr uint32_t passkey() const { return passkey_; }

    constexpr void Start() {
        state_ = Pairing::kWaiting;
        passkey_ = 0;
    }
    // An event; true when the page changes.
    constexpr bool Passkey(uint32_t key) {
        const bool changed = state_ != Pairing::kPasskey || passkey_ != key;
        state_ = Pairing::kPasskey;
        passkey_ = key;
        return changed;
    }
    constexpr bool Paired() { return Set(Pairing::kPaired); }
    // The host may try again inside the window.
    constexpr bool Failed() { return Set(Pairing::kFailed); }
    // The window's seconds left: 0 closes it (unless paired).
    constexpr bool Left(uint32_t seconds) {
        return seconds == 0 && state_ != Pairing::kPaired && Set(Pairing::kClosed);
    }

   private:
    constexpr bool Set(Pairing p) {
        const bool changed = state_ != p;
        state_ = p;
        return changed;
    }
    Pairing state_ = Pairing::kWaiting;
    uint32_t passkey_ = 0;
};

// "123 456": six digits, split for reading off the glass.
constexpr void Passkey(uint32_t key, char *out, size_t size) {
    status::Text t(out, size);
    t.Number(key / 1000 % 1000, 3).Add(" ").Number(key % 1000, 3);
}

}  // namespace ui::ble_view
