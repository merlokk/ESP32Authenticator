#include "crypto.h"

#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "esp_random.h"
#include "psa/crypto.h"

namespace crypto {

namespace {

constexpr const char *TAG = "crypto";

esp_err_t Check(psa_status_t status, const char *what) {
    if (status == PSA_SUCCESS) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "%s: psa error %d", what, static_cast<int>(status));
    return ESP_FAIL;
}

esp_err_t Aes256Cbc(bool encrypt, const uint8_t *key, const uint8_t *iv, uint8_t *data,
                    size_t len) {
    if (len % kAesBlockSize != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, kAes256KeySize * 8);
    psa_set_key_usage_flags(&attr, encrypt ? PSA_KEY_USAGE_ENCRYPT : PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_CBC_NO_PADDING);
    psa_key_id_t id = 0;
    esp_err_t err = Check(psa_import_key(&attr, key, kAes256KeySize, &id), "aes key");
    if (err != ESP_OK) {
        return err;
    }
    // Output to a separate buffer: PSA does not promise in-place operation.
    uint8_t *out = static_cast<uint8_t *>(malloc(len));
    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    size_t n = 0;
    size_t tail = 0;
    if (out == nullptr) {
        err = ESP_ERR_NO_MEM;
    } else if ((err = Check(encrypt ? psa_cipher_encrypt_setup(&op, id, PSA_ALG_CBC_NO_PADDING)
                                    : psa_cipher_decrypt_setup(&op, id, PSA_ALG_CBC_NO_PADDING),
                            "aes setup")) == ESP_OK &&
               (err = Check(psa_cipher_set_iv(&op, iv, kAesBlockSize), "aes iv")) == ESP_OK &&
               (err = Check(psa_cipher_update(&op, data, len, out, len, &n), "aes")) == ESP_OK &&
               (err = Check(psa_cipher_finish(&op, out + n, len - n, &tail), "aes finish")) ==
                   ESP_OK) {
        if (n + tail == len) {
            memcpy(data, out, len);
        } else {
            err = ESP_FAIL;
        }
    }
    psa_cipher_abort(&op);
    psa_destroy_key(id);
    if (out != nullptr) {
        memset(out, 0, len);
        free(out);
    }
    return err;
}

}  // namespace

esp_err_t Init() { return Check(psa_crypto_init(), "init"); }

void Random(uint8_t *out, size_t len) { esp_fill_random(out, len); }

esp_err_t Sha256(const uint8_t *data, size_t len, uint8_t out[kSha256Size]) {
    size_t n = 0;
    return Check(psa_hash_compute(PSA_ALG_SHA_256, data, len, out, kSha256Size, &n), "sha256");
}

esp_err_t Aes256CbcEncrypt(const uint8_t key[kAes256KeySize], const uint8_t iv[kAesBlockSize],
                           uint8_t *data, size_t len) {
    return Aes256Cbc(true, key, iv, data, len);
}

esp_err_t Aes256CbcDecrypt(const uint8_t key[kAes256KeySize], const uint8_t iv[kAesBlockSize],
                           uint8_t *data, size_t len) {
    return Aes256Cbc(false, key, iv, data, len);
}

}  // namespace crypto
