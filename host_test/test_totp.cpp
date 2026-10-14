// totp: the pure half of the algorithm (devices/crypto/otp.h: counter, message,
// dynamic truncation against the HMAC values of RFC 4226 appendix D), Base32
// (RFC 4648) and the three windows. The HMAC itself runs on the board
// (`totp test`: the RFC 6238 vectors).

#include <cstring>

#include "otp.h"
#include "totp.h"
#include "unity.h"

namespace {

void FromHex(const char *hex, uint8_t *out) {
    auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (size_t i = 0; hex[2 * i] != '\0'; ++i) {
        out[i] = static_cast<uint8_t>(nibble(hex[2 * i]) << 4 | nibble(hex[2 * i + 1]));
    }
}

void test_totp_truncates_the_rfc4226_hmacs(void) {
    // HMAC-SHA1("12345678901234567890", counter) and the HOTP value.
    struct Vector {
        const char *hmac;
        uint32_t code;
    };
    const Vector vectors[] = {
        {"cc93cf18508d94934c64b65d8ba7667fb7cde4b0", 755224},
        {"75a48a19d4cbe100644e8ac1397eea747a2d33ab", 287082},
        {"0bacb7fa082fef30782211938bc1c5e70416ff44", 359152},
        {"66c28227d03a2d5529262ff016a1e6ef76557ece", 969429},
        {"a904c900a64b35909874b33e61c5938a8e15ed1c", 338314},
        {"a37e783d7b7233c083d4f62926c7a25f238d0316", 254676},
        {"bc9cd28561042c83f219324d3c607256c03272ae", 287922},
        {"a4fb960c0bc06e1eabb804e5b397cdc4b45596fa", 162583},
        {"1b3c89f65e6c9e883012052823443f048b4332db", 399871},
        {"1637409809a679dc698207310c8c7fc07290d9e5", 520489},
    };
    for (const Vector &v : vectors) {
        uint8_t mac[crypto::kSha1Size];
        FromHex(v.hmac, mac);
        TEST_ASSERT_EQUAL_UINT32(v.code, crypto::OtpTruncate(mac));
    }
    // 8 digits: the RFC's full value of counter 0 is 1284755224.
    uint8_t mac[crypto::kSha1Size];
    FromHex(vectors[0].hmac, mac);
    TEST_ASSERT_EQUAL_UINT32(84755224, crypto::OtpTruncate(mac, 8));
}

void test_totp_counter_and_message(void) {
    TEST_ASSERT_EQUAL_UINT64(0, crypto::OtpCounter(0));
    TEST_ASSERT_EQUAL_UINT64(0, crypto::OtpCounter(29));
    TEST_ASSERT_EQUAL_UINT64(1, crypto::OtpCounter(30));
    TEST_ASSERT_EQUAL_UINT64(0x23523ED, crypto::OtpCounter(1111111111));  // RFC 6238 table
    TEST_ASSERT_EQUAL_UINT64(0x27BC86AA, crypto::OtpCounter(20000000000));
    TEST_ASSERT_EQUAL_UINT64(0, crypto::OtpCounter(-5));
    uint8_t message[8];
    crypto::OtpMessage(0x0102030405060708ULL, message);
    const uint8_t expected[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    TEST_ASSERT_EQUAL_MEMORY(expected, message, 8);
}

void test_totp_base32(void) {
    uint8_t out[totp::kMaxKeyBytes];
    // "12345678901234567890", the RFC secret.
    TEST_ASSERT_EQUAL_UINT32(
        20, totp::Base32Decode("GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ", out, sizeof out));
    TEST_ASSERT_EQUAL_MEMORY("12345678901234567890", out, 20);
    // Lower case, spaces, dashes and padding, as apps show secrets.
    TEST_ASSERT_EQUAL_UINT32(
        20, totp::Base32Decode("gezd gnbv-gy3t qojq gezd gnbv gy3t qojq", out, sizeof out));
    TEST_ASSERT_EQUAL_MEMORY("12345678901234567890", out, 20);
    TEST_ASSERT_EQUAL_UINT32(5, totp::Base32Decode("MZXW6YTB", out, sizeof out));
    TEST_ASSERT_EQUAL_MEMORY("fooba", out, 5);
    TEST_ASSERT_EQUAL_UINT32(1, totp::Base32Decode("MY======", out, sizeof out));
    TEST_ASSERT_EQUAL_MEMORY("f", out, 1);
}

void test_totp_base64(void) {
    uint8_t out[64] = {};
    // RFC 4648 section 10.
    TEST_ASSERT_EQUAL_UINT32(6, totp::Base64Decode("Zm9vYmFy", out, sizeof out));
    TEST_ASSERT_EQUAL_MEMORY("foobar", out, 6);
    TEST_ASSERT_EQUAL_UINT32(4, totp::Base64Decode("Zm9vYg==", out, sizeof out));
    TEST_ASSERT_EQUAL_MEMORY("foob", out, 4);
    TEST_ASSERT_EQUAL_UINT32(5, totp::Base64Decode("Zm9v YmE", out, sizeof out));
    // Standard and URL-safe alphabets.
    TEST_ASSERT_EQUAL_UINT32(3, totp::Base64Decode("+/+/", out, sizeof out));
    TEST_ASSERT_EQUAL_UINT32(0xFB, out[0]);
    TEST_ASSERT_EQUAL_UINT32(3, totp::Base64Decode("-_-_", out, sizeof out));
    TEST_ASSERT_EQUAL_UINT32(0xFB, out[0]);
    TEST_ASSERT_EQUAL_UINT32(0, totp::Base64Decode("", out, sizeof out));
    TEST_ASSERT_EQUAL_UINT32(0, totp::Base64Decode("Zm9v!", out, sizeof out));
    TEST_ASSERT_EQUAL_UINT32(0, totp::Base64Decode("Zm9vY", out, sizeof out));  // stray digit
    TEST_ASSERT_EQUAL_UINT32(0, totp::Base64Decode("Zm9vYmFy", out, 5));         // overflow
}

void test_totp_base32_encode(void) {
    char text[40];
    TEST_ASSERT_TRUE(totp::Base32Encode(reinterpret_cast<const uint8_t *>("foobar"), 6, text,
                                        sizeof text));
    TEST_ASSERT_EQUAL_STRING("MZXW6YTBOI", text);  // RFC 4648 without padding
    TEST_ASSERT_TRUE(totp::Base32Encode(reinterpret_cast<const uint8_t *>("f"), 1, text,
                                        sizeof text));
    TEST_ASSERT_EQUAL_STRING("MY", text);
    TEST_ASSERT_FALSE(totp::Base32Encode(reinterpret_cast<const uint8_t *>("foobar"), 6, text,
                                         10));  // no room for the NUL
}

void test_totp_base32_rejects(void) {
    uint8_t out[4];
    TEST_ASSERT_EQUAL_UINT32(0, totp::Base32Decode("", out, sizeof out));
    TEST_ASSERT_EQUAL_UINT32(0, totp::Base32Decode("ABC1", out, sizeof out));  // 1 is not Base32
    TEST_ASSERT_EQUAL_UINT32(0, totp::Base32Decode("AB8C", out, sizeof out));
    TEST_ASSERT_EQUAL_UINT32(0, totp::Base32Decode("MZXW6YTB", out, sizeof out));  // 5 > 4
    TEST_ASSERT_EQUAL_UINT32(0, totp::Base32Decode(nullptr, out, sizeof out));
}

void test_totp_windows(void) {
    totp::Windows w = totp::WindowsAt(1111111111);  // 1111111110 + 1
    TEST_ASSERT_EQUAL_INT64(1111111110, w.start);
    TEST_ASSERT_EQUAL_UINT32(29, w.remaining);
    TEST_ASSERT_EQUAL_UINT64(crypto::OtpCounter(1111111111) - 1, crypto::OtpCounter(w.times[0]));
    TEST_ASSERT_EQUAL_UINT64(crypto::OtpCounter(1111111111), crypto::OtpCounter(w.times[1]));
    TEST_ASSERT_EQUAL_UINT64(crypto::OtpCounter(1111111111) + 1, crypto::OtpCounter(w.times[2]));

    w = totp::WindowsAt(1111111110);  // on the boundary: a whole window left
    TEST_ASSERT_EQUAL_INT64(1111111110, w.start);
    TEST_ASSERT_EQUAL_UINT32(30, w.remaining);
    w = totp::WindowsAt(1111111139);
    TEST_ASSERT_EQUAL_UINT32(1, w.remaining);
}

}  // namespace

void RegisterTotpTests(void) {
    RUN_TEST(test_totp_truncates_the_rfc4226_hmacs);
    RUN_TEST(test_totp_counter_and_message);
    RUN_TEST(test_totp_base32);
    RUN_TEST(test_totp_base32_rejects);
    RUN_TEST(test_totp_base64);
    RUN_TEST(test_totp_base32_encode);
    RUN_TEST(test_totp_windows);
}
