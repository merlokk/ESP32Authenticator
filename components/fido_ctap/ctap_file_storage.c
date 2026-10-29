// LionKey storage on ESP-IDF: lionkey's RAM log (ctap_memory_storage, an
// append-only list of items, deleted ones only flagged) is the working copy;
// the file is a store header (store_header.h: version, modification time)
// followed by the log's used part [0, write_index), AES-256-GCM with the
// vault's data key (the header is the associated data, a random nonce per
// write), so it is loaded only once the PIN is in. A plain file is refused.
//
// - Every change rewrites the file through a temp file + rename, so a power
//   loss leaves either the old or the new store.
// - Rollback: every write takes a version above the file's and NVS's, and
//   NVS (`fido/store_ver`) keeps the highest one accepted, written after the
//   file. A file below it (an old copy put back) is refused and left as is;
//   so is no file at all once a version is kept.
// - The file holds only the live items: every write seals a compacted copy
//   of the log, so a deleted credential never reaches the file (nor the sync
//   server), and loading it changes nothing (no rewrite: no new version that
//   synced devices would race to upload). The log itself drops deleted items
//   (compaction) only at init: later the core keeps item handles (offsets),
//   which compaction would move.
// - The global signature counter lives in NVS: otherwise every assertion
//   would append an item to the log.
// - At most max_credentials credentials (lionkey stores non-discoverable ones
//   too); creating one more fails with KEY_STORE_FULL.

#include "ctap_file_storage.h"

#include <stddef.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "ctap_credentials_store.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nvs.h"
#include "store_header_c.h"

static const char *TAG = "ctap_store";
_Static_assert(sizeof(((ctap_file_storage_context_t *)0)->store_id) == STORE_STORE_ID_SIZE,
               "the store id of store_header_c.h");

static const char *kNvsNamespace = "fido";
static const char *kNvsCounter = "sign_count";
static const char *kNvsVersion = "store_ver";

// The store cipher (ctap_crypto_esp.cpp, crypto driver).
bool fido_store_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
                     size_t aad_len, const uint8_t *in, size_t len, uint8_t *out);
int fido_store_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
                    size_t aad_len, const uint8_t *in, size_t len, uint8_t *out);
void fido_store_random(uint8_t *out, size_t len);
// 2024-01-01: an earlier clock was never set (rtc::Believable).
static const time_t kBelievableTime = 1704067200;

static ctap_file_storage_context_t *ctx_of(const ctap_storage_t *storage) {
    return (ctap_file_storage_context_t *)storage->context;
}

static bool nvs_version(uint32_t *out) {
    *out = 0;
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    const bool found = nvs_get_u32(h, kNvsVersion, out) == ESP_OK;
    nvs_close(h);
    return found;
}

// Written only when it grows (NVS wear: storage-encryption.md).
static void keep_version(uint32_t version) {
    uint32_t kept;
    if (nvs_version(&kept) && kept >= version) {
        return;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u32(h, kNvsVersion, version);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        // The file is newer than NVS: it is still accepted at the next boot.
        ESP_LOGE(TAG, "store version %u not kept: %s", (unsigned)version, esp_err_to_name(err));
    }
}

// Scratch for the ciphertext: PSRAM, internal RAM only if full.
static uint8_t *scratch(size_t size) {
    uint8_t *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p != NULL ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

// The file saves write next to it first.
static void tmp_path(const char *path, char *out, size_t size) {
    snprintf(out, size, "%s.tmp", path);
}

uint32_t ctap_file_storage_nvs_version(void) {
    uint32_t v;
    nvs_version(&v);
    return v;
}

// Exactly `version`, also below the kept one (another store taken by the sync).
static void set_version(uint32_t version) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u32(h, kNvsVersion, version);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "store version %u not set: %s", (unsigned)version, esp_err_to_name(err));
    }
}

// A zeroed buffer of the log's size: PSRAM, like the RAM log itself
// (fido_ctap.c); internal RAM only if full. NULL: out of memory.
static uint8_t *log_buffer(size_t size) {
    uint8_t *b = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return b != NULL ? b : heap_caps_calloc(1, size, MALLOC_CAP_8BIT);
}

