#pragma once

// When the web server should be up (ported from approver-esp32).
// <cstdint> only: host-tested.

#include <cstdint>

namespace web {

enum class Desired : uint8_t {
    kOff = 0,
    kOn,    // whenever there is a network
    kAuto,  // only while the device's own AP is up
};

const char *Name(Desired desired);

// `network_wanted`: the radio is supposed to be up (lets the server close its
// socket before the netif goes: closing after is a null call). `stack_up`:
// lwIP exists (listen() without it is an assert). `ap`: our AP is up.
bool ShouldRun(Desired desired, bool network_wanted, bool stack_up, bool ap);

enum class Reconcile : uint8_t {
    kNothing = 0,
    kStart,
    kStop,
};

Reconcile Next(Desired desired, bool network_wanted, bool stack_up, bool ap, bool running);

}  // namespace web
