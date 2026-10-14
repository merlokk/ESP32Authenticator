#include "records.h"

#include <cstring>
#include <initializer_list>

#include "totp.h"

namespace vault {

namespace {

constexpr uint8_t kFormat = 1;

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

int CompareNoCase(const char *a, const char *b) {
    for (;; ++a, ++b) {
        const char x = Lower(*a);
        const char y = Lower(*b);
        if (x != y || x == '\0') {
            return static_cast<unsigned char>(x) - static_cast<unsigned char>(y);
        }
    }
}

int Order(const Record &a, const Record &b) {
    const int site = CompareNoCase(a.site, b.site);
    return site != 0 ? site : CompareNoCase(a.user, b.user);
}

size_t MaxOf(Field field) {
    switch (field) {
        case Field::kSite:
            return kSiteMax;
        case Field::kUser:
            return kUserMax;
        case Field::kPassword:
            return kPasswordMax;
        default:
            return kOtpMax;
    }
}

char *FieldOf(Record &r, Field field) {
    switch (field) {
        case Field::kSite:
            return r.site;
        case Field::kUser:
            return r.user;
        case Field::kPassword:
            return r.password;
        default:
            return r.otp;
    }
}

Error CheckOtp(const char *value, char *out) {
    size_t n = 0;
    for (const char *p = value; *p != '\0'; ++p) {
        if (*p == ' ' || *p == '-' || *p == '=') {
            continue;
        }
        if (n == kOtpMax) {
            return Error::kTooLong;
        }
        out[n++] = *p >= 'a' && *p <= 'z' ? static_cast<char>(*p - 'a' + 'A') : *p;
    }
    out[n] = '\0';
    if (n == 0) {
        return Error::kOk;  // no TOTP
    }
    uint8_t key[totp::kMaxKeyBytes];
    const size_t bytes = totp::Base32Decode(out, key, sizeof key);
    Wipe(key, sizeof key);
    if (bytes == 0) {
        out[0] = '\0';
        return Error::kBadOtp;
    }
    return Error::kOk;
}

void Put(uint8_t *&p, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        *p++ = static_cast<uint8_t>(v >> (8 * i));
    }
}

uint64_t Get(const uint8_t *&p, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) {
        v |= static_cast<uint64_t>(*p++) << (8 * i);
    }
    return v;
}

}  // namespace

const char *Describe(Error error) {
    switch (error) {
        case Error::kOk:
            return "ok";
        case Error::kSiteEmpty:
            return "the site is empty";
        case Error::kTooLong:
            return "too long";
        case Error::kBadText:
            return "control characters are not allowed";
        case Error::kBadOtp:
            return "the TOTP secret is not Base32 (A-Z, 2-7)";
        case Error::kDuplicate:
            return "this site already has this user";
        case Error::kFull:
            return "the store is full";
        case Error::kNotFound:
            return "no such record";
        case Error::kBadFormat:
            return "the stored data is malformed";
        case Error::kLocked:
            return "the password store is not open";
        case Error::kStorage:
            return "the store file could not be written; nothing changed";
        case Error::kBadPin:
            return "the PIN is 6 digits";
        case Error::kWrongPin:
            return "wrong PIN";
        case Error::kPinLocked:
            return "too many wrong PINs: PIN entry is locked for a while";
        case Error::kPinInUse:
            return "the PIN and the wipe PIN must differ";
        case Error::kWiped:
            return "wiped: the password store, the FIDO2 credentials and the key are gone";
    }
    return "?";
}

bool ValidPin(const char *pin) {
    size_t n = 0;
    for (; pin[n] != '\0'; ++n) {
        if (pin[n] < '0' || pin[n] > '9' || n == kPinLength) {
            return false;
        }
    }
    return n == kPinLength;
}

void Wipe(void *data, size_t size) {
    volatile uint8_t *p = static_cast<volatile uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
        p[i] = 0;
    }
}

Error CheckField(Field field, const char *value, char *out) {
    if (field == Field::kOtp) {
        return CheckOtp(value, out);
    }
    const size_t length = strlen(value);
    if (length > MaxOf(field)) {
        return Error::kTooLong;
    }
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c < 0x20 || c == 0x7F) {
            return Error::kBadText;
        }
    }
    if (field == Field::kSite && length == 0) {
        return Error::kSiteEmpty;
    }
    memcpy(out, value, length + 1);
    return Error::kOk;
}

long Records::Find(uint32_t id) const {
    for (size_t i = 0; i < count_; ++i) {
        if (slots_[i].id == id) {
            return static_cast<long>(i);
        }
    }
    return -1;
}

bool Records::Duplicate(const char *site, const char *user, uint32_t except_id) const {
    for (size_t i = 0; i < count_; ++i) {
        if (slots_[i].id != except_id && CompareNoCase(slots_[i].site, site) == 0 &&
            CompareNoCase(slots_[i].user, user) == 0) {
            return true;
        }
    }
    return false;
}

// Moves the record at `from` to where the order puts it among the others
// (one Record on the stack).
void Records::Place(size_t from) {
    Record moving = slots_[from];
    memmove(&slots_[from], &slots_[from + 1], (count_ - from - 1) * sizeof(Record));
    size_t at = count_ - 1;
    while (at > 0 && Order(moving, slots_[at - 1]) < 0) {
        --at;
    }
    memmove(&slots_[at + 1], &slots_[at], (count_ - 1 - at) * sizeof(Record));
    slots_[at] = moving;
    Wipe(&moving, sizeof moving);
}

