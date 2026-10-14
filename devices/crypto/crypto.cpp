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

psa_key_id_t GcmKey(const uint8_t *key, bool encrypt) {
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, kAes256KeySize * 8);
    psa_set_key_usage_flags(&attr, encrypt ? PSA_KEY_USAGE_ENCRYPT : PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    psa_key_id_t id = 0;
    return Check(psa_import_key(&attr, key, kAes256KeySize, &id), "gcm key") == ESP_OK ? id : 0;
}

}  // namespace

esp_err_t Aes256GcmSeal(const uint8_t key[kAes256KeySize], const uint8_t nonce[kGcmNonceSize],
                        const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t len,
                        uint8_t *out) {
    const psa_key_id_t id = GcmKey(key, true);
    if (id == 0) {
        return ESP_FAIL;
    }
    size_t n = 0;
    esp_err_t err = Check(psa_aead_encrypt(id, PSA_ALG_GCM, nonce, kGcmNonceSize, aad, aad_len,
                                           in, len, out, len + kGcmTagSize, &n),
                          "gcm seal");
    if (err == ESP_OK && n != len + kGcmTagSize) {
        err = ESP_FAIL;
    }
    psa_destroy_key(id);
    return err;
}

esp_err_t Aes256GcmOpen(const uint8_t key[kAes256KeySize], const uint8_t nonce[kGcmNonceSize],
                        const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t len,
                        uint8_t *out) {
    const psa_key_id_t id = GcmKey(key, false);
    if (id == 0) {
        return ESP_FAIL;
    }
    size_t n = 0;
    const psa_status_t st = psa_aead_decrypt(id, PSA_ALG_GCM, nonce, kGcmNonceSize, aad, aad_len,
                                             in, len + kGcmTagSize, out, len, &n);
    psa_destroy_key(id);
    if (st == PSA_ERROR_INVALID_SIGNATURE) {
        memset(out, 0, len);
        return ESP_ERR_INVALID_CRC;
    }
    return Check(st, "gcm open") == ESP_OK && n == len ? ESP_OK : ESP_FAIL;
}

esp_err_t Init() { return Check(psa_crypto_init(), "init"); }

void Random(uint8_t *out, size_t len) { esp_fill_random(out, len); }

esp_err_t Sha256(const uint8_t *data, size_t len, uint8_t out[kSha256Size]) {
    size_t n = 0;
    return Check(psa_hash_compute(PSA_ALG_SHA_256, data, len, out, kSha256Size, &n), "sha256");
}

esp_err_t HmacSha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
                   uint8_t out[kSha1Size]) {
    if (key == nullptr || key_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, key_len * 8);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_1));
    psa_key_id_t id = 0;
    esp_err_t err = Check(psa_import_key(&attr, key, key_len, &id), "hmac key");
    if (err != ESP_OK) {
        return err;
    }
    size_t n = 0;
    err = Check(psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_1), data, len, out, kSha1Size, &n),
                "hmac-sha1");
    psa_destroy_key(id);
    return err == ESP_OK && n != kSha1Size ? ESP_FAIL : err;
}

esp_err_t Hotp(const uint8_t *key, size_t key_len, uint64_t counter, unsigned digits,
               uint32_t *code) {
    if (code == nullptr || digits < 6 || digits > kOtpMaxDigits) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t message[8];
    OtpMessage(counter, message);
    uint8_t mac[kSha1Size];
    const esp_err_t err = HmacSha1(key, key_len, message, sizeof message, mac);
    if (err == ESP_OK) {
        *code = OtpTruncate(mac, digits);
    }
    memset(mac, 0, sizeof mac);
    return err;
}

esp_err_t Totp(const uint8_t *key, size_t key_len, int64_t unix_seconds, uint32_t step,
               unsigned digits, uint32_t *code) {
    if (step == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return Hotp(key, key_len, OtpCounter(unix_seconds, step), digits, code);
}

esp_err_t TotpSelfTest() {
    static const uint8_t kKey[] = "12345678901234567890";  // RFC 6238 SHA-1 seed
    struct Vector {
        int64_t time;
        uint32_t code;
    };
    static const Vector kVectors[] = {
        {59, 94287082},         {1111111109, 7081804},  {1111111111, 14050471},
        {1234567890, 89005924}, {2000000000, 69279037}, {20000000000, 65353130},
    };
    for (const Vector &v : kVectors) {
        uint32_t code = 0;
        const esp_err_t err = Totp(kKey, 20, v.time, kTotpStep, 8, &code);
        if (err != ESP_OK) {
            return err;
        }
        if (code != v.code) {
            ESP_LOGE(TAG, "totp self-test: t=%lld gave %08lu", static_cast<long long>(v.time),
                     static_cast<unsigned long>(code));
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_OK;
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
