#include "store_header_c.h"

#include "store_header.h"

static_assert(STORE_HEADER_SIZE == storefile::kHeaderSize);
static_assert(STORE_KIND_FIDO == static_cast<uint8_t>(storefile::Kind::kFido));
static_assert(STORE_NONCE_SIZE == storefile::kNonceSize);
static_assert(STORE_TAG_SIZE == storefile::kTagSize);
static_assert(STORE_SALT_SIZE == storefile::kSaltSize);
static_assert(STORE_KEY_ID_SIZE == storefile::kKeyIdSize);

extern "C" {

void store_header_encode(uint8_t kind, const store_header_t *header,
                         uint8_t out[STORE_HEADER_SIZE]) {
    storefile::Header h;
    h.kind = static_cast<storefile::Kind>(kind);
    h.cipher = static_cast<storefile::Cipher>(header->cipher);
    h.version = header->version;
    h.length = header->length;
    h.modified_utc = header->modified_utc;
    for (size_t i = 0; i < STORE_NONCE_SIZE; ++i) {
        h.nonce[i] = header->nonce[i];
    }
    h.kdf_iterations = header->kdf_iterations;
    for (size_t i = 0; i < STORE_SALT_SIZE; ++i) {
        h.salt[i] = header->salt[i];
    }
    for (size_t i = 0; i < STORE_KEY_ID_SIZE; ++i) {
        h.key_id[i] = header->key_id[i];
    }
    storefile::Encode(h, out);
}

bool store_header_decode(const uint8_t *data, size_t size, size_t file_size, uint8_t kind,
                         store_header_t *out) {
    storefile::Header h;
    if (!storefile::Decode(data, size, file_size, static_cast<storefile::Kind>(kind), &h)) {
        return false;
    }
    out->cipher = static_cast<uint8_t>(h.cipher);
    out->version = h.version;
    out->length = h.length;
    out->modified_utc = h.modified_utc;
    for (size_t i = 0; i < STORE_NONCE_SIZE; ++i) {
        out->nonce[i] = h.nonce[i];
    }
    out->kdf_iterations = h.kdf_iterations;
    for (size_t i = 0; i < STORE_SALT_SIZE; ++i) {
        out->salt[i] = h.salt[i];
    }
    for (size_t i = 0; i < STORE_KEY_ID_SIZE; ++i) {
        out->key_id[i] = h.key_id[i];
    }
    return true;
}

bool store_header_rollback(uint32_t version, bool nvs_has, uint32_t nvs_version) {
    return storefile::Rollback(version, nvs_has, nvs_version);
}

uint32_t store_header_next_version(uint32_t file_version, uint32_t nvs_version) {
    return storefile::NextVersion(file_version, nvs_version);
}

}  // extern "C"
