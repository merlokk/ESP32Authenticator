#pragma once

// What a FIDO2 / U2F request asks the user to allow (x4-design.md): read
// from the request before the core takes it, shown while the core waits for
// the button (Left or Home allow, Right refuses). CTAP2 requests are CBOR
// (a command byte, then a map with integer keys); U2F ones are APDUs (CLA 0).
// Only what the prompt shows is read; anything malformed is "a request".
// Pure (host-tested).

#include <cstddef>
#include <cstdint>

namespace ui::fido_prompt {

inline constexpr size_t kTextSize = 65;  // a stored string (64 bytes) + NUL

enum class Kind : uint8_t {
    kOther,        // a request (unknown or unread)
    kRegister,     // authenticatorMakeCredential: a new key
    kSignIn,       // authenticatorGetAssertion
    kU2fRegister,  // U2F_REGISTER
    kU2fSignIn,    // U2F_AUTHENTICATE
    kReset,        // authenticatorReset: every key erased (a 5 s hold)
    kSelect,       // authenticatorSelection: "is this the key?"
};

struct Request {
    Kind kind;
    char site[kTextSize];  // rp id, or "U2F 1a2b3c4d" (as the store keeps U2F keys)
    char user[kTextSize];  // the account's name
    char display[kTextSize];
    bool resident;  // makeCredential: a passkey (rk)
    bool verify;    // the PIN (pinUvAuthParam) or uv asked
    uint8_t allow;  // getAssertion: allowList entries (0: any key for the site)
    uint8_t accounts;  // sign-in: the store's keys for the site (the device fills it)
};

// --- a small CBOR reader: definite lengths, depth-limited; a fault stops it.

class Cbor {
   public:
    constexpr Cbor(const uint8_t *data, size_t size) : p_(data), end_(data + size) {}
    constexpr bool ok() const { return ok_; }

    // The next item's head: major type 0..7, its argument.
    constexpr bool Head(uint8_t *major, uint64_t *arg) {
        if (!ok_ || p_ >= end_) {
            return Fail();
        }
        const uint8_t b = *p_++;
        *major = static_cast<uint8_t>(b >> 5);
        const uint8_t ai = b & 0x1f;
        if (ai < 24) {
            *arg = ai;
            return true;
        }
        if (ai > 27) {
            return Fail();  // indefinite lengths: not read
        }
        const size_t n = static_cast<size_t>(1) << (ai - 24);
        if (static_cast<size_t>(end_ - p_) < n) {
            return Fail();
        }
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) {
            v = (v << 8) | *p_++;
        }
        *arg = v;
        return true;
    }

    // One whole item, whatever it is.
    constexpr bool Skip(int depth = 0) {
        uint8_t major = 0;
        uint64_t arg = 0;
        if (depth > 8 || !Head(&major, &arg)) {
            return Fail();
        }
        switch (major) {
            case 2:
            case 3:
                return Bytes(arg);
            case 4:
                for (uint64_t i = 0; i < arg; ++i) {
                    if (!Skip(depth + 1)) {
                        return false;
                    }
                }
                return true;
            case 5:
                for (uint64_t i = 0; i < arg * 2; ++i) {
                    if (!Skip(depth + 1)) {
                        return false;
                    }
                }
                return true;
            case 6:
                return Skip(depth + 1);  // a tag, then its item
            default:
                return true;  // ints, simple values: the head is all
        }
    }

    // A text string into `out` (cut at its size), or false (the item is spent).
    constexpr bool Text(char *out, size_t size) {
        uint8_t major = 0;
        uint64_t arg = 0;
        if (!Head(&major, &arg)) {
            return false;
        }
        if (major != 3) {
            return SkipRest(major, arg) && false;
        }
        if (static_cast<uint64_t>(end_ - p_) < arg) {
            return Fail();
        }
        size_t n = 0;
        for (uint64_t i = 0; i < arg; ++i) {
            const uint8_t c = p_[i];
            if (n + 1 < size) {
                out[n++] = c < 0x20 ? '?' : static_cast<char>(c);  // no control characters on the glass
            }
        }
        out[n] = '\0';
        p_ += arg;
        return true;
    }