// The live items of `mem`, rebuilt from the start of `out` (mem->memory_size
// bytes, zeroed): their length.
static size_t live_items(ctap_memory_storage_context_t *mem, uint8_t *out) {
    const ctap_storage_t in = CTAP_MEMORY_STORAGE_CONST_INIT(mem);
    ctap_memory_storage_context_t out_ctx = {
        .memory_size = mem->memory_size,
        .memory = out,
        .write_index = 0,
    };
    const ctap_storage_t dst = CTAP_MEMORY_STORAGE_CONST_INIT(&out_ctx);
    ctap_storage_item_t item = {.handle = 0, .key = 0};
    while (ctap_memory_storage_find_item(&in, &item) == CTAP_STORAGE_OK) {
        ctap_storage_item_t copy = {.handle = 0, .key = item.key, .size = item.size,
                                    .data = item.data};
        ctap_memory_storage_create_or_update_item(&dst, &copy);
        item.key = 0;  // keep iterating over all keys
    }
    return out_ctx.write_index;
}

// The RAM log's live items, sealed with `key`, into `out_path`. `changed`:
// the credentials or PIN changed (the modification time moves); false: only
// the layout (compaction) or the key. *header: what was written.
static ctap_storage_status_t write_file(ctap_file_storage_context_t *ctx,
                                        const fido_ctap_key_t *key, const char *out_path,
                                        bool changed, store_header_t *header) {
    uint8_t *live = log_buffer(ctx->mem.memory_size);
    if (live == NULL) {
        ESP_LOGE(TAG, "out of memory: %s not written", ctx->path);
        return CTAP_STORAGE_ERROR;
    }
    const size_t n = live_items(&ctx->mem, live);
    uint32_t kept;
    nvs_version(&kept);
    *header = (store_header_t){
        .cipher = 1,
        .version = store_header_next_version(ctx->version, kept),
        .length = (uint32_t)n,
        .modified_utc = ctx->modified_utc,
        .kdf_iterations = key->iterations,
    };
    memcpy(header->salt, key->salt, sizeof header->salt);
    memcpy(header->key_id, key->id, sizeof header->key_id);
    memcpy(header->seed_id, key->seed_id, sizeof header->seed_id);
    static const uint8_t kNoId[STORE_STORE_ID_SIZE] = {0};
    if (memcmp(ctx->store_id, kNoId, sizeof kNoId) == 0) {
        fido_store_random(ctx->store_id, sizeof ctx->store_id);
    }
    memcpy(header->store_id, ctx->store_id, sizeof header->store_id);
    if (changed) {
        const time_t now = time(NULL);
        header->modified_utc = now >= kBelievableTime ? (int64_t)now : 0;
    }
    fido_store_random(header->nonce, sizeof header->nonce);
    uint8_t head[STORE_HEADER_SIZE];
    store_header_encode(STORE_KIND_FIDO, header, head);
    uint8_t *sealed = scratch(n + STORE_TAG_SIZE);
    const bool sealed_ok =
        sealed != NULL && fido_store_seal(key->key, header->nonce, head, sizeof head, live, n, sealed);
    memset(live, 0, ctx->mem.memory_size);  // private keys
    heap_caps_free(live);
    if (!sealed_ok) {
        ESP_LOGE(TAG, "encrypting %s failed", ctx->path);
        heap_caps_free(sealed);
        return CTAP_STORAGE_ERROR;
    }
    FILE *f = fopen(out_path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "cannot write %s", out_path);
        heap_caps_free(sealed);
        return CTAP_STORAGE_ERROR;
    }
    const bool ok = fwrite(head, 1, sizeof head, f) == sizeof head &&
                    fwrite(sealed, 1, n + STORE_TAG_SIZE, f) == n + STORE_TAG_SIZE;
    heap_caps_free(sealed);
    if (fclose(f) != 0 || !ok) {
        ESP_LOGE(TAG, "write %s failed", out_path);
        remove(out_path);
        return CTAP_STORAGE_ERROR;
    }
    return CTAP_STORAGE_OK;
}

// `from` over the store file; the written header becomes the current one.
static ctap_storage_status_t install(ctap_file_storage_context_t *ctx, const char *from,
                                     const store_header_t *header) {
    remove(ctx->path);  // SPIFFS rename does not replace
    if (rename(from, ctx->path) != 0) {
        ESP_LOGE(TAG, "rename %s failed", from);
        return CTAP_STORAGE_ERROR;
    }
    ctx->version = header->version;
    ctx->modified_utc = header->modified_utc;
    keep_version(header->version);  // after the file: a power loss between is a newer file
    return CTAP_STORAGE_OK;
}

