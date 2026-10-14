#pragma once

// C view of store_header.h, for the C store code (ctap_file_storage.c).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STORE_HEADER_SIZE 64u
#define STORE_KIND_FIDO 1u

#define STORE_NONCE_SIZE 12u
#define STORE_TAG_SIZE 16u

typedef struct {
    uint8_t cipher;  // 0 plain, 1 AES-256-GCM
    uint32_t version;
    uint32_t length;
    int64_t modified_utc;
    uint8_t nonce[STORE_NONCE_SIZE];
} store_header_t;

// Header of `kind` (no KDF fields).
void store_header_encode(uint8_t kind, const store_header_t *header,
                         uint8_t out[STORE_HEADER_SIZE]);
bool store_header_decode(const uint8_t *data, size_t size, size_t file_size, uint8_t kind,
                         store_header_t *out);
bool store_header_rollback(uint32_t version, bool nvs_has, uint32_t nvs_version);
uint32_t store_header_next_version(uint32_t file_version, uint32_t nvs_version);

#ifdef __cplusplus
}
#endif
