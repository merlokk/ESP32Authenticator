// LionKey storage on ESP-IDF: lionkey's RAM log (ctap_memory_storage, an
// append-only list of items, deleted ones only flagged) is the working copy;
// the file holds its used part [0, write_index).
//
// - Every change rewrites the file through a temp file + rename, so a power
//   loss leaves either the old or the new store.
// - Deleted items are dropped (compaction) only at init: later the core keeps
//   item handles (offsets), which compaction would move.
// - The global signature counter lives in NVS: otherwise every assertion
//   would append an item to the log.
// - At most max_credentials credentials (lionkey stores non-discoverable ones
//   too); creating one more fails with KEY_STORE_FULL.

#include "ctap_file_storage.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "ctap_store";

static const char *kNvsNamespace = "fido";
static const char *kNvsCounter = "sign_count";

static ctap_file_storage_context_t *ctx_of(const ctap_storage_t *storage) {
    return (ctap_file_storage_context_t *)storage->context;
}

static ctap_storage_status_t save(const ctap_storage_t *storage) {
    ctap_file_storage_context_t *ctx = ctx_of(storage);
    if (ctx->path == NULL) {
        return CTAP_STORAGE_OK;
    }
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%s.tmp", ctx->path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "cannot write %s", tmp);
        return CTAP_STORAGE_ERROR;
    }
    const size_t n = ctx->mem.write_index;
    const bool ok = fwrite(ctx->mem.memory, 1, n, f) == n;
    if (fclose(f) != 0 || !ok) {
        ESP_LOGE(TAG, "write %s failed", tmp);
        remove(tmp);
        return CTAP_STORAGE_ERROR;
    }
    remove(ctx->path);  // SPIFFS rename does not replace
    if (rename(tmp, ctx->path) != 0) {
        ESP_LOGE(TAG, "rename %s failed", tmp);
        return CTAP_STORAGE_ERROR;
    }
    return CTAP_STORAGE_OK;
}

// Rebuilds the log from its live items, which also sets write_index (lionkey's
// ctap_memory_storage_init() asserts on a non-empty memory, so it is not
// used). Returns false if out of memory.
static bool compact(const ctap_storage_t *storage) {
    ctap_file_storage_context_t *ctx = ctx_of(storage);
    uint8_t *fresh = calloc(1, ctx->mem.memory_size);
    if (fresh == NULL) {
        return false;
    }
    ctap_memory_storage_context_t out_ctx = {
        .memory_size = ctx->mem.memory_size,
        .memory = fresh,
        .write_index = 0,
    };
    const ctap_storage_t out = CTAP_MEMORY_STORAGE_CONST_INIT(&out_ctx);
    ctap_storage_item_t item = {.handle = 0, .key = 0};
    while (ctap_memory_storage_find_item(storage, &item) == CTAP_STORAGE_OK) {
        ctap_storage_item_t copy = {.handle = 0, .key = item.key, .size = item.size,
                                    .data = item.data};
        ctap_memory_storage_create_or_update_item(&out, &copy);
        item.key = 0;  // keep iterating over all keys
    }
    memcpy(ctx->mem.memory, fresh, ctx->mem.memory_size);
    ctx->mem.write_index = out_ctx.write_index;
    free(fresh);
    return true;
}

ctap_storage_status_t ctap_file_storage_init(const ctap_storage_t *storage) {
    ctap_file_storage_context_t *ctx = ctx_of(storage);
    memset(ctx->mem.memory, 0, ctx->mem.memory_size);
    ctx->mem.write_index = 0;
    size_t loaded = 0;
    if (ctx->path != NULL) {
        FILE *f = fopen(ctx->path, "rb");
        if (f != NULL) {
            loaded = fread(ctx->mem.memory, 1, ctx->mem.memory_size, f);
            fclose(f);
        } else if (errno != ENOENT) {
            ESP_LOGE(TAG, "cannot read %s: errno %d", ctx->path, errno);
            return CTAP_STORAGE_ERROR;
        }
    } else {
        ESP_LOGW(TAG, "no store file: credentials are lost on reboot");
    }
    ctx->mem.write_index = loaded;  // find_item only looks at [0, write_index)
    if (!compact(storage)) {
        return CTAP_STORAGE_ERROR;
    }
    ESP_LOGI(TAG, "%s: %u bytes, %u live", ctx->path ? ctx->path : "RAM", (unsigned)loaded,
             (unsigned)ctx->mem.write_index);
    return ctx->mem.write_index != loaded ? save(storage) : CTAP_STORAGE_OK;
}

static size_t count_credentials(const ctap_storage_t *storage) {
    size_t n = 0;
    ctap_storage_item_t item = {.handle = 0, .key = CTAP_STORAGE_KEY_CREDENTIAL};
    while (ctap_memory_storage_find_item(storage, &item) == CTAP_STORAGE_OK) {
        ++n;
    }
    return n;
}

ctap_storage_status_t ctap_file_storage_create_or_update_item(const ctap_storage_t *storage,
                                                              ctap_storage_item_t *item) {
    if (item->key == CTAP_STORAGE_KEY_CREDENTIAL && item->handle == 0 &&
        count_credentials(storage) >= ctx_of(storage)->max_credentials) {
        return CTAP_STORAGE_OUT_OF_MEMORY_ERROR;  // a new one, not an overwrite
    }
    const ctap_storage_status_t st = ctap_memory_storage_create_or_update_item(storage, item);
    return st == CTAP_STORAGE_OK ? save(storage) : st;
}

ctap_storage_status_t ctap_file_storage_delete_item(const ctap_storage_t *storage,
                                                    uint32_t item_handle) {
    const ctap_storage_status_t st = ctap_memory_storage_delete_item(storage, item_handle);
    return st == CTAP_STORAGE_OK ? save(storage) : st;
}

ctap_storage_status_t ctap_file_storage_increment_counter(const ctap_storage_t *storage,
                                                          uint32_t increment,
                                                          uint32_t *counter_new_value) {
    (void)storage;
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) {
        return CTAP_STORAGE_ERROR;
    }
    uint32_t value = 0;
    nvs_get_u32(h, kNvsCounter, &value);  // missing: starts at 0
    value += increment;
    esp_err_t err = nvs_set_u32(h, kNvsCounter, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return CTAP_STORAGE_ERROR;
    }
    *counter_new_value = value;
    return CTAP_STORAGE_OK;
}

size_t ctap_file_storage_estimate_num_remaining_items(const ctap_storage_t *storage,
                                                     const ctap_storage_item_t *item) {
    size_t n = ctap_memory_storage_estimate_num_remaining_items(storage, item);
    if (item->key == CTAP_STORAGE_KEY_CREDENTIAL) {
        const size_t max = ctx_of(storage)->max_credentials;
        const size_t used = count_credentials(storage);
        const size_t left = used < max ? max - used : 0;
        n = n < left ? n : left;
    }
    return n;
}

// authenticatorReset. The signature counter is kept: it only has to grow.
ctap_storage_status_t ctap_file_storage_erase(const ctap_storage_t *storage) {
    ctap_file_storage_context_t *ctx = ctx_of(storage);
    ctap_memory_storage_erase(storage);
    if (ctx->path != NULL) {
        remove(ctx->path);
    }
    return CTAP_STORAGE_OK;
}