// Writes the file with the next version through <file>.tmp.
static ctap_storage_status_t save(const ctap_storage_t *storage, bool changed) {
    ctap_file_storage_context_t *ctx = ctx_of(storage);
    if (ctx->path == NULL) {
        return CTAP_STORAGE_OK;
    }
    if (!ctx->has_key) {
        ESP_LOGE(TAG, "no key: %s not written", ctx->path);
        return CTAP_STORAGE_ERROR;
    }
    char tmp[64];
    tmp_path(ctx->path, tmp, sizeof tmp);
    store_header_t header;
    const ctap_storage_status_t st = write_file(ctx, &ctx->key, tmp, changed, &header);
    return st == CTAP_STORAGE_OK ? install(ctx, tmp, &header) : st;
}

static void new_path(const char *path, char *out, size_t size) {
    snprintf(out, size, "%s.new", path);
}

static store_header_t staged_header;

ctap_storage_status_t ctap_file_storage_stage(const ctap_storage_t *storage) {
    ctap_file_storage_context_t *ctx = ctx_of(storage);
    char path[64];
    new_path(ctx->path, path, sizeof path);
    return write_file(ctx, &ctx->staged, path, false, &staged_header);
}

ctap_storage_status_t ctap_file_storage_commit(const ctap_storage_t *storage) {
    ctap_file_storage_context_t *ctx = ctx_of(storage);
    char path[64];
    new_path(ctx->path, path, sizeof path);
    const ctap_storage_status_t st = install(ctx, path, &staged_header);
    if (st == CTAP_STORAGE_OK) {
        ctx->key = ctx->staged;
    }
    memset(&ctx->staged, 0, sizeof ctx->staged);
    ctx->has_staged = false;
    return st;
}

void ctap_file_storage_unstage(const ctap_storage_t *storage) {
    ctap_file_storage_context_t *ctx = ctx_of(storage);
    char path[64];
    new_path(ctx->path, path, sizeof path);
    remove(path);
    memset(&ctx->staged, 0, sizeof ctx->staged);
    ctx->has_staged = false;
}

ctap_storage_status_t ctap_file_storage_install_copy(const ctap_storage_t *storage,
                                                    const char *from, bool force) {
    ctap_file_storage_context_t *ctx = ctx_of(storage);
    if (ctx->path == NULL) {
        return CTAP_STORAGE_ERROR;
    }
    if (force) {
        struct stat st;
        FILE *f = stat(from, &st) == 0 ? fopen(from, "rb") : NULL;
        if (f == NULL) {
            return CTAP_STORAGE_ERROR;
        }
        uint8_t head[STORE_HEADER_SIZE];
        const size_t got = fread(head, 1, sizeof head, f);
        fclose(f);
        store_header_t header = {0};
        if (!store_header_decode(head, got, (size_t)st.st_size, STORE_KIND_FIDO, &header)) {
            return CTAP_STORAGE_ERROR;
        }
        set_version(header.version);  // before the rename: a power loss keeps the old file
    }
    remove(ctx->path);  // SPIFFS rename does not replace
    if (rename(from, ctx->path) != 0) {
        ESP_LOGE(TAG, "rename %s failed", from);
        return CTAP_STORAGE_ERROR;
    }
    return CTAP_STORAGE_OK;
}

ctap_storage_status_t ctap_file_storage_rewrite(const ctap_storage_t *storage,
                                               uint32_t min_version) {
    ctap_file_storage_context_t *ctx = ctx_of(storage);
    if (min_version > 0 && ctx->version < min_version - 1) {
        ctx->version = min_version - 1;
    }
    return save(storage, false);
}

// Rebuilds the log from its live items, which also sets write_index (lionkey's
// ctap_memory_storage_init() asserts on a non-empty memory, so it is not
// used). Returns false if out of memory.
static bool compact(const ctap_storage_t *storage) {
    ctap_file_storage_context_t *ctx = ctx_of(storage);
    uint8_t *fresh = log_buffer(ctx->mem.memory_size);
    if (fresh == NULL) {
        return false;
    }
    const size_t n = live_items(&ctx->mem, fresh);
    memcpy(ctx->mem.memory, fresh, ctx->mem.memory_size);
    ctx->mem.write_index = n;
    memset(fresh, 0, ctx->mem.memory_size);  // private keys
    heap_caps_free(fresh);
    return true;
}

