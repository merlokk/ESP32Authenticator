#pragma once

// The stores' sync with the server (device-protocol.md: Stores): what to do
// with each store, the requests' bodies and URLs, the answers. Pure
// (host-tested); server.cpp carries it out.
//
// Each store (vault.bin, fido.bin) is compared by its header: the store id,
// the version and the nonce (one write). NVS keeps the copy both sides had at
// the last sync, so a change on either side shows: only ours changed:
// upload; only the server's: download; both: a conflict, resolved by hand
// (Settings > Server > Sync), the automatic sync stopped until then. A store
// never synced looks at the account's stores under its seed phrase first: none:
// upload; one and ours empty: take it; else a conflict too.

#include <cstddef>
#include <cstdint>

#include "server_logic.h"

namespace server::sync {

inline constexpr size_t kStoreIdSize = 8;  // store_header.h
inline constexpr size_t kSeedIdSize = 4;
inline constexpr size_t kNonceSize = 12;
inline constexpr size_t kKeyIdSize = 8;
inline constexpr size_t kMaxFileBytes = 256 * 1024;  // the server's Sync:MaxFileBytes

enum class Store : uint8_t { kVault, kFido };
inline constexpr size_t kStores = 2;
inline constexpr Store kAll[kStores] = {Store::kVault, Store::kFido};

// store_header.h's kind: 2 vault.bin, 1 fido.bin.
constexpr uint8_t KindOf(Store s) { return s == Store::kVault ? 2 : 1; }
constexpr const char *StoreName(Store s) { return s == Store::kVault ? "passwords" : "passkeys"; }

constexpr bool SameBytes(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

// One write of a store.
struct Copy {
    bool has = false;
    uint8_t store_id[kStoreIdSize] = {};
    uint32_t version = 0;
    uint8_t nonce[kNonceSize] = {};
};

constexpr bool SameWrite(const Copy &a, const Copy &b) {
    return a.has && b.has && a.version == b.version &&
           SameBytes(a.store_id, b.store_id, kStoreIdSize) &&
           SameBytes(a.nonce, b.nonce, kNonceSize);
}

enum class Step : uint8_t {
    kNothing,   // in sync (or no store)
    kRecord,    // the same write on both sides: kept as the synced one
    kUpload,
    kDownload,  // the server's newer copy of our store
    kLook,      // never synced: the account's stores first (AfterLook)
    kAdopt,     // the account's one store (another store id) instead of our empty one
    kConflict,  // both changed, or two stores: by hand
};

// `local`: our file's header; `synced`: what both had at the last sync (NVS,
// !has: never); `server`: the server's newest of our store id (heads, its
// store id ours; !has: it has none); `empty`: our store holds nothing.
constexpr Step Decide(const Copy &local, const Copy &synced, const Copy &server, bool empty) {
    if (!local.has) {
        return Step::kNothing;
    }
    if (SameWrite(local, server)) {
        return SameWrite(local, synced) ? Step::kNothing : Step::kRecord;
    }
    const bool known = synced.has && SameBytes(synced.store_id, local.store_id, kStoreIdSize);
    if (!known) {
        // Never synced (or NVS lost): only an empty store gives way unasked.
        if (!server.has) {
            return Step::kLook;
        }
        return empty && server.version > local.version ? Step::kDownload : Step::kConflict;
    }
    if (!server.has) {
        return Step::kUpload;  // the server lost it (or another account): ours again
    }
    const bool ours = !SameWrite(local, synced);
    const bool theirs = !SameWrite(server, synced);
    if (!theirs) {
        return ours ? Step::kUpload : Step::kNothing;
    }
    if (!ours) {
        return server.version > local.version ? Step::kDownload : Step::kUpload;
    }
    return Step::kConflict;
}

// After kLook: `found` stores of this kind under our seed phrase (not ours).
constexpr Step AfterLook(bool empty, size_t found) {
    return found == 0 ? Step::kUpload : empty && found == 1 ? Step::kAdopt : Step::kConflict;
}

// The version "keep this device's" writes ours with: above the server's.
constexpr uint32_t KeepVersion(uint32_t server_version) { return server_version + 1; }

// --- Where each store stands (Settings > Server > Sync, the status bar) ----------

enum class Phase : uint8_t {
    kNone,      // no such store here (or not usable: refused, no card)
    kInSync,
    kChanged,   // changes to send
    kNewer,     // the server has a newer copy: taken once unlocked
    kConflict,  // both changed (or two stores): by hand
    kNeedPin,   // the server's copy has another PIN: by hand
    kFailed,    // the last try failed (Status::said)
};

constexpr const char *PhaseText(Phase p) {
    switch (p) {
        case Phase::kNone: return "none";
        case Phase::kInSync: return "in sync";
        case Phase::kChanged: return "changes to send";
        case Phase::kNewer: return "newer on the server: unlock";
        case Phase::kConflict: return "conflict";
        case Phase::kNeedPin: return "the server's copy: its PIN";
        case Phase::kFailed: return "failed";
    }
    return "";
}

// Stops the automatic sync: only a person decides.
constexpr bool ByHand(Phase p) { return p == Phase::kConflict || p == Phase::kNeedPin; }

// The status bar's sync icon.
enum class Icon : uint8_t {
    kHidden,   // no server
    kOff,      // not registered (or no client id)
    kPending,  // changes to send or to take, or a failed try
    kOk,
    kConflict,  // a conflict or a PIN to type: Settings > Server > Sync
};

constexpr Icon IconOf(bool host, bool registered, const Phase (&phases)[kStores]) {
    if (!host) {
        return Icon::kHidden;
    }
    if (!registered) {
        return Icon::kOff;
    }
    bool pending = false;
    for (const Phase p : phases) {
        if (ByHand(p)) {
            return Icon::kConflict;
        }
        pending |= p == Phase::kChanged || p == Phase::kNewer || p == Phase::kFailed;
    }
    return pending ? Icon::kPending : Icon::kOk;
}

// --- When the sync goes ---------------------------------------------------------

// A failed sync is tried again this soon.
inline constexpr int64_t kRetrySeconds = 60;

// Is a sync due? `ready`: registered, Wi-Fi online, the clock set;
// `minutes`: server.syncMinutes (0: never on its own); `by_hand`: a store
// waits for a person (ByHand): nothing on its own until then; `changed`: a
// store changed here (or the PIN came) since the last one started. `now`,
// `tried` (the last start, -1 none), `done` (the last that went through, -1
// none since the boot): monotonic seconds. The first once ready, then on a
// change and every `minutes`; a failed one again in kRetrySeconds.
constexpr bool SyncDue(bool ready, uint8_t minutes, bool by_hand, bool changed, int64_t now,
                       int64_t tried, int64_t done) {
    if (!ready || minutes == 0 || by_hand) {
        return false;
    }
    if (tried < 0 || changed) {
        return true;
    }
    if (done < tried) {
        return now - tried >= kRetrySeconds;
    }
    return now - done >= static_cast<int64_t>(minutes) * 60;
}

// --- NVS: the synced copy -------------------------------------------------------

// store id (8), version (4, LE), nonce (12).
inline constexpr size_t kSyncedSize = kStoreIdSize + 4 + kNonceSize;

constexpr void EncodeSynced(const Copy &c, uint8_t out[kSyncedSize]) {
    for (size_t i = 0; i < kStoreIdSize; ++i) {
        out[i] = c.store_id[i];
    }
    for (size_t i = 0; i < 4; ++i) {
        out[kStoreIdSize + i] = static_cast<uint8_t>(c.version >> (8 * i));
    }
    for (size_t i = 0; i < kNonceSize; ++i) {
        out[kStoreIdSize + 4 + i] = c.nonce[i];
    }
}

constexpr Copy DecodeSynced(const uint8_t in[kSyncedSize]) {
    Copy c;
    c.has = true;
    for (size_t i = 0; i < kStoreIdSize; ++i) {
        c.store_id[i] = in[i];
    }
    for (size_t i = 0; i < 4; ++i) {
        c.version |= static_cast<uint32_t>(in[kStoreIdSize + i]) << (8 * i);
    }
    for (size_t i = 0; i < kNonceSize; ++i) {
        c.nonce[i] = in[kStoreIdSize + 4 + i];
    }
    return c;
}

// --- Hex ------------------------------------------------------------------------

constexpr int HexDigit(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

// 2n hex digits into `n` bytes. False: not that.
constexpr bool FromHex(const char *text, size_t len, uint8_t *out, size_t n) {
    if (len != 2 * n) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const int hi = HexDigit(text[2 * i]);
        const int lo = HexDigit(text[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

// --- Requests -------------------------------------------------------------------

// What heads asks about: one of our stores.
struct Entry {
    uint8_t store_id[kStoreIdSize];
    uint8_t seed_id[kSeedIdSize];
    uint8_t kind;
};

inline constexpr size_t kHeadsBodySize = 160;  // two entries

// {"stores":[{"storeId":"<16 hex>","seedId":"<8 hex>","kind":N},...]}. Its
// length, 0 when it does not fit.
constexpr size_t HeadsBody(const Entry *entries, size_t count, char *out, size_t size) {
    size_t n = 0;
    out[0] = '\0';
    bool ok = PutRaw(R"({"stores":[)", out, size, &n);
    for (size_t i = 0; ok && i < count; ++i) {
        char store[2 * kStoreIdSize + 1] = {};
        char seed[2 * kSeedIdSize + 1] = {};
        Hex(entries[i].store_id, kStoreIdSize, store);
        Hex(entries[i].seed_id, kSeedIdSize, seed);
        ok = (i == 0 || PutRaw(",", out, size, &n)) && PutRaw(R"({"storeId":")", out, size, &n) &&
             PutRaw(store, out, size, &n) && PutRaw(R"(","seedId":")", out, size, &n) &&
             PutRaw(seed, out, size, &n) && PutRaw(R"(","kind":)", out, size, &n) &&
             PutNumber(entries[i].kind, out, size, &n) && PutRaw("}", out, size, &n);
    }
    ok = ok && PutRaw("]}", out, size, &n);
    return ok ? n : 0;
}

// "https://<host>/api/device/stores?seedId=<8 hex>".
constexpr bool StoresUrl(const char *host, const uint8_t seed_id[kSeedIdSize], char *out,
                         size_t size) {
    char seed[2 * kSeedIdSize + 1] = {};
    Hex(seed_id, kSeedIdSize, seed);
    size_t n = 0;
    out[0] = '\0';
    return PutRaw("https://", out, size, &n) && PutRaw(host, out, size, &n) &&
           PutRaw("/api/device/stores?seedId=", out, size, &n) && PutRaw(seed, out, size, &n);
}

// "https://<host>/api/device/stores/files/<store id>/<seed id>/<kind>?above=N".
constexpr bool DownloadUrl(const char *host, const uint8_t store_id[kStoreIdSize],
                           const uint8_t seed_id[kSeedIdSize], uint8_t kind, uint32_t above,
                           char *out, size_t size) {
    char store[2 * kStoreIdSize + 1] = {};
    char seed[2 * kSeedIdSize + 1] = {};
    Hex(store_id, kStoreIdSize, store);
    Hex(seed_id, kSeedIdSize, seed);
    size_t n = 0;
    out[0] = '\0';
    return PutRaw("https://", out, size, &n) && PutRaw(host, out, size, &n) &&
           PutRaw("/api/device/stores/files/", out, size, &n) && PutRaw(store, out, size, &n) &&
           PutRaw("/", out, size, &n) && PutRaw(seed, out, size, &n) &&
           PutRaw("/", out, size, &n) && PutNumber(kind, out, size, &n) &&
           PutRaw("?above=", out, size, &n) && PutNumber(above, out, size, &n);
}

// --- Answers --------------------------------------------------------------------

// The objects of the array `"name":[{...},{...}]`: `*at` starts at nullptr;
// each call moves it to the next object ([*obj, *obj + *obj_len)). False: no
// more (or not that shape). Strings may hold braces; objects in objects are
// skipped whole.
constexpr bool NextObject(const char *body, size_t len, const char *name, const char **at,
                          const char **obj, size_t *obj_len) {
    const char *end = body + len;
    const char *p = *at;
    if (p == nullptr) {
        p = FindValue(body, len, name);
        if (p == nullptr || p >= end || *p != '[') {
            return false;
        }
        ++p;
    }
    p = SkipSpace(p, end);
    if (p < end && *p == ',') {
        p = SkipSpace(p + 1, end);
    }
    if (p >= end || *p != '{') {
        return false;
    }
    const char *start = p;
    int depth = 0;
    bool in_string = false;
    for (; p < end; ++p) {
        if (in_string) {
            if (*p == '\\') {
                ++p;
            } else if (*p == '"') {
                in_string = false;
            }
        } else if (*p == '"') {
            in_string = true;
        } else if (*p == '{') {
            ++depth;
        } else if (*p == '}' && --depth == 0) {
            *obj = start;
            *obj_len = static_cast<size_t>(p + 1 - start);
            *at = p + 1;
            return true;
        }
    }
    return false;  // cut by the end
}

// An object's store id and kind.
constexpr bool StoreOf(const char *obj, size_t len, uint8_t store_id[kStoreIdSize],
                       int64_t *kind) {
    char text[2 * kStoreIdSize + 1] = {};
    return GetString(obj, len, "storeId", text, sizeof text) ==
               static_cast<int>(2 * kStoreIdSize) &&
           FromHex(text, 2 * kStoreIdSize, store_id, kStoreIdSize) &&
           GetNumber(obj, len, "kind", kind);
}

// An object's version and nonce into `out` (its store id set by the caller);
// !has when they are null.
constexpr void CopyOf(const char *obj, size_t len, Copy *out) {
    int64_t version = 0;
    char nonce[2 * kNonceSize + 1] = {};
    out->has = GetNumber(obj, len, "version", &version) && version >= 0 &&
               version <= 0xFFFFFFFF &&
               GetString(obj, len, "nonce", nonce, sizeof nonce) ==
                   static_cast<int>(2 * kNonceSize) &&
               FromHex(nonce, 2 * kNonceSize, out->nonce, kNonceSize);
    out->version = out->has ? static_cast<uint32_t>(version) : 0;
}

// heads' answer for one of our stores: the server's newest of it (`out`, its
// store id ours; !has: none). False: the answer does not name it.
constexpr bool ParseHeads(const char *body, size_t len, const uint8_t store_id[kStoreIdSize],
                          uint8_t kind, Copy *out) {
    const char *at = nullptr;
    const char *obj = nullptr;
    size_t obj_len = 0;
    while (NextObject(body, len, "stores", &at, &obj, &obj_len)) {
        uint8_t id[kStoreIdSize] = {};
        int64_t k = 0;
        if (!StoreOf(obj, obj_len, id, &k) || k != kind || !SameBytes(id, store_id, kStoreIdSize)) {
            continue;
        }
        *out = Copy();
        for (size_t i = 0; i < kStoreIdSize; ++i) {
            out->store_id[i] = store_id[i];
        }
        CopyOf(obj, obj_len, out);
        return true;
    }
    return false;
}

// One of the account's stores (GET /api/device/stores).
struct Candidate {
    Copy copy;
    uint8_t key_id[kKeyIdSize] = {};
    int64_t modified = 0;  // Unix seconds, 0 unknown
    char by[kClientIdSize + 1] = "";  // the device that uploaded it
    char at[17] = "";                 // when: "2026-10-27 19:07" (UTC)
};

// The account's stores of `kind` other than `ours`: how many, and the first
// (the newest sync: the server lists them so) into `first`. -1: not the
// answer.
constexpr int ParseStores(const char *body, size_t len, uint8_t kind,
                          const uint8_t ours[kStoreIdSize], Candidate *first) {
    if (FindValue(body, len, "stores") == nullptr) {
        return -1;
    }
    const char *at = nullptr;
    const char *obj = nullptr;
    size_t obj_len = 0;
    int found = 0;
    while (NextObject(body, len, "stores", &at, &obj, &obj_len)) {
        Candidate c;
        int64_t k = 0;
        if (!StoreOf(obj, obj_len, c.copy.store_id, &k) || k != kind ||
            SameBytes(c.copy.store_id, ours, kStoreIdSize)) {
            continue;
        }
        CopyOf(obj, obj_len, &c.copy);
        if (!c.copy.has) {
            continue;
        }
        if (++found > 1) {
            continue;
        }
        char key[2 * kKeyIdSize + 1] = {};
        if (GetString(obj, obj_len, "keyId", key, sizeof key) == static_cast<int>(2 * kKeyIdSize)) {
            FromHex(key, 2 * kKeyIdSize, c.key_id, kKeyIdSize);
        }
        GetNumber(obj, obj_len, "modified", &c.modified);
        if (GetString(obj, obj_len, "uploadedByDeviceId", c.by, sizeof c.by) < 0) {
            c.by[0] = '\0';
        }
        char when[40] = "";
        if (GetString(obj, obj_len, "uploadedAt", when, sizeof when) >= 16) {
            for (size_t i = 0; i < 16; ++i) {
                c.at[i] = when[i] == 'T' ? ' ' : when[i];
            }
            c.at[16] = '\0';
        }
        *first = c;
    }
    return found;
}

// --- Other seed phrases' stores (shown only: they never open here) ------------

enum class OtherKind : uint8_t {
    kOld,    // one of ours under the phrase it had before (this device's phrase changed)
    kMoved,  // one of ours, newer, under another phrase: changed on another device
    kOther,  // another store: a device set up with its own phrase
};

struct Other {
    OtherKind why = OtherKind::kOther;
    uint8_t seed_id[kSeedIdSize] = {};
    uint8_t kind = 0;
    uint32_t version = 0;
    char at[17] = "";  // when it reached the server (UTC), or ""
};

struct Others {
    uint8_t count = 0;  // the stores under other phrases (as far as the answer holds)
    Other shown;        // the one to tell: a moved one first, then another device's, then an old one
};

// GET /api/device/stores (every phrase's) against our seed id and our stores
// (`ours[i]` the local copy of kAll[i], !has: none). False: not the answer.
constexpr bool ParseOthers(const char *body, size_t len, const uint8_t own_seed[kSeedIdSize],
                           const Copy (&ours)[kStores], Others *out) {
    if (FindValue(body, len, "stores") == nullptr) {
        return false;
    }
    *out = Others();
    auto rank = [](OtherKind k) { return k == OtherKind::kMoved ? 2 : k == OtherKind::kOther ? 1 : 0; };
    const char *at = nullptr;
    const char *obj = nullptr;
    size_t obj_len = 0;
    while (NextObject(body, len, "stores", &at, &obj, &obj_len)) {
        Other o;
        uint8_t store_id[kStoreIdSize] = {};
        int64_t kind = 0;
        char seed[2 * kSeedIdSize + 1] = {};
        if (!StoreOf(obj, obj_len, store_id, &kind) ||
            GetString(obj, obj_len, "seedId", seed, sizeof seed) != static_cast<int>(2 * kSeedIdSize) ||
            !FromHex(seed, 2 * kSeedIdSize, o.seed_id, kSeedIdSize) ||
            SameBytes(o.seed_id, own_seed, kSeedIdSize)) {
            continue;
        }
        Copy c;
        CopyOf(obj, obj_len, &c);
        o.kind = static_cast<uint8_t>(kind);
        o.version = c.version;
        const Copy *mine = nullptr;
        for (size_t i = 0; i < kStores; ++i) {
            if (ours[i].has && KindOf(kAll[i]) == kind &&
                SameBytes(ours[i].store_id, store_id, kStoreIdSize)) {
                mine = &ours[i];
            }
        }
        o.why = mine == nullptr                ? OtherKind::kOther
                : c.version > mine->version    ? OtherKind::kMoved
                                               : OtherKind::kOld;
        char when[40] = "";
        if (GetString(obj, obj_len, "uploadedAt", when, sizeof when) >= 16) {
            for (size_t i = 0; i < 16; ++i) {
                o.at[i] = when[i] == 'T' ? ' ' : when[i];
            }
            o.at[16] = '\0';
        }
        if (out->count == 0 || rank(o.why) > rank(out->shown.why)) {
            out->shown = o;
        }
        if (out->count < 255) {
            ++out->count;
        }
    }
    return true;
}

constexpr const char *KindName(uint8_t kind) {
    return kind == 2 ? "passwords" : kind == 1 ? "passkeys" : "a store";
}

}  // namespace server::sync
