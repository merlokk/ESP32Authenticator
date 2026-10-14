#pragma once

#include <cstddef>

#include "esp_err.h"

// SPIFFS on the `spiffs` partition, mounted at kBasePath through the VFS, so
// files are used with plain stdio (`fopen("/spiffs/x", ...)`).
// Never formats on its own: on the X4 Pro the partition holds stock firmware
// data. Only an explicit Format() does.

namespace spiffs_fs {

constexpr const char *kBasePath = "/spiffs";
constexpr const char *kPartitionLabel = "spiffs";

esp_err_t Init();
bool Mounted();

// Erases the partition and creates an empty file system, then mounts it.
// Destroys all files (on the X4 Pro: the stock firmware's data).
esp_err_t Format();

// Total and used bytes of the mounted file system.
esp_err_t Info(size_t *total, size_t *used);

// Builds the full VFS path for `name` ("a.txt" or "/spiffs/a.txt").
// Returns false if it does not fit into `out`.
bool FullPath(const char *name, char *out, size_t out_size);

}  // namespace spiffs_fs
