#pragma once

// Store files on a mounted file system (SPIFFS or the SD card): header
// (store_header.h) + body, the rollback check against NVS, and the save
// sequence. For C++ stores (vault); fido.bin has its own copy in C
// (ctap_file_storage.c).
//
// - Save: the next version (above the file's and NVS's) into the header, write
//   `<path>.tmp`, remove `<path>`, rename. NVS `<namespace>/store_ver` keeps
//   the highest version accepted, written after the file, only when it grows.
// - Load: a `.tmp` with no file is a save cut before its rename (taken); a
//   `.tmp` next to the file an unfinished one (dropped). A file below the kept
//   version, damaged, of another kind, or missing while a version is kept, is
//   refused and left as is.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "store_header.h"

namespace storefile {

struct File {
    const char *path;           // full VFS path, e.g. "/spiffs/vault.bin"
    Kind kind;
    const char *nvs_namespace;  // up to 15 characters
};

enum class Loaded {
    kOk,       // `header` and `body` filled
    kNew,      // no file and no version kept: start empty
    kRefused,  // see `why`; the file is left as it is
};

// Reads the body (everything after the header: payload, + tag when encrypted)
// into `body` (`capacity` bytes); `*body_size` = its size. `raw` gets the
// header as stored: the AES-GCM associated data.
Loaded Load(const File &file, uint8_t *body, size_t capacity, Header *header, size_t *body_size,
            uint8_t raw[kHeaderSize], const char **why);

// The version for the next write: above `current` (the loaded file's) and NVS.
// Set into the header before sealing: the header is authenticated.
uint32_t NextVersion(const File &file, uint32_t current);

// Writes `header` as given (kind set from `file`) and `body`.
esp_err_t Save(const File &file, const Header &header, const uint8_t *body, size_t body_size);

// --- Several files replaced together (a PIN change: a new key for all):
// each written to <path>.new (Stage), then the marker file (BeginCommit),
// then each .new renamed over its file (Promote, the version kept in NVS),
// then the marker removed (EndCommit). At boot, before any Load,
// FinishCommit: with the marker every .new is complete and promoted, without
// it they are dropped (the old files and the old key stand).
esp_err_t Stage(const File &file, const Header &header, const uint8_t *body, size_t body_size);
void Unstage(const File &file);
esp_err_t BeginCommit(const char *marker);
esp_err_t Promote(const File &file, uint32_t version);
void EndCommit(const char *marker);
void FinishCommit(const char *marker, const char *const *paths, size_t count);

// Removes the file, its .tmp and the kept version: as if never made (the
// next Load is kNew). For a wipe.
void Forget(const File &file);

// The highest version accepted (NVS), 0 if none.
uint32_t KeptVersion(const File &file);

// The file's size, 0 if none.
size_t FileSize(const File &file);

}  // namespace storefile
