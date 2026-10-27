// The authentication server's pure side (components/server/server_logic.h).

#include <cstring>

#include "server_logic.h"
#include "unity.h"

namespace {

void test_server_state_round_trip(void) {
    server::State in = {};
    in.has_key = true;
    in.registered = true;
    for (size_t i = 0; i < server::kKeySize; ++i) {
        in.key[i] = static_cast<uint8_t>(i + 1);
    }
    for (size_t i = 0; i < server::kTotpSize; ++i) {
        in.totp[i] = static_cast<uint8_t>(0xA0 + i);
    }
    in.registered_at = 1761500000;
    in.last_sync = -2;  // any value goes through
    uint8_t bytes[server::kFileSize];
    server::Encode(in, bytes);
    TEST_ASSERT_EQUAL_MEMORY("SRV1", bytes, 4);
    TEST_ASSERT_EQUAL_HEX8(3, bytes[4]);
    server::State out = {};
    TEST_ASSERT_TRUE(server::Decode(bytes, sizeof bytes, &out));
    TEST_ASSERT_TRUE(out.has_key);
    TEST_ASSERT_TRUE(out.registered);
    TEST_ASSERT_EQUAL_MEMORY(in.key, out.key, server::kKeySize);
    TEST_ASSERT_EQUAL_MEMORY(in.totp, out.totp, server::kTotpSize);
    TEST_ASSERT_TRUE(out.registered_at == in.registered_at);
    TEST_ASSERT_TRUE(out.last_sync == in.last_sync);
}

void test_server_state_refused(void) {
    server::State s = {};
    s.has_key = true;
    uint8_t bytes[server::kFileSize];
    server::Encode(s, bytes);
    server::State out = {};
    out.registered_at = 7;
    TEST_ASSERT_FALSE(server::Decode(bytes, sizeof bytes - 1, &out));  // cut
    bytes[0] = 'X';
    TEST_ASSERT_FALSE(server::Decode(bytes, sizeof bytes, &out));  // not ours
    bytes[0] = 'S';
    bytes[4] = 2;  // registered with no key
    TEST_ASSERT_FALSE(server::Decode(bytes, sizeof bytes, &out));
    bytes[4] = 4;  // an unknown flag
    TEST_ASSERT_FALSE(server::Decode(bytes, sizeof bytes, &out));
    TEST_ASSERT_TRUE(out.registered_at == 7);  // left as it was
    bytes[4] = 0;
    TEST_ASSERT_TRUE(server::Decode(bytes, sizeof bytes, &out));
    TEST_ASSERT_FALSE(out.has_key);
}

void test_server_base64url(void) {
    // RFC 4648 10, no padding; '-' and '_' for '+' and '/'.
    const char *in[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char *want[] = {"", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy"};
    char out[16];
    for (size_t i = 0; i < 7; ++i) {
        const size_t n = std::strlen(in[i]);
        const size_t len = server::Base64Url(reinterpret_cast<const uint8_t *>(in[i]), n, out,
                                             sizeof out);
        TEST_ASSERT_EQUAL_STRING(want[i], out);
        TEST_ASSERT_EQUAL_UINT(std::strlen(want[i]), len);
        TEST_ASSERT_EQUAL_UINT(len, server::Base64UrlLength(n));
    }
    const uint8_t url_bytes[] = {0xFB, 0xFF};
    server::Base64Url(url_bytes, 2, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("-_8", out);
    TEST_ASSERT_EQUAL_UINT(0, server::Base64Url(url_bytes, 2, out, 3));  // no room for the NUL
}

void test_server_base32(void) {
    const char *in[] = {"f", "fo", "foo", "foob", "fooba", "foobar"};
    const char *want[] = {"MY", "MZXQ", "MZXW6", "MZXW6YQ", "MZXW6YTB", "MZXW6YTBOI"};
    char out[16];
    for (size_t i = 0; i < 6; ++i) {
        const size_t n = std::strlen(in[i]);
        TEST_ASSERT_EQUAL_UINT(std::strlen(want[i]),
                               server::Base32(reinterpret_cast<const uint8_t *>(in[i]), n, out,
                                              sizeof out));
        TEST_ASSERT_EQUAL_STRING(want[i], out);
    }
    TEST_ASSERT_EQUAL_UINT(32, server::Base32Length(server::kTotpSize));
}

void test_server_token(void) {
    const server::Claims c = {"a\"b\\c@example.org", "00112233445566778899aabbccddeeff",
                              "aa:bb:cc:dd:ee:ff", "MZXW6YTBOI", "BAAA", 1761500000};
    char payload[server::kPayloadSize];
    const size_t n = server::Payload(c, payload, sizeof payload);
    TEST_ASSERT_EQUAL_STRING(
        R"({"email":"a\"b\\c@example.org","serial":"00112233445566778899aabbccddeeff",)"
        R"("mac":"aa:bb:cc:dd:ee:ff","totp":"MZXW6YTBOI","pub":"BAAA","iat":1761500000})",
        payload);
    TEST_ASSERT_EQUAL_UINT(std::strlen(payload), n);
    TEST_ASSERT_EQUAL_UINT(0, server::Payload(c, payload, 40));  // cut: refused

    char token[server::kTokenSize];
    const size_t input = server::SigningInput("{}", 2, token, sizeof token);
    TEST_ASSERT_EQUAL_STRING("eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9.e30", token);
    TEST_ASSERT_EQUAL_UINT(std::strlen(token), input);
    uint8_t signature[server::kSignatureSize] = {};
    const size_t total = server::AppendSignature(token, input, sizeof token, signature);
    TEST_ASSERT_EQUAL_UINT(input + 1 + 86, total);
    TEST_ASSERT_EQUAL_UINT(std::strlen(token), total);
    TEST_ASSERT_EQUAL_CHAR('.', token[input]);

    char url[160];
    TEST_ASSERT_TRUE(server::RegisterUrl("auth.merlokk.com", url, sizeof url));
    TEST_ASSERT_EQUAL_STRING("https://auth.merlokk.com/register", url);
    TEST_ASSERT_FALSE(server::RegisterUrl("", url, sizeof url));
    TEST_ASSERT_FALSE(server::RegisterUrl("auth.merlokk.com", url, 20));
}

void test_server_token_fits(void) {
    // The longest claims: a 64-character e-mail, every one escaped.
    char email[65];
    std::memset(email, '"', 64);
    email[64] = '\0';
    char pub[server::Base64UrlLength(server::kPublicSize) + 1];
    std::memset(pub, 'A', sizeof pub - 1);
    pub[sizeof pub - 1] = '\0';
    const server::Claims c = {email, "00112233445566778899aabbccddeeff", "aa:bb:cc:dd:ee:ff",
                              "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567", pub, 4102444800};
    char payload[server::kPayloadSize];
    const size_t n = server::Payload(c, payload, sizeof payload);
    TEST_ASSERT_GREATER_THAN(0, n);
    char token[server::kTokenSize];
    const size_t input = server::SigningInput(payload, n, token, sizeof token);
    TEST_ASSERT_GREATER_THAN(0, input);
    const uint8_t signature[server::kSignatureSize] = {};
    TEST_ASSERT_GREATER_THAN(0, server::AppendSignature(token, input, sizeof token, signature));
}

void test_server_texts(void) {
    char out[24];
    server::Ago(1000, 0, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("never", out);
    server::Ago(1000, 990, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("just now", out);
    server::Ago(100, 990, out, sizeof out);  // the clock behind
    TEST_ASSERT_EQUAL_STRING("just now", out);
    server::Ago(1000 + 5 * 60, 1000, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("5 min ago", out);
    server::Ago(1000 + 3 * 3600 + 59, 1000, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("3 h ago", out);
    server::Ago(1000 + 47 * 3600, 1000, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("47 h ago", out);
    server::Ago(1000 + 3 * 86400, 1000, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("3 days ago", out);

    char code[8];
    server::CodeText(123456, code);
    TEST_ASSERT_EQUAL_STRING("123 456", code);
    server::CodeText(42, code);
    TEST_ASSERT_EQUAL_STRING("000 042", code);

    TEST_ASSERT_EQUAL_STRING("not registered", server::Summary(false, server::Link::kOk));
    TEST_ASSERT_EQUAL_STRING("registered", server::Summary(true, server::Link::kNone));
    TEST_ASSERT_EQUAL_STRING("registered, offline", server::Summary(true, server::Link::kFailed));

    char hex[5];
    const uint8_t bytes[] = {0x0F, 0xA0};
    server::Hex(bytes, 2, hex);
    TEST_ASSERT_EQUAL_STRING("0fa0", hex);
}

}  // namespace

void RegisterServerTests(void) {
    RUN_TEST(test_server_state_round_trip);
    RUN_TEST(test_server_state_refused);
    RUN_TEST(test_server_base64url);
    RUN_TEST(test_server_base32);
    RUN_TEST(test_server_token);
    RUN_TEST(test_server_token_fits);
    RUN_TEST(test_server_texts);
}
