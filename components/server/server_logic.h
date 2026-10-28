#pragma once

// The authentication server's side of the device (server.h): the state file,
// the registration token and the texts of Settings > Server. Pure
// (host-tested).
//
// Registration: a new ECDSA P-256 key and TOTP secret (the hardware RNG), then
// a JWT (ES256, RFC 7515/7519) signed by that key, carrying the e-mail typed
// on the device, the chip's serial number, its MAC, the TOTP secret (base32)
// and the public key; the device POSTs it to https://<host>/register
// (Content-Type: application/jwt, the token as the body).
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
inline constexpr size_t kClientIdSize = 36;   // a UUID, "8-4-4-4-12"

// What server.bin keeps (SPIFFS, plain, as seed.txt).
struct State {
    bool has_key;
    bool registered;  // the server took this key
    uint8_t key[kKeySize];
    uint8_t totp[kTotpSize];
    int64_t registered_at;  // Unix seconds, 0: never
    int64_t last_sync;      // the last status the server took, 0: never
    char client_id[kClientIdSize + 1];  // the registration's answer; empty: none
};

// "SRV2", flags (bit 0 a key, bit 1 registered), key, TOTP, the two times
// (little-endian), the client id (kClientIdSize, NUL-padded). "SRV1" (before
// the client id) is still read: registered, but signing in needs a new
// registration.
inline constexpr size_t kFileSizeV1 = 4 + 1 + kKeySize + kTotpSize + 8 + 8;
inline constexpr size_t kFileSize = kFileSizeV1 + kClientIdSize;
inline constexpr uint8_t kMagic[4] = {'S', 'R', 'V', '2'};

// A client id as the server gives it: 36 of 0-9 a-f A-F, '-' at 8, 13, 18, 23.
constexpr bool ValidClientId(const char *id, size_t len) {
    if (len != kClientIdSize) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        const char c = id[i];
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (dash ? c != '-' : !hex) {
            return false;
        }
    }
    return true;
}

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
    n += 16;
    bool end = false;
    for (size_t i = 0; i < kClientIdSize; ++i) {
        end = end || s.client_id[i] == '\0';
        out[n++] = end ? 0 : static_cast<uint8_t>(s.client_id[i]);
    }
}