    // A simple value true / false; other items are spent and read as false.
    constexpr bool Bool() {
        uint8_t major = 0;
        uint64_t arg = 0;
        if (!Head(&major, &arg)) {
            return false;
        }
        if (major == 7) {
            return arg == 21;
        }
        SkipRest(major, arg);
        return false;
    }

    // A map's or an array's count (and its major in `major`).
    constexpr bool Container(uint8_t want, uint64_t *count) {
        uint8_t major = 0;
        if (!Head(&major, count)) {
            return false;
        }
        if (major != want) {
            SkipRest(major, *count);
            return false;
        }
        return true;
    }

    // A text key equal to `key` (the key is spent either way).
    constexpr bool KeyIs(const char *key) {
        char k[16] = {};
        if (!Text(k, sizeof k)) {
            return false;
        }
        size_t i = 0;
        for (; key[i] != '\0' && key[i] == k[i]; ++i) {
        }
        return key[i] == '\0' && k[i] == '\0';
    }

    // An unsigned integer key (anything else: -1, spent).
    constexpr int64_t IntKey() {
        uint8_t major = 0;
        uint64_t arg = 0;
        if (!Head(&major, &arg)) {
            return -1;
        }
        if (major == 0) {
            return static_cast<int64_t>(arg);
        }
        SkipRest(major, arg);
        return -1;
    }

   private:
    constexpr bool Fail() {
        ok_ = false;
        return false;
    }
    constexpr bool Bytes(uint64_t n) {
        if (static_cast<uint64_t>(end_ - p_) < n) {
            return Fail();
        }
        p_ += n;
        return true;
    }
    // The rest of an item whose head is read.
    constexpr bool SkipRest(uint8_t major, uint64_t arg) {
        switch (major) {
            case 2:
            case 3:
                return Bytes(arg);
            case 4:
                for (uint64_t i = 0; i < arg; ++i) {
                    if (!Skip(1)) {
                        return false;
                    }
                }
                return true;
            case 5:
                for (uint64_t i = 0; i < arg * 2; ++i) {
                    if (!Skip(1)) {
                        return false;
                    }
                }
                return true;
            case 6:
                return Skip(1);
            default:
                return true;
        }
    }

    const uint8_t *p_;
    const uint8_t *end_;
    bool ok_ = true;
};

// The rp / user maps: the text members named `a` (and `b`) into their outputs.
constexpr void ReadNames(Cbor &c, const char *a, char *out_a, const char *b, char *out_b) {
    uint64_t n = 0;
    if (!c.Container(5, &n)) {
        return;
    }
    for (uint64_t i = 0; i < n && c.ok(); ++i) {
        char key[16] = {};
        if (!c.Text(key, sizeof key)) {
            c.Skip();
            continue;
        }
        size_t k = 0;
        for (; a[k] != '\0' && a[k] == key[k]; ++k) {
        }
        if (a[k] == '\0' && key[k] == '\0') {
            if (!c.Text(out_a, kTextSize)) {
                out_a[0] = '\0';
            }
            continue;
        }
        k = 0;
        if (b != nullptr) {
            for (; b[k] != '\0' && b[k] == key[k]; ++k) {
            }
        }
        if (b != nullptr && b[k] == '\0' && key[k] == '\0') {
            if (!c.Text(out_b, kTextSize)) {
                out_b[0] = '\0';
            }
            continue;
        }
        c.Skip();
    }
}

// An options map: rk and uv.
constexpr void ReadOptions(Cbor &c, Request *r) {
    uint64_t n = 0;
    if (!c.Container(5, &n)) {
        return;
    }
    for (uint64_t i = 0; i < n && c.ok(); ++i) {
        char key[8] = {};
        if (!c.Text(key, sizeof key)) {
            c.Skip();
            continue;
        }
        const bool v = c.Bool();
        if (key[0] == 'r' && key[1] == 'k' && key[2] == '\0') {
            r->resident = v;
        } else if (key[0] == 'u' && key[1] == 'v' && key[2] == '\0') {
            r->verify = r->verify || v;
        }
    }
}

