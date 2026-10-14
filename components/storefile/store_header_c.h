#pragma once

// C view of store_header.h, for the C store code (ctap_file_storage.c).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STORE_HEADER_SIZE 64u  // written; the most a header takes
#define STORE_KIND_FIDO 1u

typedef struct {
    uint8_t cipher;  // 0 plain
    uint32_t version;
    uint32_t length;
    int64_t modified_utc;
    uint8_t header_size;  // decoded: bytes before the payload (40 or 64)
} store_header_t;

typedef enum { STORE_PARSE_OK, STORE_PARSE_NO_HEADER, STORE_PARSE_BAD } store_parse_t;
typedef enum { STORE_LOAD, STORE_MIGRATE, STORE_ROLLBACK } store_verdict_t;

// Plain header of `kind` (nonce zero).
void store_header_encode(uint8_t kind, const store_header_t *header,
                         uint8_t out[STORE_HEADER_SIZE]);
store_parse_t store_header_decode(const uint8_t *data, size_t size, size_t file_size,
                                  uint8_t kind, store_header_t *out);
store_verdict_t store_header_check(store_parse_t parse, uint32_t version, bool nvs_has,
                                   uint32_t nvs_version);
uint32_t store_header_next_version(uint32_t file_version, uint32_t nvs_version);

#ifdef __cplusplus
}
#endif
