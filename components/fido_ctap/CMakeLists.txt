# LionKey CTAP 2.1 core (lib/lionkey, git submodule, used as is) with the
# sources its own CMake would build: software crypto and TinyCBOR.
set(lk ${CMAKE_CURRENT_LIST_DIR}/../../lib/lionkey)
set(cbor ${lk}/lib/tinycbor/src)

set(lionkey_srcs
    ${lk}/core/ctap.c
    ${lk}/core/ctap_asn1.c
    ${lk}/core/ctap_credential_management.c
    ${lk}/core/ctap_credential_store.c
    ${lk}/core/ctap_credentials.c
    ${lk}/core/ctap_crypto_software.c
    ${lk}/core/ctap_encode.c
    ${lk}/core/ctap_get_info.c
    ${lk}/core/ctap_memory_storage.c
    ${lk}/core/ctap_parse.c
    ${lk}/core/ctap_pin.c
    ${lk}/core/ctap_pin_protocol.c
    ${lk}/core/ctap_pin_uv_auth_token.c
    ${lk}/core/ctap_reset.c
    ${lk}/core/ctap_selection.c
    ${lk}/core/ctap_string.c
    ${lk}/core/utils.c
    ${lk}/crypto/hkdf/hkdf.c
    ${lk}/crypto/hmac/hmac.c
    ${lk}/crypto/sha256/sha256.c
    ${lk}/crypto/micro-ecc/uECC.c
    ${lk}/crypto/tiny-AES-c/aes.c
    ${lk}/crypto/tinymt/tinymt/tinymt32.c
    ${cbor}/cborencoder.c
    ${cbor}/cborencoder_close_container_checked.c
    ${cbor}/cborerrorstrings.c
    ${cbor}/cborparser.c
    ${cbor}/cborvalidation.c)

idf_component_register(SRCS "fido_ctap.c" "ctap_file_storage.c" "ctap_crypto_esp.cpp" ${lionkey_srcs}
                    INCLUDE_DIRS "."
                    PRIV_INCLUDE_DIRS "tinycbor"
                                      ${lk}/core
                                      ${lk}/crypto/hash
                                      ${lk}/crypto/hkdf
                                      ${lk}/crypto/hmac
                                      ${lk}/crypto/sha256
                                      ${lk}/crypto/micro-ecc
                                      ${lk}/crypto/tiny-AES-c
                                      ${lk}/crypto/tinymt/tinymt
                                      ${cbor}
                    PRIV_REQUIRES crypto esp_timer nvs_flash)

# Same settings as lionkey's own build (test/CMakeLists.txt).
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LIONKEY_DEBUG_LEVEL=1  # errors only, asserts off
    CBOR_PARSER_MAX_RECURSIONS=4
    CBOR_NO_FLOATING_POINT
    TINYAES_ENABLE_AES256=1 TINYAES_ENABLE_CBC=1 TINYAES_ENABLE_ECB=0 TINYAES_ENABLE_CTR=0
    uECC_OPTIMIZATION_LEVEL=4 uECC_SQUARE_FUNC=1 uECC_SUPPORT_COMPRESSED_POINT=0)
# NimBLE's tinycrypt exports the same uECC_* names: give micro-ecc its own.
foreach(sym uECC_compress uECC_compute_public_key uECC_curve_private_key_size
            uECC_curve_public_key_size uECC_decompress uECC_get_rng uECC_make_key
            uECC_point_mult uECC_secp160r1 uECC_secp192r1 uECC_secp224r1 uECC_secp256k1
            uECC_secp256r1 uECC_set_rng uECC_shared_secret uECC_sign uECC_sign_deterministic
            uECC_sign_with_k uECC_valid_public_key uECC_verify)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${sym}=lk_${sym})
endforeach()
# Third-party code: do not fail the build on its warnings.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-variable
                       -Wno-unused-function -Wno-missing-field-initializers)