constexpr char Hex(uint8_t v) { return static_cast<char>(v < 10 ? '0' + v : 'a' + v - 10); }

// "U2F " and the application's first 4 bytes in hex (the store's U2F rp id).
constexpr void U2fSite(const uint8_t *app, char *out) {
    const char *prefix = "U2F ";
    size_t n = 0;
    for (; prefix[n] != '\0'; ++n) {
        out[n] = prefix[n];
    }
    for (size_t i = 0; i < 4; ++i) {
        out[n++] = Hex(app[i] >> 4);
        out[n++] = Hex(app[i] & 0xf);
    }
    out[n] = '\0';
}

// A request (the command byte first) as the prompt shows it.
constexpr Request Parse(const uint8_t *msg, size_t len) {
    Request r = {};
    if (len == 0) {
        return r;
    }
    // U2F: an APDU, CLA 0; the data after a short (Lc) or an extended (0, Lc Lc) length.
    if (msg[0] == 0x00 && len >= 5) {
        const size_t data = msg[4] == 0 && len >= 7 ? 7 : 5;
        const uint8_t ins = msg[1];
        if ((ins == 0x01 || ins == 0x02) && len >= data + 64) {
            r.kind = ins == 0x01 ? Kind::kU2fRegister : Kind::kU2fSignIn;
            U2fSite(msg + data + 32, r.site);  // challenge, then the application
        }
        return r;
    }
    const uint8_t command = msg[0];
    if (command == 0x07) {
        r.kind = Kind::kReset;
        return r;
    }
    if (command == 0x0b) {
        r.kind = Kind::kSelect;
        return r;
    }
    if (command != 0x01 && command != 0x02) {
        return r;
    }
    r.kind = command == 0x01 ? Kind::kRegister : Kind::kSignIn;
    Cbor c(msg + 1, len - 1);
    uint64_t n = 0;
    if (!c.Container(5, &n)) {
        return r;
    }
    for (uint64_t i = 0; i < n && c.ok(); ++i) {
        const int64_t key = c.IntKey();
        if (command == 0x01 && key == 2) {
            char name[kTextSize] = {};
            ReadNames(c, "id", r.site, "name", name);
        } else if (command == 0x01 && key == 3) {
            ReadNames(c, "name", r.user, "displayName", r.display);
        } else if (command == 0x02 && key == 1) {
            if (!c.Text(r.site, kTextSize)) {
                r.site[0] = '\0';
            }
        } else if (command == 0x02 && key == 3) {
            uint64_t count = 0;
            if (c.Container(4, &count)) {
                r.allow = static_cast<uint8_t>(count > 255 ? 255 : count);
                for (uint64_t k = 0; k < count && c.ok(); ++k) {
                    c.Skip();
                }
            }
        } else if ((command == 0x01 && key == 7) || (command == 0x02 && key == 5)) {
            ReadOptions(c, &r);
        } else if ((command == 0x01 && key == 8) || (command == 0x02 && key == 6)) {
            r.verify = true;  // pinUvAuthParam: the PIN was given
            c.Skip();
        } else {
            c.Skip();
        }
    }
    return r;
}

constexpr const char *Title(Kind k) {
    switch (k) {
        case Kind::kRegister: return "Create a key";
        case Kind::kSignIn: return "Sign in";
        case Kind::kU2fRegister: return "Register (U2F)";
        case Kind::kU2fSignIn: return "Sign in (U2F)";
        case Kind::kReset: return "Reset FIDO2";
        case Kind::kSelect: return "Use this key?";
        case Kind::kOther: break;
    }
    return "A FIDO2 request";
}

}  // namespace ui::fido_prompt
