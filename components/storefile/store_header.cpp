#include "store_header.h"

namespace storefile {

namespace {

constexpr uint8_t kMagic[4] = {'E', 'S', 'A', 'F'};

void Put32(uint8_t *out, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<uint8_t>(v >> (8 * i));
    }
}

uint32_t Get32(const uint8_t *in) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<uint32_t>(in[i]) << (8 * i);
    }
    return v;
}

}  // namespace

void Encode(const Header &header, uint8_t out[kHeaderSize]) {
    for (size_t i = 0; i < kHeaderSize; ++i) {
        out[i] = 0;
    }
    for (int i = 0; i < 4; ++i) {
        out[i] = kMagic[i];
    }
    out[4] = kFormat;
    out[5] = static_cast<uint8_t>(header.kind);
    out[6] = static_cast<uint8_t>(header.cipher);
    Put32(out + 8, header.version);
    Put32(out + 12, header.length);
    const uint64_t modified = static_cast<uint64_t>(header.modified_utc);
    Put32(out + 16, static_cast<uint32_t>(modified));
    Put32(out + 20, static_cast<uint32_t>(modified >> 32));
    for (size_t i = 0; i < kNonceSize; ++i) {
        out[24 + i] = header.nonce[i];
    }
    Put32(out + 36, header.kdf_iterations);
    for (size_t i = 0; i < kSaltSize; ++i) {
        out[40 + i] = header.salt[i];
    }
    for (size_t i = 0; i < kKeyIdSize; ++i) {
        out[56 + i] = header.key_id[i];
    }
    for (size_t i = 0; i < kStoreIdSize; ++i) {
        out[64 + i] = header.store_id[i];
    }
    for (size_t i = 0; i < kSeedIdSize; ++i) {
        out[72 + i] = header.seed_id[i];
    }
}

bool Decode(const uint8_t *data, size_t size, size_t file_size, Kind kind, Header *out) {
    if (size < kHeaderSize || data[0] != kMagic[0] || data[1] != kMagic[1] ||
        data[2] != kMagic[2] || data[3] != kMagic[3] || data[4] != kFormat ||
        data[5] != static_cast<uint8_t>(kind) ||
        data[6] > static_cast<uint8_t>(Cipher::kAesGcm)) {
        return false;
    }
    Header h;
    h.kind = kind;
    h.cipher = static_cast<Cipher>(data[6]);
    h.version = Get32(data + 8);
    h.length = Get32(data + 12);
    h.modified_utc = static_cast<int64_t>(static_cast<uint64_t>(Get32(data + 16)) |
                                          static_cast<uint64_t>(Get32(data + 20)) << 32);
    for (size_t i = 0; i < kNonceSize; ++i) {
        h.nonce[i] = data[24 + i];
    }
    h.kdf_iterations = Get32(data + 36);
    for (size_t i = 0; i < kSaltSize; ++i) {
        h.salt[i] = data[40 + i];
    }
    for (size_t i = 0; i < kKeyIdSize; ++i) {
        h.key_id[i] = data[56 + i];
    }
    for (size_t i = 0; i < kStoreIdSize; ++i) {
        h.store_id[i] = data[64 + i];
    }
    for (size_t i = 0; i < kSeedIdSize; ++i) {
        h.seed_id[i] = data[72 + i];
    }
    const size_t tag = h.cipher == Cipher::kPlain ? 0 : kTagSize;
    // A truncated or padded file: refused, not guessed at.
    if (file_size < kHeaderSize + tag || file_size - kHeaderSize - tag != h.length) {
        return false;
    }
    *out = h;
    return true;
}

bool Rollback(uint32_t version, bool nvs_has, uint32_t nvs_version) {
    return nvs_has && version < nvs_version;
}

uint32_t NextVersion(uint32_t file_version, uint32_t nvs_version) {
    const uint32_t top = file_version > nvs_version ? file_version : nvs_version;
    return top == UINT32_MAX ? top : top + 1;
}

bool Newer(const Header &a, const Header &b) {
    if (a.modified_utc != b.modified_utc) {
        return a.modified_utc > b.modified_utc;  // 0 (unknown) is below any time
    }
    return a.version > b.version;
}

}  // namespace storefile
