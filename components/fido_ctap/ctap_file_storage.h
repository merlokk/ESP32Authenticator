#pragma once

// LionKey storage: its RAM log (ctap_memory_storage) mirrored to a file.

#include "ctap_memory_storage.h"

typedef struct {
    ctap_memory_storage_context_t mem;  // first: the memory storage casts the context to it
    const char *path;                   // NULL: RAM only
    size_t max_credentials;             // KEY_STORE_FULL beyond it
} ctap_file_storage_context_t;

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