// False: not a server.bin (`out` left as it is).
constexpr bool Decode(const uint8_t *in, size_t size, State *out) {
    if (size != kFileSize && size != kFileSizeV1) {
        return false;
    }
    for (size_t i = 0; i < 3; ++i) {
        if (in[i] != kMagic[i]) {
            return false;
        }
    }
    if (in[3] != (size == kFileSize ? '2' : '1')) {
        return false;
    }
    char id[kClientIdSize + 1] = {};
    if (size == kFileSize) {
        size_t len = 0;
        while (len < kClientIdSize && in[kFileSizeV1 + len] != 0) {
            id[len] = static_cast<char>(in[kFileSizeV1 + len]);
            ++len;
        }
        for (size_t i = len; i < kClientIdSize; ++i) {
            if (in[kFileSizeV1 + i] != 0) {
                return false;  // padding that is not
            }
        }
        if (len != 0 && !ValidClientId(id, len)) {
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
    for (size_t i = 0; i <= kClientIdSize; ++i) {
        out->client_id[i] = id[i];
    }
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
constexpr size_t SigningInput(const char *header, size_t header_len, const char *payload,
                              size_t payload_len, char *out, size_t size) {
    const size_t h = Base64Url(reinterpret_cast<const uint8_t *>(header), header_len, out, size);
    if (h == 0 || h + 1 >= size) {
        return 0;
    }
    out[h] = '.';
    const size_t p = Base64Url(reinterpret_cast<const uint8_t *>(payload), payload_len,
                               out + h + 1, size - h - 1);
    return p == 0 ? 0 : h + 1 + p;
}

// The registration token's (kHeader).
constexpr size_t SigningInput(const char *payload, size_t payload_len, char *out, size_t size) {
    return SigningInput(kHeader, sizeof kHeader - 1, payload, payload_len, out, size);
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

// "https://<host>/register": the token goes in the POST's body (never in the
// URL: servers log those). False when it does not fit.
constexpr bool RegisterUrl(const char *host, char *out, size_t size) {
    size_t n = 0;
    return host[0] != '\0' && PutRaw("https://", out, size, &n) && PutRaw(host, out, size, &n) &&
           PutRaw("/register", out, size, &n);
}

// --- Ping --------------------------------------------------------------------

// "https://<host>/api/device/ping" (device-protocol.md: anonymous, never
// cached). False when it does not fit.
constexpr bool PingUrl(const char *host, char *out, size_t size) {
    size_t n = 0;
    return host[0] != '\0' && PutRaw("https://", out, size, &n) && PutRaw(host, out, size, &n) &&
           PutRaw("/api/device/ping", out, size, &n);
}

// The answer's body is read up to this (the server sends about 40 bytes).
inline constexpr size_t kPingBodySize = 128;

constexpr const char *SkipSpace(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        ++p;
    }
    return p;
}

// The value after `"name":` in a flat JSON object, or nullptr. Good enough
// for the ping's {"status":"ok","time":N}: no nesting, names not in values.
constexpr const char *FindValue(const char *body, size_t len, const char *name) {
    const char *end = body + len;
    for (const char *p = body; p < end; ++p) {
        if (*p != '"') {
            continue;
        }
        const char *q = p + 1;
        const char *s = name;
        while (q < end && *s != '\0' && *q == *s) {
            ++q;
            ++s;
        }
        if (*s != '\0' || q >= end || *q != '"') {
            continue;
        }
        q = SkipSpace(q + 1, end);
        if (q < end && *q == ':') {
            return SkipSpace(q + 1, end);
        }
    }
    return nullptr;
}

// The non-negative integer at `p` (up to `end`). False: none, too big, or cut
// by the end (a body over its buffer).
constexpr bool ReadNumber(const char *p, const char *end, int64_t *out) {
    if (p == nullptr || p >= end || *p < '0' || *p > '9') {
        return false;
    }
    int64_t v = 0;
    for (; p < end && *p >= '0' && *p <= '9'; ++p) {
        if (v > (INT64_MAX - 9) / 10) {
            return false;
        }
        v = v * 10 + (*p - '0');
    }
    if (p >= end) {
        return false;
    }
    *out = v;
    return true;
}

// The number `"name":N` in `body`. False: none (`out` left as it is).
constexpr bool GetNumber(const char *body, size_t len, const char *name, int64_t *out) {
    return ReadNumber(FindValue(body, len, name), body + len, out);
}

// The string `"name":"..."` in `body` into `out` (no escapes: the server's
// ids, codes and versions have none). Its length; -1: none, not a string,
// an escape, or longer than `size` - 1.
constexpr int GetString(const char *body, size_t len, const char *name, char *out, size_t size) {
    const char *end = body + len;
    const char *p = FindValue(body, len, name);
    if (p == nullptr || p >= end || *p != '"' || size == 0) {
        return -1;
    }
    size_t n = 0;
    for (++p; p < end && *p != '"'; ++p) {
        if (*p == '\\' || n + 1 >= size) {
            return -1;
        }
        out[n++] = *p;
    }
    if (p >= end) {
        return -1;
    }
    out[n] = '\0';
    return static_cast<int>(n);
}

// A 200 ping's body: {"status":"ok","time":<Unix seconds>}. False: anything
// else ("no server").
constexpr bool ParsePing(const char *body, size_t len, int64_t *server_time) {
    char status[4] = "";
    return GetString(body, len, "status", status, sizeof status) == 2 && status[0] == 'o' &&
           status[1] == 'k' && GetNumber(body, len, "time", server_time);
}

// --- Answers -----------------------------------------------------------------

// A refusal's code: Problem Details' "type" without
// "urn:authserver:problem:", or an OAuth "error" (/connect/token). False:
// neither.
constexpr bool ErrorCode(const char *body, size_t len, char *out, size_t size) {
    constexpr const char kPrefix[] = "urn:authserver:problem:";
    char type[96] = "";
    const int n = GetString(body, len, "type", type, sizeof type);
    if (n > 0) {
        size_t i = 0;
        while (kPrefix[i] != '\0' && type[i] == kPrefix[i]) {
            ++i;
        }
        const char *code = kPrefix[i] == '\0' ? type + i : type;
        size_t o = 0;
        if (PutRaw(code, out, size, &o)) {
            return true;
        }
    }
    return GetString(body, len, "error", out, size) > 0;
}

// What Settings > Server says for a refusal: `status` and its code (empty:
// none came).
constexpr void RefusalText(int status, const char *code, char *out, size_t size) {
    struct Known {
        const char *code;
        const char *text;
    };
    constexpr Known kKnown[] = {
        {"device-registration.refused", "refused: no open window"},
        {"device-registration.invalid", "refused: a malformed token"},
        {"device-registration.stale", "refused: the clock is off"},
        {"device.taken", "on another account"},
        {"device-status.invalid", "status refused"},
        {"invalid_client", "sign-in refused: register again"},
        {"invalid_grant", "sign-in refused"},
    };
    size_t n = 0;
    out[0] = '\0';
    for (const Known &k : kKnown) {
        size_t i = 0;
        while (k.code[i] != '\0' && k.code[i] == code[i]) {
            ++i;
        }
        if (k.code[i] == '\0' && code[i] == '\0') {
            PutRaw(k.text, out, size, &n);
            return;
        }
    }
    if (status == 502) {
        PutRaw("the server's backend is down", out, size, &n);
        return;
    }
    PutRaw("the server said ", out, size, &n);
    PutNumber(status, out, size, &n);
    if (code[0] != '\0') {
        PutRaw(": ", out, size, &n);
        PutRaw(code, out, size, &n);
    }
}

// --- Sign-in (POST /connect/token) -------------------------------------------

inline constexpr const char kAssertionHeader[] =
    R"({"alg":"ES256","typ":"client-authentication+jwt"})";
inline constexpr int64_t kAssertionSeconds = 300;  // exp - iat

// The client assertion's claims: iss = sub = the client id, aud
// "https://<host>/", jti (hex), iat, exp. Its length, 0 when it does not fit.
constexpr size_t AssertionPayload(const char *client_id, const char *host, const char *jti,
                                  int64_t iat, char *out, size_t size) {
    size_t n = 0;
    const bool ok = PutRaw("{\"iss\":", out, size, &n) && PutString(client_id, out, size, &n) &&
                    PutRaw(",\"sub\":", out, size, &n) && PutString(client_id, out, size, &n) &&
                    PutRaw(",\"aud\":\"https://", out, size, &n) && PutRaw(host, out, size, &n) &&
                    PutRaw("/\",\"jti\":", out, size, &n) && PutString(jti, out, size, &n) &&
                    PutRaw(",\"iat\":", out, size, &n) && PutNumber(iat, out, size, &n) &&
                    PutRaw(",\"exp\":", out, size, &n) &&
                    PutNumber(iat + kAssertionSeconds, out, size, &n) &&
                    PutRaw("}", out, size, &n);
    return ok ? n : 0;
}

// The form: grant_type, client_id, client_assertion_type, client_assertion
// (the id and the JWT need no escapes). Its length, 0 when it does not fit.
constexpr size_t TokenForm(const char *client_id, const char *assertion, char *out, size_t size) {
    size_t n = 0;
    const bool ok =
        PutRaw("grant_type=client_credentials&client_id=", out, size, &n) &&
        PutRaw(client_id, out, size, &n) &&
        PutRaw("&client_assertion_type=urn%3Aietf%3Aparams%3Aoauth%3Aclient-assertion-type%3A"
               "jwt-bearer&client_assertion=",
               out, size, &n) &&
        PutRaw(assertion, out, size, &n);
    return ok ? n : 0;
}

// "https://<host><path>". False when it does not fit.
constexpr bool Url(const char *host, const char *path, char *out, size_t size) {
    size_t n = 0;
    return host[0] != '\0' && PutRaw("https://", out, size, &n) && PutRaw(host, out, size, &n) &&
           PutRaw(path, out, size, &n);
}

// The access token's answer: {"access_token":..,"token_type":"Bearer",
// "expires_in":N}. False: not one, or a token over `size` - 1.
constexpr bool ParseToken(const char *body, size_t len, char *token, size_t size,
                          int64_t *expires_in) {
    char type[8] = "";
    const int t = GetString(body, len, "token_type", type, sizeof type);
    const bool bearer = t == 6 && (type[0] == 'B' || type[0] == 'b') && type[1] == 'e' &&
                        type[2] == 'a' && type[3] == 'r' && type[4] == 'e' && type[5] == 'r';
    return bearer && GetString(body, len, "access_token", token, size) > 0 &&
           GetNumber(body, len, "expires_in", expires_in);
}

// Sign in again this long before the token runs out.
inline constexpr int64_t kTokenMargin = 30;

// --- Status (POST /api/device/status) ----------------------------------------

// The device's state (device-protocol.md: Status).
struct Report {
    const char *firmware;  // esp_app_desc_t.version
    const char *board;     // "x4pro", "dev"
    const char *mac;
    bool wifi;
    bool ble;
    int battery;      // percent; -1: none (left out)
    int64_t uptime;   // seconds since the boot
    int64_t time;     // the clock; 0: not set
};

inline constexpr size_t kStatusBodySize = 256;

// The JSON body. Its length, 0 when it does not fit.
constexpr size_t StatusBody(const Report &r, char *out, size_t size) {
    size_t n = 0;
    bool ok = PutRaw("{\"firmware\":", out, size, &n) && PutString(r.firmware, out, size, &n) &&
              PutRaw(",\"board\":", out, size, &n) && PutString(r.board, out, size, &n) &&
              PutRaw(",\"mac\":", out, size, &n) && PutString(r.mac, out, size, &n) &&
              PutRaw(",\"wifi\":", out, size, &n) && PutRaw(r.wifi ? "true" : "false", out, size, &n) &&
              PutRaw(",\"ble\":", out, size, &n) && PutRaw(r.ble ? "true" : "false", out, size, &n);
    if (ok && r.battery >= 0) {
        ok = PutRaw(",\"battery\":", out, size, &n) && PutNumber(r.battery, out, size, &n);
    }
    ok = ok && PutRaw(",\"uptime\":", out, size, &n) && PutNumber(r.uptime, out, size, &n) &&
         PutRaw(",\"time\":", out, size, &n) && PutNumber(r.time, out, size, &n) &&
         PutRaw("}", out, size, &n);
    return ok ? n : 0;
}

inline constexpr size_t kVersionSize = 33;  // 1..32 characters

// The answer: {"update": null | {"id", "version", ...}, "time": N}. `update`:
// the release's version, empty for none. False: not that.
constexpr bool ParseStatus(const char *body, size_t len, char *update, size_t size,
                           int64_t *server_time) {
    const char *u = FindValue(body, len, "update");
    if (u == nullptr || u >= body + len || !GetNumber(body, len, "time", server_time)) {
        return false;
    }
    update[0] = '\0';
    if (*u == 'n') {
        return true;  // null
    }
    return *u == '{' && GetString(body, len, "version", update, size) > 0;
}

// --- When the status goes ------------------------------------------------------

// A failed status is tried again this soon (not the whole interval).
inline constexpr int64_t kStatusRetrySeconds = 60;

// Is a status due? `ready`: registered, Wi-Fi online, the clock set. `now`,
// `tried`, `sent`: monotonic seconds (since the boot), `tried` the last
// attempt (-1: none), `sent` the last one the server took (-1: none since
// the boot). The first goes as soon as the device is ready (at the boot, or
// when Wi-Fi first connects), then every `minutes`.
constexpr bool StatusDue(bool ready, uint8_t minutes, int64_t now, int64_t tried, int64_t sent) {
    if (!ready || minutes == 0) {
        return false;
    }
    const int64_t interval = static_cast<int64_t>(minutes) * 60;
    const int64_t retry = interval < kStatusRetrySeconds ? interval : kStatusRetrySeconds;
    if (tried >= 0 && now - tried < (tried == sent ? interval : retry)) {
        return false;
    }
    return true;
}

// The registration takes an `iat` within this of the server's clock.
inline constexpr int64_t kIatWindow = 10 * 60;

// Would the server refuse the registration's `iat` (device-registration.stale)?
// `offset`: the device's clock minus the server's, from a ping.
constexpr bool ClockTooFar(int64_t offset) { return offset > kIatWindow || offset < -kIatWindow; }

// How far the device's clock is off the server's, for the TOTP code the
// server checks (30 s steps): from kClockBad a code may fall in the wrong
// step, from kClockCritical it does.
inline constexpr int64_t kClockBad = 15;
inline constexpr int64_t kClockCritical = 60;

enum class ClockLevel : uint8_t { kOk, kBad, kCritical };

constexpr ClockLevel ClockLevelOf(int64_t offset) {
    const int64_t d = offset < 0 ? -offset : offset;
    return d >= kClockCritical ? ClockLevel::kCritical
           : d >= kClockBad    ? ClockLevel::kBad
                               : ClockLevel::kOk;
}

// The device's clock against the server's (`offset` = device - server),
// always with the seconds (the server's time is whole seconds: +-1 s is the
// resolution): "same (+1 s)" within 2 s, "+12 s ahead", "-20 s behind, bad",
// "+75 s ahead, critical"; from 2 min the sign says which way:
// "-185 s (3 min), critical", "+432000 s (5 days), critical".
constexpr void OffsetText(int64_t offset, char *out, size_t size) {
    if (size == 0) {
        return;
    }
    size_t n = 0;
    out[0] = '\0';
    const int64_t d = offset < 0 ? -offset : offset;
    const char *sign = offset < 0 ? "-" : "+";
    if (d <= 2) {
        if (PutRaw("same (", out, size, &n) && PutRaw(sign, out, size, &n) &&
            PutNumber(d, out, size, &n)) {
            PutRaw(" s)", out, size, &n);
        }
        return;
    }
    bool ok = PutRaw(sign, out, size, &n) && PutNumber(d, out, size, &n) &&
              PutRaw(" s", out, size, &n);
    if (d < 120) {
        ok = ok && PutRaw(offset > 0 ? " ahead" : " behind", out, size, &n);
    } else {
        const int64_t v = d < 2 * 3600 ? d / 60 : d < 2 * 86400 ? d / 3600 : d / 86400;
        const char *unit = d < 2 * 3600 ? " min)" : d < 2 * 86400 ? " h)" : " days)";
        ok = ok && PutRaw(" (", out, size, &n) && PutNumber(v, out, size, &n) &&
             PutRaw(unit, out, size, &n);
    }
    const ClockLevel level = ClockLevelOf(offset);
    if (ok && level != ClockLevel::kOk) {
        PutRaw(level == ClockLevel::kBad ? ", bad" : ", critical", out, size, &n);
    }
}

// --- What Settings > Server says ----------------------------------------------

// The last request to the server.
enum class Link : uint8_t {
    kNone,     // none since the boot
    kOk,       // GET /api/device/ping: 200 {"status":"ok"}
    kFailed,   // no answer (no network, DNS, TLS, a timeout) or not the server's
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