// A save is: write .tmp, remove the file, rename .tmp. A power loss leaves
// either a partial .tmp next to the file (dropped) or a complete .tmp and no
// file (taken: the remove only follows a good write).
static void recover_tmp(const char *path) {
    char tmp[64];
    tmp_path(path, tmp, sizeof tmp);
    struct stat st;
    if (stat(tmp, &st) != 0) {
        return;
    }
    if (stat(path, &st) == 0) {
        ESP_LOGW(TAG, "%s: an unfinished save, dropped", tmp);
        remove(tmp);
    } else if (rename(tmp, path) == 0) {
        ESP_LOGW(TAG, "%s: a save cut before its rename, taken", tmp);
    } else {
        ESP_LOGE(TAG, "rename %s failed", tmp);
    }
}

// Reads the file into the RAM log. Returns false (with ctx->refused) if it
// must not be loaded; *rewrite: no file yet, write an empty one.
static bool load(ctap_file_storage_context_t *ctx, bool *rewrite) {
    *rewrite = false;
    recover_tmp(ctx->path);
    struct stat st;
    if (stat(ctx->path, &st) != 0) {
        if (errno == ENOENT) {
            // No file and no version: the first boot, an empty store. With a
            // version kept, the file was deleted (or SPIFFS formatted): refused
            // like a rollback, since an old copy would be taken after an empty
            // one; `fido2 reset` starts an empty store above that version.
            uint32_t kept;
            if (nvs_version(&kept)) {
                ESP_LOGE(TAG, "%s missing, version %u was seen", ctx->path, (unsigned)kept);
                ctx->refused = "the store file is missing";
                return false;
            }
            *rewrite = true;
            return true;
        }
        ESP_LOGE(TAG, "cannot stat %s: errno %d", ctx->path, errno);
        ctx->refused = "the store file cannot be read";
        return false;
    }
    FILE *f = fopen(ctx->path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "cannot read %s: errno %d", ctx->path, errno);
        ctx->refused = "the store file cannot be read";
        return false;
    }
    const size_t file_size = (size_t)st.st_size;
    uint8_t head[STORE_HEADER_SIZE];
    const size_t got = fread(head, 1, sizeof head, f);
    store_header_t header = {0};
    uint32_t kept;
    const bool has_kept = nvs_version(&kept);
    bool ok = false;
    if (!store_header_decode(head, got, file_size, STORE_KIND_FIDO, &header)) {
        ctx->refused = "the store file is damaged or of an unknown format";
    } else if (store_header_rollback(header.version, has_kept, kept)) {
        ESP_LOGE(TAG, "%s: version %u, but %u was seen", ctx->path, (unsigned)header.version,
                 (unsigned)kept);
        ctx->refused = "the store file is older than one this device has used (rollback)";
    } else if (header.cipher == 0) {
        ctx->refused = "the store file is not encrypted";
    } else if (header.length > ctx->mem.memory_size) {
        // More than fits: the limit was lowered. Truncating would lose
        // credentials on the next save, so the file is left as it is.
        ESP_LOGE(TAG, "%s: %u bytes, larger than the %u-byte store", ctx->path,
                 (unsigned)header.length, (unsigned)ctx->mem.memory_size);
        ctx->refused = "the store file is larger than the store (credential limit lowered)";
    } else if (!ctx->has_key) {
        ctx->refused = "locked: enter the PIN";
    } else if (memcmp(header.seed_id, ctx->key.seed_id, sizeof header.seed_id) != 0) {
        ctx->refused = "the store file has another seed phrase";
    } else if (memcmp(header.key_id, ctx->key.id, sizeof header.key_id) != 0) {
        ctx->refused = "the store file has another key (another PIN, or not yet re-keyed)";
    } else {
        const size_t sealed_size = header.length + STORE_TAG_SIZE;
        uint8_t *sealed = scratch(sealed_size);
        const bool read = sealed != NULL && fread(sealed, 1, sealed_size, f) == sealed_size;
        const int opened = read ? fido_store_open(ctx->key.key, header.nonce, head, sizeof head,
                                                  sealed, header.length, ctx->mem.memory)
                                : -1;
        heap_caps_free(sealed);
        ok = opened == 0;
        if (!ok) {
            ctx->refused = opened == 1 ? "the store file does not decrypt with this key (other "
                                         "key, or damaged)"
                                       : "the store file cannot be read";
        }
    }
    fclose(f);
    if (!ok) {
        ESP_LOGE(TAG, "%s not loaded: %s", ctx->path, ctx->refused);
        memset(ctx->mem.memory, 0, ctx->mem.memory_size);
        return false;
    }
    ctx->mem.write_index = header.length;  // find_item only looks at [0, write_index)
    ctx->version = header.version;
    ctx->modified_utc = header.modified_utc;
    memcpy(ctx->store_id, header.store_id, sizeof ctx->store_id);
    keep_version(header.version);  // above NVS: written before a power loss, or synced
    return true;
}

