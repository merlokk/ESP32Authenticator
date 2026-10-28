#pragma once

// The server's firmware releases (device-protocol.md "Firmware" in
// ESP32AuthenticatorServer): the list for this board, which is newer than the
// running one, and the image's download in pieces. Pure (host-tested).
//
// GET /api/device/firmware?board=<board>:
//   {"releases":[{"id","board","version","built","size","sha256","active",...}]}
// GET /api/device/firmware/{id}/image with Range: bytes=a-b: 206, the piece;
// the SHA-256 of the whole image is the release's `sha256`.

#include <cstddef>
#include <cstdint>

#include "server_logic.h"
#include "sync_logic.h"

namespace server::firmware {

inline constexpr size_t kIdSize = kClientIdSize + 1;  // a UUID, as the client id
inline constexpr size_t kBuiltSize = 24;               // "Oct 27 2026 21:07:30"
inline constexpr size_t kShaSize = 65;                 // lower-case hex
inline constexpr size_t kMaxReleases = 8;              // the newest kept
inline constexpr size_t kPieceSize = 64 * 1024;        // one Range request
inline constexpr size_t kListBodySize = 4096;          // the list's answer, read up to this

struct Release {
    char id[kIdSize];
    char version[kVersionSize];
    char built[kBuiltSize];
    uint32_t size;
    char sha256[kShaSize];
    bool active;  // the one the status answer offers
};

// esp_app_desc_t.version: "x.y.z", "x.y.z-nogit" or "x.y.z-<commits>-g<hash>[-dirty]".
struct Version {
    bool ok;
    uint32_t major, minor, patch, commits;
};

constexpr bool ReadPart(const char *&p, uint32_t *out) {
    if (*p < '0' || *p > '9') {
        return false;
    }
    uint32_t v = 0;
    for (; *p >= '0' && *p <= '9'; ++p) {
        if (v > 99999999) {
            return false;
        }
        v = v * 10 + static_cast<uint32_t>(*p - '0');
    }
    *out = v;
    return true;
}

constexpr Version ParseVersion(const char *text) {
    Version v = {};
    const char *p = text;
    if (!ReadPart(p, &v.major) || *p++ != '.' || !ReadPart(p, &v.minor) || *p++ != '.' ||
        !ReadPart(p, &v.patch)) {
        return {};
    }
    // "-<commits>-g...": the commits since the tag; anything else (nogit,
    // nothing) is the tag itself.
    if (*p == '-' && p[1] >= '0' && p[1] <= '9') {
        ++p;
        if (!ReadPart(p, &v.commits) || *p != '-') {
            return {};
        }
    }
    v.ok = true;
    return v;
}

// <0, 0, >0 as `a` is older, the same, newer than `b`. Not a version is
// older than any; two of those are the same. The hash orders nothing.
constexpr int Compare(const char *a, const char *b) {
    const Version x = ParseVersion(a);
    const Version y = ParseVersion(b);
    if (!x.ok || !y.ok) {
        return (x.ok ? 1 : 0) - (y.ok ? 1 : 0);
    }
    const uint32_t xs[] = {x.major, x.minor, x.patch, x.commits};
    const uint32_t ys[] = {y.major, y.minor, y.patch, y.commits};
    for (size_t i = 0; i < 4; ++i) {
        if (xs[i] != ys[i]) {
            return xs[i] > ys[i] ? 1 : -1;
        }
    }
    return 0;
}

constexpr bool Newer(const char *candidate, const char *running) {
    return Compare(candidate, running) > 0;
}

// Newest first; the same version keeps the server's order (newest upload first).
constexpr void SortNewestFirst(Release *r, size_t n) {
    for (size_t i = 1; i < n; ++i) {
        for (size_t j = i; j > 0 && Compare(r[j].version, r[j - 1].version) > 0; --j) {
            Release t = r[j];
            r[j] = r[j - 1];
            r[j - 1] = t;
        }
    }
}

constexpr bool AnyNewer(const Release *r, size_t n, const char *running) {
    for (size_t i = 0; i < n; ++i) {
        if (Newer(r[i].version, running)) {
            return true;
        }
    }
    return false;
}

constexpr bool IsHex(const char *s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return s[n] == '\0';
}

// The "true" / "false" after `"name":`; false when not there.
constexpr bool GetBool(const char *body, size_t len, const char *name) {
    const char *p = FindValue(body, len, name);
    return p != nullptr && p + 4 <= body + len && p[0] == 't' && p[1] == 'r' && p[2] == 'u' &&
           p[3] == 'e';
}

// The list's releases into `out` (up to kMaxReleases: the newest), newest
// first; their count. -1: not the list. Malformed entries are left out.
constexpr int ParseReleases(const char *body, size_t len, Release *out) {
    if (FindValue(body, len, "releases") == nullptr) {
        return -1;
    }
    size_t n = 0;
    const char *at = nullptr;
    const char *obj = nullptr;
    size_t obj_len = 0;
    while (sync::NextObject(body, len, "releases", &at, &obj, &obj_len)) {
        Release r = {};
        int64_t size = 0;
        const int id_len = GetString(obj, obj_len, "id", r.id, sizeof r.id);
        if (id_len < 0 || !ValidClientId(r.id, static_cast<size_t>(id_len)) ||
            GetString(obj, obj_len, "version", r.version, sizeof r.version) <= 0 ||
            GetString(obj, obj_len, "sha256", r.sha256, sizeof r.sha256) != 64 ||
            !IsHex(r.sha256, 64) || !GetNumber(obj, obj_len, "size", &size) || size <= 0 ||
            size > 0x7fffffff) {
            continue;
        }
        if (GetString(obj, obj_len, "built", r.built, sizeof r.built) < 0) {
            r.built[0] = '\0';
        }
        r.size = static_cast<uint32_t>(size);
        r.active = GetBool(obj, obj_len, "active");
        if (n < kMaxReleases) {
            out[n++] = r;
            SortNewestFirst(out, n);
        } else if (Compare(r.version, out[n - 1].version) > 0) {
            out[n - 1] = r;  // the oldest kept makes room
            SortNewestFirst(out, n);
        }
    }
    return static_cast<int>(n);
}

// https://<host>/api/device/firmware?board=<board>
constexpr bool ListUrl(const char *host, const char *board, char *out, size_t size) {
    size_t n = 0;
    return host[0] != '\0' && board[0] != '\0' && PutRaw("https://", out, size, &n) &&
           PutRaw(host, out, size, &n) && PutRaw("/api/device/firmware?board=", out, size, &n) &&
           PutRaw(board, out, size, &n);
}

// https://<host>/api/device/firmware/<id>/image
constexpr bool ImageUrl(const char *host, const char *id, char *out, size_t size) {
    size_t n = 0;
    return host[0] != '\0' && ValidClientId(id, kClientIdSize) &&
           PutRaw("https://", out, size, &n) && PutRaw(host, out, size, &n) &&
           PutRaw("/api/device/firmware/", out, size, &n) && PutRaw(id, out, size, &n) &&
           PutRaw("/image", out, size, &n);
}

// The next piece from `offset` of an image of `total` bytes: "bytes=a-b".
constexpr bool RangeHeader(uint32_t offset, uint32_t total, char *out, size_t size) {
    if (offset >= total) {
        return false;
    }
    const uint32_t last =
        total - offset > kPieceSize ? offset + static_cast<uint32_t>(kPieceSize) - 1 : total - 1;
    size_t n = 0;
    return PutRaw("bytes=", out, size, &n) && PutNumber(offset, out, size, &n) &&
           PutRaw("-", out, size, &n) && PutNumber(last, out, size, &n);
}

// "bytes a-b/total": its first byte. False: another shape.
constexpr bool RangeStart(const char *text, uint32_t *first) {
    const char *p = text;
    for (const char *s = "bytes "; *s != '\0'; ++s, ++p) {
        if (*p != *s) {
            return false;
        }
    }
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t total = 0;
    if (!ReadPart(p, &a) || *p++ != '-' || !ReadPart(p, &b) || *p++ != '/' ||
        !ReadPart(p, &total) || *p != '\0' || b < a || b >= total) {
        return false;
    }
    *first = a;
    return true;
}

}  // namespace server::firmware
