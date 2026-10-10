// LionKey crypto table on the crypto driver (hardware RNG, SHA-256, AES).
// P-256 stays on lionkey's micro-ecc: the ESP32-S3 has no ECC accelerator.
// Streaming SHA-256 for HMAC/HKDF (sha256_bind_ctx, hash_alg_sha256) stays
// lionkey's software one.

#include "crypto.h"

extern "C" {
#include "ctap_crypto_software.h"
}

namespace {

ctap_crypto_status_t Status(esp_err_t err) {
    return err == ESP_OK ? CTAP_CRYPTO_OK : CTAP_CRYPTO_ERROR;
}

ctap_crypto_status_t Init(const ctap_crypto_t *, uint32_t) { return Status(crypto::Init()); }

ctap_crypto_status_t RngInit(const ctap_crypto_t *, uint32_t) { return CTAP_CRYPTO_OK; }

ctap_crypto_status_t RngGenerate(const ctap_crypto_t *, uint8_t *buffer, size_t length) {
    crypto::Random(buffer, length);
    return CTAP_CRYPTO_OK;
}

ctap_crypto_status_t AesEncrypt(const ctap_crypto_t *, const uint8_t *iv, const uint8_t *key,
                                uint8_t *data, size_t length) {
    return Status(crypto::Aes256CbcEncrypt(key, iv, data, length));
}

ctap_crypto_status_t AesDecrypt(const ctap_crypto_t *, const uint8_t *iv, const uint8_t *key,
                                uint8_t *data, size_t length) {
    return Status(crypto::Aes256CbcDecrypt(key, iv, data, length));
}

ctap_crypto_status_t Sha256(const ctap_crypto_t *, const uint8_t *data, size_t length,
                            uint8_t *hash) {
    return Status(crypto::Sha256(data, length, hash));
}

ctap_software_crypto_context_t software_ctx;

}  // namespace

extern "C" const ctap_crypto_t fido_ctap_crypto = {
    .context = &software_ctx,
    .init = Init,
    .rng_init = RngInit,
    .rng_generate_data = RngGenerate,
    .ecc_secp256r1_compute_public_key = ctap_software_crypto_ecc_secp256r1_compute_public_key,
    .ecc_secp256r1_sign = ctap_software_crypto_ecc_secp256r1_sign,
    .ecc_secp256r1_shared_secret = ctap_software_crypto_ecc_secp256r1_shared_secret,
    .aes_256_cbc_encrypt = AesEncrypt,
    .aes_256_cbc_decrypt = AesDecrypt,
    .sha256_bind_ctx = ctap_software_crypto_sha256_bind_ctx,
    .sha256_compute_digest = Sha256,
    .sha256 = &hash_alg_sha256,
};