ctap_storage_status_t ctap_file_storage_init(const ctap_storage_t *storage) {
    ctap_file_storage_context_t *ctx = ctx_of(storage);
    memset(ctx->mem.memory, 0, ctx->mem.memory_size);
    ctx->mem.write_index = 0;
    ctx->version = 0;
    ctx->modified_utc = 0;
    memset(ctx->store_id, 0, sizeof ctx->store_id);
    ctx->refused = NULL;
    bool rewrite = false;
    if (ctx->path == NULL) {
        ESP_LOGW(TAG, "no store file: credentials are lost on reboot");
    } else if (!load(ctx, &rewrite)) {
        return CTAP_STORAGE_ERROR;
    }
    const size_t loaded = ctx->mem.write_index;
    if (!compact(storage)) {
        return CTAP_STORAGE_ERROR;
    }
    ESP_LOGI(TAG, "%s: version %u, %u bytes, %u live", ctx->path ? ctx->path : "RAM",
             (unsigned)ctx->version, (unsigned)loaded, (unsigned)ctx->mem.write_index);
    if (rewrite) {
        return save(storage, true);
    }
    return ctx->mem.write_index != loaded ? save(storage, false) : CTAP_STORAGE_OK;
}

static size_t count_credentials(const ctap_storage_t *storage) {
    size_t n = 0;
    ctap_storage_item_t item = {.handle = 0, .key = CTAP_STORAGE_KEY_CREDENTIAL};
    while (ctap_memory_storage_find_item(storage, &item) == CTAP_STORAGE_OK) {
        ++n;
    }
    return n;
}

// --- origin items

#define ORIGIN_OLD_SIZE 40u  // id + host only

#define CREDENTIAL_ID_OFFSET offsetof(ctap_credential_source_t, id)

// The credential ID of a credential item's data, false if too short.
static bool id_of(const uint8_t *data, size_t size, uint8_t id[32]) {
    if (size < CREDENTIAL_ID_OFFSET + 32) {
        return false;
    }
    memcpy(id, data + CREDENTIAL_ID_OFFSET, 32);
    return true;
}

// The live item with this handle (any key), false if none.
static bool item_at(const ctap_storage_t *storage, uint32_t handle, ctap_storage_item_t *out) {
    ctap_storage_item_t item = {.handle = 0, .key = 0};
    while (ctap_memory_storage_find_item(storage, &item) == CTAP_STORAGE_OK) {
        if (item.handle == handle) {
            *out = item;
            return true;
        }
        item.key = 0;
    }
    return false;
}

static bool find_origin(const ctap_storage_t *storage, const uint8_t id[32],
                        ctap_storage_item_t *out) {
    ctap_storage_item_t item = {.handle = 0, .key = CTAP_FILE_STORAGE_KEY_ORIGIN};
    while (ctap_memory_storage_find_item(storage, &item) == CTAP_STORAGE_OK) {
        if (item.size >= ORIGIN_OLD_SIZE && memcmp(item.data, id, 32) == 0) {
            *out = item;
            return true;
        }
    }
    return false;
}

static void delete_origin(const ctap_storage_t *storage, const uint8_t id[32]) {
    ctap_storage_item_t item;
    if (find_origin(storage, id, &item)) {
        ctap_memory_storage_delete_item(storage, item.handle);
    }
}

// Records where and when credential `id` is made. Best effort: a missing
// record only shows as "unknown".
static void add_origin(const ctap_storage_t *storage, const uint8_t id[32]) {
    uint8_t record[CTAP_FILE_STORAGE_ORIGIN_SIZE];
    memset(record, 0, sizeof record);
    memcpy(record, id, 32);
    fido_ctap_client_t client;
    memset(&client, 0, sizeof client);
    bool (*get_client)(fido_ctap_client_t *) = ctx_of(storage)->client;
    if (get_client == NULL || !get_client(&client)) {
        memset(&client, 0, sizeof client);
    }
    record[32] = client.transport;
    record[33] = client.addr_type;
    memcpy(record + 34, client.addr, 6);
    const time_t now = time(NULL);
    const int64_t created = now >= kBelievableTime ? (int64_t)now : 0;
    for (int i = 0; i < 8; ++i) {
        record[40 + i] = (uint8_t)((uint64_t)created >> (8 * i));
    }
    esp_read_mac(record + 48, ESP_MAC_BASE);
    ctap_storage_item_t item = {.handle = 0,
                                .key = CTAP_FILE_STORAGE_KEY_ORIGIN,
                                .size = sizeof record,
                                .data = record};
    if (ctap_memory_storage_create_or_update_item(storage, &item) != CTAP_STORAGE_OK) {
        ESP_LOGW(TAG, "no room for the origin record");
    }
}

