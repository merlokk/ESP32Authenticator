// What vault.cpp and store_io.cpp need besides files: NVS in memory, the
// uptime clock, and crypto stand-ins. The crypto is NOT cryptography: a
// deterministic mix with the same shapes (a wrong key gives another key id,
// a changed byte fails the tag), enough for the store's logic.

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "crypto.h"
#include "esp_timer.h"
#include "nvs.h"

namespace fake {

int64_t now_us = 0;

namespace {

std::map<std::string, int64_t> g_nvs;  // "namespace/key" -> value
std::vector<std::string> g_handles;     // handle - 1 -> namespace

std::string NvsKey(nvs_handle_t handle, const char *key) {
    return g_handles.at(handle - 1) + "/" + key;
}

template <typename T>
esp_err_t NvsGet(nvs_handle_t handle, const char *key, T *out) {
    const auto it = g_nvs.find(NvsKey(handle, key));
    if (it == g_nvs.end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out = static_cast<T>(it->second);
    return ESP_OK;
}

esp_err_t NvsSet(nvs_handle_t handle, const char *key, int64_t value) {
    g_nvs[NvsKey(handle, key)] = value;
    return ESP_OK;
}

// FNV-1a over `data`, from `h`.
uint64_t Mix(uint64_t h, const void *data, size_t len) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

constexpr uint64_t kBasis = 0xcbf29ce484222325ULL;

// `len` bytes from the state `h`.
void Expand(uint64_t h, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        h = Mix(h, &i, sizeof i);
        out[i] = static_cast<uint8_t>((h ^ (h >> 29)) >> 32);
    }
}

uint64_t Stream(const uint8_t *key, const uint8_t *nonce) {
    return Mix(Mix(kBasis, key, crypto::kAes256KeySize), nonce, crypto::kGcmNonceSize);
}

void Tag(uint64_t stream, const uint8_t *aad, size_t aad_len, const uint8_t *cipher, size_t len,
         uint8_t out[crypto::kGcmTagSize]) {
    Expand(Mix(Mix(stream, aad, aad_len), cipher, len), out, crypto::kGcmTagSize);
}

void Xor(uint64_t stream, const uint8_t *in, size_t len, uint8_t *out) {
    for (size_t i = 0; i < len; ++i) {
        uint8_t k;
        Expand(Mix(stream, &i, sizeof i), &k, 1);
        out[i] = in[i] ^ k;
    }
}

}  // namespace

void NvsClear() { g_nvs.clear(); }

}  // namespace fake

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out) {
    (void)mode;
    fake::g_handles.push_back(name);
    *out = static_cast<nvs_handle_t>(fake::g_handles.size());
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }
esp_err_t nvs_commit(nvs_handle_t handle) {
    (void)handle;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
    return fake::g_nvs.erase(fake::NvsKey(handle, key)) != 0 ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out) {
    return fake::NvsGet(handle, key, out);
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value) {
    return fake::NvsSet(handle, key, value);
}
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out) {
    return fake::NvsGet(handle, key, out);
}
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value) {
    return fake::NvsSet(handle, key, value);
}
esp_err_t nvs_get_i64(nvs_handle_t handle, const char *key, int64_t *out) {
    return fake::NvsGet(handle, key, out);
}
esp_err_t nvs_set_i64(nvs_handle_t handle, const char *key, int64_t value) {
    return fake::NvsSet(handle, key, value);
}

namespace crypto {

void Random(uint8_t *out, size_t len) {
    static uint64_t counter = 0;
    ++counter;
    fake::Expand(fake::Mix(fake::kBasis, &counter, sizeof counter), out, len);
}

esp_err_t Sha256(const uint8_t *data, size_t len, uint8_t out[kSha256Size]) {
    fake::Expand(fake::Mix(fake::kBasis, data, len), out, kSha256Size);
    return ESP_OK;
}

esp_err_t Pbkdf2Sha256(const uint8_t *password, size_t password_len, const uint8_t *salt,
                       size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len) {
    uint64_t h = fake::Mix(fake::kBasis, password, password_len);
    h = fake::Mix(h, salt, salt_len);
    fake::Expand(fake::Mix(h, &iterations, sizeof iterations), out, out_len);
    return ESP_OK;
}

esp_err_t Aes256GcmSeal(const uint8_t key[kAes256KeySize], const uint8_t nonce[kGcmNonceSize],
                        const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t len,
                        uint8_t *out) {
    const uint64_t stream = fake::Stream(key, nonce);
    fake::Xor(stream, in, len, out);
    fake::Tag(stream, aad, aad_len, out, len, out + len);
    return ESP_OK;
}

esp_err_t Aes256GcmOpen(const uint8_t key[kAes256KeySize], const uint8_t nonce[kGcmNonceSize],
                        const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t len,
                        uint8_t *out) {
    const uint64_t stream = fake::Stream(key, nonce);
    uint8_t tag[kGcmTagSize];
    fake::Tag(stream, aad, aad_len, in, len, tag);
    if (std::memcmp(tag, in + len, sizeof tag) != 0) {
        return ESP_ERR_INVALID_CRC;
    }
    fake::Xor(stream, in, len, out);
    return ESP_OK;
}

}  // namespace crypto
