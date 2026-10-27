#pragma once

// The authentication server's side of the device (server.h): the state file,
// the registration token and the texts of Settings > Server. Pure
// (host-tested).
//
// Registration: a new ECDSA P-256 key and TOTP secret (the hardware RNG), then
// a JWT (ES256, RFC 7515/7519) signed by that key, carrying the e-mail typed
// on the device, the chip's serial number, its MAC, the TOTP secret (base32)
// and the public key; the device opens https://<host>/register?token=<JWT>.
// The server checks the signature and keeps the device id and its public
// key; the user signs in there with the TOTP code the device shows.

#include <cstddef>
#include <cstdint>

namespace server {

inline constexpr size_t kKeySize = 32;        // the P-256 private scalar
inline constexpr size_t kPublicSize = 65;     // 0x04 || X || Y
inline constexpr size_t kSignatureSize = 64;  // r || s (JWS ES256)
inline constexpr size_t kTotpSize = 20;       // 160 bits (RFC 4226)
inline constexpr size_t kSerialSize = 16;     // eFuse OPTIONAL_UNIQUE_ID

// What server.bin keeps (SPIFFS, plain, as seed.txt).
struct State {
    bool has_key;
    bool registered;  // the server took this key
    uint8_t key[kKeySize];
    uint8_t totp[kTotpSize];
    int64_t registered_at;  // Unix seconds, 0: never
    int64_t last_sync;      // the last answer from the server, 0: never
};

// "SRV1", flags (bit 0 a key, bit 1 registered), key, TOTP, the two times
// (little-endian).
inline constexpr size_t kFileSize = 4 + 1 + kKeySize + kTotpSize + 8 + 8;
inline constexpr uint8_t kMagic[4] = {'S', 'R', 'V', '1'};

constexpr void PutTime(int64_t v, uint8_t *out) {
    const auto u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>(u >> (8 * i));
    }
}

constexpr int64_t GetTime(const uint8_t *in) {
    uint64_t u = 0;
    for (int i = 7; i >= 0; --i) {
        u = u << 8 | in[i];
    }
    return static_cast<int64_t>(u);
}

constexpr void Encode(const State &s, uint8_t out[kFileSize]) {
    size_t n = 0;
    for (const uint8_t b : kMagic) {
        out[n++] = b;
    }
    out[n++] = static_cast<uint8_t>((s.has_key ? 1 : 0) | (s.registered ? 2 : 0));
    for (const uint8_t b : s.key) {
        out[n++] = b;
    }
    for (const uint8_t b : s.totp) {
        out[n++] = b;
    }
    PutTime(s.registered_at, out + n);
    PutTime(s.last_sync, out + n + 8);
}

// False: not a server.bin (`out` left as it is).
constexpr bool Decode(const uint8_t *in, size_t size, State *out) {
    if (size != kFileSize) {
        return false;
    }
    for (size_t i = 0; i < sizeof kMagic; ++i) {
        if (in[i] != kMagic[i]) {
            return false;
        }
    }
    const uint8_t flags = in[4];
    if ((flags & ~3) != 0 || ((flags & 2) != 0 && (flags & 1) == 0)) {
        return false;  // registered with no key
    }
    out->has_key = (flags & 1) != 0;
    out->registered = (flags & 2) != 0;
    size_t n = 5;
    for (uint8_t &b : out->key) {
        b = in[n++];
    }
    for (uint8_t &b : out->totp) {
        b = in[n++];
    }
    out->registered_at = GetTime(in + n);
    out->last_sync = GetTime(in + n + 8);
    return true;
}

// --- Encodings ---------------------------------------------------------------

// Base64url without padding (RFC 4648 5): its length for `n` bytes.
constexpr size_t Base64UrlLength(size_t n) { return (n * 4 + 2) / 3; }