bool Records::Insert() {
    Record &fresh = slots_[count_];
    if (Duplicate(fresh.site, fresh.user, fresh.id)) {
        Wipe(&fresh, sizeof fresh);
        return false;
    }
    ++count_;
    Place(count_ - 1);
    return true;
}

Error Records::Add(const char *site, const char *user, const char *password, const char *otp,
                   int64_t now, uint32_t *id) {
    if (count_ == capacity_) {
        return Error::kFull;
    }
    Record &r = slots_[count_];
    Wipe(&r, sizeof r);
    Error e = CheckField(Field::kSite, site, r.site);
    if (e == Error::kOk) e = CheckField(Field::kUser, user, r.user);
    if (e == Error::kOk) e = CheckField(Field::kPassword, password, r.password);
    if (e == Error::kOk) e = CheckField(Field::kOtp, otp, r.otp);
    if (e != Error::kOk) {
        Wipe(&r, sizeof r);
        return e;
    }
    r.id = next_id_;
    r.modified_utc = now;
    const uint32_t made = r.id;
    if (!Insert()) {
        return Error::kDuplicate;
    }
    ++next_id_;
    if (id != nullptr) {
        *id = made;
    }
    return Error::kOk;
}

Error Records::Set(uint32_t id, Field field, const char *value, int64_t now) {
    const long at = Find(id);
    if (at < 0) {
        return Error::kNotFound;
    }
    char text[kPasswordMax + 1];
    const Error e = CheckField(field, value, text);
    if (e != Error::kOk) {
        Wipe(text, sizeof text);
        return e;
    }
    Record &old = slots_[at];
    if ((field == Field::kSite && Duplicate(text, old.user, id)) ||
        (field == Field::kUser && Duplicate(old.site, text, id))) {
        return Error::kDuplicate;
    }
    memcpy(FieldOf(old, field), text, strlen(text) + 1);
    Wipe(text, sizeof text);
    old.modified_utc = now;
    Place(static_cast<size_t>(at));
    return Error::kOk;
}

Error Records::Remove(uint32_t id) {
    const long at = Find(id);
    if (at < 0) {
        return Error::kNotFound;
    }
    memmove(&slots_[at], &slots_[at + 1], (count_ - at - 1) * sizeof(Record));
    --count_;
    Wipe(&slots_[count_], sizeof(Record));
    return Error::kOk;
}

void Records::Clear() {
    Wipe(slots_, capacity_ * sizeof(Record));
    count_ = 0;
    next_id_ = 1;
}

size_t Records::Serialize(uint8_t *out, size_t capacity) const {
    size_t need = 1 + 4 + 2;
    for (size_t i = 0; i < count_; ++i) {
        const Record &r = slots_[i];
        need += 4 + 8 + 4 + strlen(r.site) + strlen(r.user) + strlen(r.password) + strlen(r.otp);
    }
    if (need > capacity) {
        return 0;
    }
    uint8_t *p = out;
    *p++ = kFormat;
    Put(p, next_id_, 4);
    Put(p, count_, 2);
    for (size_t i = 0; i < count_; ++i) {
        const Record &r = slots_[i];
        Put(p, r.id, 4);
        Put(p, static_cast<uint64_t>(r.modified_utc), 8);
        for (const char *text : {r.site, r.user, r.password, r.otp}) {
            const size_t n = strlen(text);
            *p++ = static_cast<uint8_t>(n);
            memcpy(p, text, n);
            p += n;
        }
    }
    return static_cast<size_t>(p - out);
}

Error Records::Parse(const uint8_t *data, size_t size) {
    Clear();
    const uint8_t *p = data;
    const uint8_t *end = data + size;
    if (size < 7 || *p++ != kFormat) {
        return Error::kBadFormat;
    }
    const uint32_t next_id = static_cast<uint32_t>(Get(p, 4));
    const size_t count = static_cast<size_t>(Get(p, 2));
    if (count > capacity_) {
        return Error::kFull;
    }
    const Field fields[] = {Field::kSite, Field::kUser, Field::kPassword, Field::kOtp};
    char text[kPasswordMax + 1];
    Error e = Error::kOk;
    for (size_t i = 0; i < count && e == Error::kOk; ++i) {
        if (end - p < 12) {
            e = Error::kBadFormat;
            break;
        }
        Record &r = slots_[count_];
        r.id = static_cast<uint32_t>(Get(p, 4));
        r.modified_utc = static_cast<int64_t>(Get(p, 8));
        if (r.id == 0 || r.id >= next_id || Find(r.id) >= 0) {
            e = Error::kBadFormat;
            break;
        }
        for (Field field : fields) {
            if (end - p < 1 || static_cast<size_t>(end - p - 1) < *p || *p > MaxOf(field)) {
                e = Error::kBadFormat;
                break;
            }
            const size_t n = *p++;
            memcpy(text, p, n);
            text[n] = '\0';
            p += n;
            if (CheckField(field, text, FieldOf(r, field)) != Error::kOk) {
                e = Error::kBadFormat;
                break;
            }
        }
        if (e == Error::kOk && !Insert()) {
            e = Error::kBadFormat;
        }
    }
    Wipe(text, sizeof text);
    if (e == Error::kOk && p != end) {
        e = Error::kBadFormat;
    }
    if (e != Error::kOk) {
        Clear();
        return e;
    }
    next_id_ = next_id;
    return Error::kOk;
}

}  // namespace vault
