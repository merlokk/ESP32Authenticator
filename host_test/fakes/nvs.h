#pragma once

// The subset of ESP-IDF's nvs.h the code under test uses: an in-memory store
// (fake::NvsClear empties it). Same names and values.

#include <cstdint>

#include "esp_err.h"

#define ESP_ERR_NVS_NOT_FOUND 0x1102

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out);
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value);
esp_err_t nvs_get_i64(nvs_handle_t handle, const char *key, int64_t *out);
esp_err_t nvs_set_i64(nvs_handle_t handle, const char *key, int64_t value);

namespace fake {
void NvsClear();
}  // namespace fake
