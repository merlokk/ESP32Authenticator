// Random bytes as hex (devices/rng/rng_logic.h).

#include <cstring>

#include "rng_logic.h"
#include "unity.h"

namespace {

void test_rng_bytes_for_hex(void) {
    TEST_ASSERT_EQUAL_UINT(0, rng::BytesForHex(0));
    TEST_ASSERT_EQUAL_UINT(1, rng::BytesForHex(1));
    TEST_ASSERT_EQUAL_UINT(1, rng::BytesForHex(2));
    TEST_ASSERT_EQUAL_UINT(2, rng::BytesForHex(3));
    TEST_ASSERT_EQUAL_UINT(1024, rng::BytesForHex(rng::kMaxHexChars));
}

void test_rng_to_hex(void) {
    const uint8_t bytes[] = {0x01, 0xAB, 0xF0, 0x9C};
    char out[16];
    TEST_ASSERT_TRUE(rng::ToHex(bytes, 8, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("01abf09c", out);
    // An odd length: the last byte's high nibble.
    TEST_ASSERT_TRUE(rng::ToHex(bytes, 3, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("01a", out);
    TEST_ASSERT_TRUE(rng::ToHex(bytes, 0, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_rng_to_hex_needs_room(void) {
    const uint8_t bytes[] = {0x12, 0x34};
    char out[5];
    memset(out, 'x', sizeof out);
    TEST_ASSERT_FALSE(rng::ToHex(bytes, 4, out, 4));  // no room for the NUL
    TEST_ASSERT_EQUAL_CHAR('x', out[0]);
    TEST_ASSERT_TRUE(rng::ToHex(bytes, 4, out, 5));
    TEST_ASSERT_EQUAL_STRING("1234", out);
}

}  // namespace

void RegisterRngTests(void) {
    RUN_TEST(test_rng_bytes_for_hex);
    RUN_TEST(test_rng_to_hex);
    RUN_TEST(test_rng_to_hex_needs_room);
}
