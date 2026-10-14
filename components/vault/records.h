#pragma once

// Password records (site, user, password, TOTP secret) in RAM and their
// serialized form (the plaintext inside the encrypted file). Pure, host-tested;
// vault.h loads, encrypts and saves them.
//
// - A site may have several users; (site, user) is unique, compared without
//   ASCII case.
// - Kept sorted by site, then user (ASCII case-insensitive): list position is
//   the order shown. Each record has an ID that never changes or repeats.
// - Text is UTF-8, no control characters. The TOTP secret is kept as
//   normalized Base32 (upper case, no spaces, '-' or '=').
//
// Serialized (little-endian):
//   format (1) = 1 | next id (4) | count (2) | records
//   record: id (4) | modified (8, Unix seconds, 0 unknown) |
//           site, user, password, otp: length (1) + bytes each

#include <cstddef>
#include <cstdint>

namespace vault {

inline constexpr size_t kSiteMax = 96;
inline constexpr size_t kUserMax = 96;
inline constexpr size_t kPasswordMax = 128;
inline constexpr size_t kOtpMax = 64;  // Base32 characters (up to 40 key bytes)

struct Record {
    uint32_t id;
    int64_t modified_utc;
    char site[kSiteMax + 1];
    char user[kUserMax + 1];
    char password[kPasswordMax + 1];
    char otp[kOtpMax + 1];
};

enum class Field : uint8_t { kSite, kUser, kPassword, kOtp };

enum class Error : uint8_t {
    kOk,
    kSiteEmpty,
    kTooLong,
    kBadText,    // a control character
    kBadOtp,     // not Base32
    kDuplicate,  // this site already has this user
    kFull,
    kNotFound,
    kBadFormat,  // serialized data does not parse
    // From vault.h, not from Records:
    kLocked,    // not loaded (no PIN yet, or the file was refused)
    kStorage,   // the file could not be written: the change is undone
    kBadPin,    // not 6 digits
    kWrongPin,  // the file does not decrypt with it
    kPinLocked,  // too many wrong PINs: wait (Info::lock_left_s)
    kPinInUse,   // the PIN and the wipe PIN must differ
    kFidoNotLoaded,  // a PIN change needs fido.bin loaded (or no fido.bin)
};

inline constexpr size_t kPinLength = 6;

// Exactly 6 ASCII digits.
bool ValidPin(const char *pin);

const char *Describe(Error error);

// Bytes of one serialized record at most.
inline constexpr size_t kMaxSerializedRecord =
    4 + 8 + 4 + kSiteMax + kUserMax + kPasswordMax + kOtpMax;

constexpr size_t MaxSerialized(size_t capacity) {
    return 1 + 4 + 2 + capacity * kMaxSerializedRecord;
}

// Checks `value` for `field` and copies it (OTP: normalized) to `out`, which
// has room for the field's maximum + NUL. An empty OTP or password is allowed.
Error CheckField(Field field, const char *value, char *out);

// The records on `slots` (caller memory, `capacity` of them).
class Records {
public:
    Records(Record *slots, size_t capacity) : slots_(slots), capacity_(capacity) {}

    size_t Count() const { return count_; }
    size_t Capacity() const { return capacity_; }
    const Record &At(size_t index) const { return slots_[index]; }
    // List position of the record with this ID, -1 if none.
    long Find(uint32_t id) const;

    // `now`: Unix seconds, 0 when unknown.
    Error Add(const char *site, const char *user, const char *password, const char *otp,
              int64_t now, uint32_t *id);
    Error Set(uint32_t id, Field field, const char *value, int64_t now);
    Error Remove(uint32_t id);
    // Empty, with every slot wiped.
    void Clear();

    // Bytes written, 0 if `capacity` is too small.
    size_t Serialize(uint8_t *out, size_t capacity) const;
    // Replaces the records; on an error they are empty.
    Error Parse(const uint8_t *data, size_t size);

private:
    // Puts slots_[count_] (filled) into sorted position; false: duplicate.
    bool Insert();
    void Place(size_t from);
    bool Duplicate(const char *site, const char *user, uint32_t except_id) const;

    Record *slots_;
    size_t capacity_;
    size_t count_ = 0;
    uint32_t next_id_ = 1;
};

// Overwrites memory the compiler may not skip (secrets).
void Wipe(void *data, size_t size);

}  // namespace vault
