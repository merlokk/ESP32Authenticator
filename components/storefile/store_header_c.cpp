#include "store_header_c.h"

#include "store_header.h"

static_assert(STORE_HEADER_SIZE == storefile::kHeaderSize);
static_assert(STORE_KIND_FIDO == static_cast<uint8_t>(storefile::Kind::kFido));

extern "C" {

void store_header_encode(uint8_t kind, const store_header_t *header,
                         uint8_t out[STORE_HEADER_SIZE]) {
    storefile::Header h;
    h.kind = static_cast<storefile::Kind>(kind);
    h.cipher = static_cast<storefile::Cipher>(header->cipher);
    h.version = header->version;
    h.length = header->length;
    h.modified_utc = header->modified_utc;
    storefile::Encode(h, out);
}

store_parse_t store_header_decode(const uint8_t *data, size_t size, size_t file_size,
                                  uint8_t kind, store_header_t *out) {
    storefile::Header h;
    switch (storefile::Decode(data, size, file_size, static_cast<storefile::Kind>(kind), &h)) {
        case storefile::Parse::kOk:
            break;
        case storefile::Parse::kNoHeader:
            return STORE_PARSE_NO_HEADER;
        default:
            return STORE_PARSE_BAD;
    }
    out->cipher = static_cast<uint8_t>(h.cipher);
    out->version = h.version;
    out->length = h.length;
    out->modified_utc = h.modified_utc;
    return STORE_PARSE_OK;
}

store_verdict_t store_header_check(store_parse_t parse, uint32_t version, bool nvs_has,
                                   uint32_t nvs_version) {
    const storefile::Parse p =
        parse == STORE_PARSE_NO_HEADER ? storefile::Parse::kNoHeader : storefile::Parse::kOk;
    switch (storefile::Check(p, version, nvs_has, nvs_version)) {
        case storefile::Verdict::kLoad:
            return STORE_LOAD;
        case storefile::Verdict::kMigrate:
            return STORE_MIGRATE;
        default:
            return STORE_ROLLBACK;
    }
}

uint32_t store_header_next_version(uint32_t file_version, uint32_t nvs_version) {
    return storefile::NextVersion(file_version, nvs_version);
}

}  // extern "C"