// `n` bytes as base64url and a NUL; the length, 0 when `size` has no room.
constexpr size_t Base64Url(const uint8_t *in, size_t n, char *out, size_t size) {
    constexpr const char *kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const size_t length = Base64UrlLength(n);
    if (size <= length) {
        return 0;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = static_cast<uint32_t>(in[i]) << 16 |
                           (i + 1 < n ? static_cast<uint32_t>(in[i + 1]) << 8 : 0) |
                           (i + 2 < n ? in[i + 2] : 0);
        out[o++] = kAlphabet[v >> 18 & 63];
        out[o++] = kAlphabet[v >> 12 & 63];
        if (i + 1 < n) {
            out[o++] = kAlphabet[v >> 6 & 63];
        }
        if (i + 2 < n) {
            out[o++] = kAlphabet[v & 63];
        }
    }
    out[o] = '\0';
    return o;
}

// Base32 without padding (RFC 4648 6, what authenticator apps take): its
// length for `n` bytes.
constexpr size_t Base32Length(size_t n) { return (n * 8 + 4) / 5; }

constexpr size_t Base32(const uint8_t *in, size_t n, char *out, size_t size) {
    constexpr const char *kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    const size_t length = Base32Length(n);
    if (size <= length) {
        return 0;
    }
    uint32_t buffer = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; ++i) {
        buffer = buffer << 8 | in[i];
        bits += 8;
        while (bits >= 5) {
            out[o++] = kAlphabet[buffer >> (bits - 5) & 31];
            bits -= 5;
        }
    }
    if (bits > 0) {
        out[o++] = kAlphabet[buffer << (5 - bits) & 31];
    }
    out[o] = '\0';
    return o;
}

// `n` bytes as lowercase hex and a NUL (`size` > 2n).
constexpr void Hex(const uint8_t *in, size_t n, char *out) {
    constexpr const char *kDigits = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[2 * i] = kDigits[in[i] >> 4];
        out[2 * i + 1] = kDigits[in[i] & 15];
    }
    out[2 * n] = '\0';
}

// --- The token ---------------------------------------------------------------

// What the registration token says (texts as they go into the JSON).
struct Claims {
    const char *email;
    const char *serial;  // hex of the eFuse unique id
    const char *mac;     // "aa:bb:cc:dd:ee:ff", the base MAC
    const char *totp;    // the TOTP secret, base32
    const char *pub;     // the public key (kPublicSize bytes), base64url
    int64_t iat;         // Unix seconds
};

// The JOSE header (always the same).
inline constexpr const char kHeader[] = R"({"alg":"ES256","typ":"JWT"})";

// Up to 64 + 32 + 17 + 32 + 87 characters and the names: room for escapes.
inline constexpr size_t kPayloadSize = 512;
// header.payload.signature in base64url.
inline constexpr size_t kTokenSize = Base64UrlLength(sizeof kHeader - 1) + 1 +
                                     Base64UrlLength(kPayloadSize) + 1 +
                                     Base64UrlLength(kSignatureSize) + 1;

// Appends `text` to out[*n] as a JSON string ('"' and '\' escaped; the texts
// are printable ASCII). False when it does not fit.
constexpr bool PutString(const char *text, char *out, size_t size, size_t *n) {
    if (*n + 1 >= size) {
        return false;
    }
    out[(*n)++] = '"';
    for (; *text != '\0'; ++text) {
        if (*text == '"' || *text == '\\') {
            if (*n + 1 >= size) {
                return false;
            }
            out[(*n)++] = '\\';
        }
        if (*n + 1 >= size) {
            return false;
        }
        out[(*n)++] = *text;
    }
    if (*n + 1 >= size) {
        return false;
    }
    out[(*n)++] = '"';
    out[*n] = '\0';
    return true;
}

constexpr bool PutRaw(const char *text, char *out, size_t size, size_t *n) {
    for (; *text != '\0'; ++text) {
        if (*n + 1 >= size) {
            return false;
        }
        out[(*n)++] = *text;
    }
    out[*n] = '\0';
    return true;
}

