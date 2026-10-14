#pragma once

// LionKey storage: its RAM log (ctap_memory_storage) mirrored to a file.

#include "ctap_memory_storage.h"
#include "fido_ctap.h"

typedef struct {
    ctap_memory_storage_context_t mem;  // first: the memory storage casts the context to it
    const char *path;                   // NULL: RAM only
    size_t max_credentials;             // KEY_STORE_FULL beyond it
    bool (*client)(fido_ctap_client_t *out);  // creator of new credentials, may be NULL
    // The file's header (store_header.h): version and modification time of
    // what is loaded / last written; 0 before the first write.
    uint32_t version;
    int64_t modified_utc;
    const char *refused;  // why the file was not loaded (init failed), else NULL
} ctap_file_storage_context_t;

// Our own item next to each credential (lionkey ignores unknown keys), made,
// replaced and deleted with it. Fixed little-endian layout (it travels with
// the store between devices):
//   0  credential ID (32)
//  32  host: transport (1), address type (1), address (6, MSB first)
//  40  created, Unix seconds, int64 (0: no believable time)
//  48  this authenticator's base MAC (6)
//  54  reserved (2)
// Records of 40 bytes (first firmware with it) carry the host only.
#define CTAP_FILE_STORAGE_KEY_ORIGIN 0x100u
#define CTAP_FILE_STORAGE_ORIGIN_SIZE 56u

// The origin of a credential; all unknown when not recorded.
fido_ctap_origin_t ctap_file_storage_origin(const ctap_storage_t *storage, const uint8_t id[32]);

ctap_storage_status_t ctap_file_storage_init(const ctap_storage_t *storage);
ctap_storage_status_t ctap_file_storage_create_or_update_item(const ctap_storage_t *storage,
                                                              ctap_storage_item_t *item);
ctap_storage_status_t ctap_file_storage_delete_item(const ctap_storage_t *storage,
                                                    uint32_t item_handle);
ctap_storage_status_t ctap_file_storage_increment_counter(const ctap_storage_t *storage,
                                                          uint32_t increment,
                                                          uint32_t *counter_new_value);
size_t ctap_file_storage_estimate_num_remaining_items(const ctap_storage_t *storage,
                                                     const ctap_storage_item_t *item);
ctap_storage_status_t ctap_file_storage_erase(const ctap_storage_t *storage);

// The global signature counter (NVS), 0 if never used.
uint32_t ctap_file_storage_counter(void);

// The highest store version this device has accepted (NVS), 0 if none.
uint32_t ctap_file_storage_nvs_version(void);

#define CTAP_FILE_STORAGE_CONST_INIT(context_ptr)                                    \
    {                                                                                \
        .context = (context_ptr),                                                    \
        .init = ctap_file_storage_init,                                              \
        .find_item = ctap_memory_storage_find_item,                                  \
        .create_or_update_item = ctap_file_storage_create_or_update_item,            \
        .delete_item = ctap_file_storage_delete_item,                                \
        .increment_counter = ctap_file_storage_increment_counter,                    \
        .estimate_num_remaining_items = ctap_file_storage_estimate_num_remaining_items, \
        .erase = ctap_file_storage_erase,                                            \
    }