fido_ctap_origin_t ctap_file_storage_origin(const ctap_storage_t *storage, const uint8_t id[32]) {
    fido_ctap_origin_t origin;
    memset(&origin, 0, sizeof origin);
    ctap_storage_item_t item;
    if (!find_origin(storage, id, &item)) {
        return origin;
    }
    const uint8_t *d = item.data;
    origin.client.transport = d[32];
    origin.client.addr_type = d[33];
    memcpy(origin.client.addr, d + 34, 6);
    if (item.size >= CTAP_FILE_STORAGE_ORIGIN_SIZE) {
        uint64_t created = 0;
        for (int i = 0; i < 8; ++i) {
            created |= (uint64_t)d[40 + i] << (8 * i);
        }
        origin.created_utc = (int64_t)created;
        memcpy(origin.device, d + 48, 6);
        origin.has_device = true;
    }
    return origin;
}

// --- items

ctap_storage_status_t ctap_file_storage_create_or_update_item(const ctap_storage_t *storage,
                                                              ctap_storage_item_t *item) {
    const bool credential = item->key == CTAP_STORAGE_KEY_CREDENTIAL;
    if (credential && item->handle == 0 &&
        count_credentials(storage) >= ctx_of(storage)->max_credentials) {
        return CTAP_STORAGE_OUT_OF_MEMORY_ERROR;  // a new one, not an overwrite
    }
    // An overwrite with another ID is a new credential in the old slot.
    uint8_t old_id[32];
    ctap_storage_item_t old;
    const bool had_old = credential && item->handle != 0 && item_at(storage, item->handle, &old) &&
                         id_of(old.data, old.size, old_id);
    uint8_t new_id[32];
    const bool has_new = credential && id_of(item->data, item->size, new_id);
    const ctap_storage_status_t st = ctap_memory_storage_create_or_update_item(storage, item);
    if (st != CTAP_STORAGE_OK) {
        return st;
    }
    if (has_new && !(had_old && memcmp(old_id, new_id, 32) == 0)) {
        if (had_old) {
            delete_origin(storage, old_id);
        }
        add_origin(storage, new_id);
    }
    return save(storage, true);
}

ctap_storage_status_t ctap_file_storage_delete_item(const ctap_storage_t *storage,
                                                    uint32_t item_handle) {
    uint8_t id[32];
    ctap_storage_item_t item;
    const bool credential = item_at(storage, item_handle, &item) &&
                            item.key == CTAP_STORAGE_KEY_CREDENTIAL &&
                            id_of(item.data, item.size, id);
    const ctap_storage_status_t st = ctap_memory_storage_delete_item(storage, item_handle);
    if (st != CTAP_STORAGE_OK) {
        return st;
    }
    if (credential) {
        delete_origin(storage, id);
    }
    return save(storage, true);
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

uint32_t ctap_file_storage_counter(void) {
    nvs_handle_t h;
    uint32_t value = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, kNvsCounter, &value);
        nvs_close(h);
    }
    return value;
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

void ctap_file_storage_forget(const ctap_storage_t *storage) {
    ctap_file_storage_context_t *ctx = ctx_of(storage);
    ctap_memory_storage_erase(storage);
    if (ctx->path == NULL) {
        return;
    }
    char tmp[64];
    tmp_path(ctx->path, tmp, sizeof tmp);
    remove(tmp);
    new_path(ctx->path, tmp, sizeof tmp);
    remove(tmp);
    remove(ctx->path);
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, kNvsVersion);
        nvs_commit(h);
        nvs_close(h);
    }
    ctx->version = 0;
    ctx->modified_utc = 0;
    memset(ctx->store_id, 0, sizeof ctx->store_id);
}

// authenticatorReset. The signature counter is kept: it only has to grow.
ctap_storage_status_t ctap_file_storage_erase(const ctap_storage_t *storage) {
    ctap_file_storage_context_t *ctx = ctx_of(storage);
    ctap_memory_storage_erase(storage);
    ctx->refused = NULL;
    // An empty store with the next version, not no file: an old copy put
    // back after a reset is still refused.
    return save(storage, true);
}