constexpr bool PutNumber(int64_t v, char *out, size_t size, size_t *n) {
    char digits[21] = {};
    size_t k = 0;
    const bool negative = v < 0;
    uint64_t u = negative ? 0 - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
    do {
        digits[k++] = static_cast<char>('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (negative && !PutRaw("-", out, size, n)) {
        return false;
    }
    while (k > 0) {
        const char c[2] = {digits[--k], '\0'};
        if (!PutRaw(c, out, size, n)) {
            return false;
        }
    }
    return true;
}

// The JSON payload: {"email":..,"serial":..,"mac":..,"totp":..,"pub":..,"iat":..}.
// Its length, 0 when it does not fit.
constexpr size_t Payload(const Claims &c, char *out, size_t size) {
    size_t n = 0;
    const bool ok = PutRaw("{\"email\":", out, size, &n) && PutString(c.email, out, size, &n) &&
                    PutRaw(",\"serial\":", out, size, &n) && PutString(c.serial, out, size, &n) &&
                    PutRaw(",\"mac\":", out, size, &n) && PutString(c.mac, out, size, &n) &&
                    PutRaw(",\"totp\":", out, size, &n) && PutString(c.totp, out, size, &n) &&
                    PutRaw(",\"pub\":", out, size, &n) && PutString(c.pub, out, size, &n) &&
                    PutRaw(",\"iat\":", out, size, &n) && PutNumber(c.iat, out, size, &n) &&
                    PutRaw("}", out, size, &n);
    return ok ? n : 0;
}

// What the key signs: base64url(header) "." base64url(payload). Its length,
// 0 when it does not fit.
constexpr size_t SigningInput(const char *payload, size_t payload_len, char *out, size_t size) {
    const size_t h = Base64Url(reinterpret_cast<const uint8_t *>(kHeader), sizeof kHeader - 1,
                               out, size);
    if (h == 0 || h + 1 >= size) {
        return 0;
    }
    out[h] = '.';
    const size_t p = Base64Url(reinterpret_cast<const uint8_t *>(payload), payload_len,
                               out + h + 1, size - h - 1);
    return p == 0 ? 0 : h + 1 + p;
}

// "." base64url(signature) after the signing input (`length` of it in
// `token`): the whole token's length, 0 when it does not fit.
constexpr size_t AppendSignature(char *token, size_t length, size_t size,
                                 const uint8_t signature[kSignatureSize]) {
    if (length + 1 >= size) {
        return 0;
    }
    token[length] = '.';
    const size_t s = Base64Url(signature, kSignatureSize, token + length + 1, size - length - 1);
    return s == 0 ? 0 : length + 1 + s;
}

// "https://<host>/register?token=<token>" (base64url needs no escaping).
// False when it does not fit.
constexpr bool RegisterUrl(const char *host, const char *token, char *out, size_t size) {
    size_t n = 0;
    return host[0] != '\0' && PutRaw("https://", out, size, &n) && PutRaw(host, out, size, &n) &&
           PutRaw("/register?token=", out, size, &n) && PutRaw(token, out, size, &n);
}

// --- What Settings > Server says ----------------------------------------------

// The last request to the server.
enum class Link : uint8_t {
    kNone,     // none since the boot
    kOk,       // it answered over HTTPS
    kFailed,   // no answer (no network, DNS, TLS, a timeout)
};

// How long ago: "never", "just now", "5 min ago", "3 h ago", "2 days ago".
// `then` 0 is never; a clock not set (now before then) says "just now".
constexpr void Ago(int64_t now, int64_t then, char *out, size_t size) {
    if (size == 0) {
        return;
    }
    size_t n = 0;
    out[0] = '\0';
    if (then <= 0) {
        PutRaw("never", out, size, &n);
        return;
    }
    const int64_t d = now - then;
    if (d < 60) {
        PutRaw("just now", out, size, &n);
        return;
    }
    const int64_t v = d < 3600 ? d / 60 : d < 2 * 86400 ? d / 3600 : d / 86400;
    const char *unit = d < 3600 ? " min ago" : d < 2 * 86400 ? " h ago" : " days ago";
    if (PutNumber(v, out, size, &n)) {
        PutRaw(unit, out, size, &n);
    }
}

// A 6-digit code as "123 456".
constexpr void CodeText(uint32_t code, char out[8]) {
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

// The summary on Settings' main page.
constexpr const char *Summary(bool registered, Link link) {
    return !registered             ? "not registered"
           : link == Link::kFailed ? "registered, offline"
                                   : "registered";
}

}  // namespace server
