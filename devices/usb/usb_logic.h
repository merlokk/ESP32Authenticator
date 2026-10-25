#pragma once

// The pure half of the USB port (host-tested): the modes, the product IDs,
// and the console's line endings. The controllers, the PHY hand-over and the
// stdio device are usb.h.

#include <cstddef>
#include <cstdint>

namespace usb {

// What the port is (config.json usb.mode).
enum class Mode : uint8_t {
    kJtag,        // the chip's USB Serial/JTAG: the console, flashing, JTAG
    kSerial,      // TinyUSB: a CDC-ACM serial port with the console
    kSerialFido,  // TinyUSB: the serial port and a FIDO2 / U2F key (CTAPHID)
};

constexpr const char *Name(Mode m) {
    return m == Mode::kSerial       ? "serial"
           : m == Mode::kSerialFido ? "serial+fido"
                                    : "jtag";
}

// On the USB OTG controller (TinyUSB), not the Serial/JTAG.
constexpr bool OnOtg(Mode m) { return m != Mode::kJtag; }

constexpr bool HasFido(Mode m) { return m == Mode::kSerialFido; }

// Espressif's VID with TinyUSB's PID map (0x4000 | CDC 1 | MSC 2 | HID 4): a
// product ID per interface set, so a host never applies one layout's cached
// drivers to another.
constexpr uint16_t kVendorId = 0x303A;
constexpr uint16_t ProductId(Mode m) { return HasFido(m) ? 0x4005 : 0x4001; }

// Console output: '\n' goes out as "\r\n" (terminals move the caret back on
// CR only). Converts from `in` (`n` bytes) as much as fits `out` (`size`
// bytes, a pair never split); returns the bytes written, `*used` the bytes
// of `in` taken.
constexpr size_t ExpandNewlines(const char *in, size_t n, char *out, size_t size, size_t *used) {
    size_t i = 0;
    size_t o = 0;
    for (; i < n; ++i) {
        if (in[i] == '\n') {
            if (o + 2 > size) {
                break;
            }
            out[o++] = '\r';
            out[o++] = '\n';
        } else {
            if (o + 1 > size) {
                break;
            }
            out[o++] = in[i];
        }
    }
    *used = i;
    return o;
}

// Console input: terminals send CR for Enter; the command line wants LF.
constexpr void CrToLf(char *buf, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == '\r') {
            buf[i] = '\n';
        }
    }
}

}  // namespace usb
