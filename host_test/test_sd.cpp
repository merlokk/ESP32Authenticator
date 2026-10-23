// SD card mount plan, paths under the mount point, sizes
// (devices/sd_card/sd_policy.h).

#include <cstring>

#include "sd_policy.h"
#include "unity.h"

namespace {

using sd_card::Outcome;
using sd_card::Retry;

void test_sd_attempt_clock(void) {
    TEST_ASSERT_EQUAL_UINT32(40000, sd_card::AttemptKhz(0));
    TEST_ASSERT_EQUAL_UINT32(40000, sd_card::AttemptKhz(1));
    TEST_ASSERT_EQUAL_UINT32(20000, sd_card::AttemptKhz(2));
    TEST_ASSERT_EQUAL_UINT32(20000, sd_card::AttemptKhz(3));
}

void test_sd_retry(void) {
    // Mounted, or a card without FAT: done.
    TEST_ASSERT_FALSE(Retry(0, Outcome::kOk, 0));
    TEST_ASSERT_FALSE(Retry(0, Outcome::kNoFilesystem, 0));
    // A marginal card: every attempt.
    TEST_ASSERT_TRUE(Retry(0, Outcome::kFailed, 0));
    TEST_ASSERT_TRUE(Retry(2, Outcome::kFailed, 0));
    TEST_ASSERT_FALSE(Retry(sd_card::kMaxAttempts - 1, Outcome::kFailed, 0));
    // No card: two attempts, not four.
    TEST_ASSERT_TRUE(Retry(0, Outcome::kNoAnswer, 1));
    TEST_ASSERT_FALSE(Retry(1, Outcome::kNoAnswer, 2));
    // A failure in between starts the count again.
    TEST_ASSERT_TRUE(Retry(2, Outcome::kNoAnswer, 1));
}

void AssertJoin(const char *rel, const char *expected) {
    char out[64];
    TEST_ASSERT_TRUE_MESSAGE(sd_card::JoinPath("/sdcard", rel, out, sizeof out), rel);
    TEST_ASSERT_EQUAL_STRING(expected, out);
}

void AssertRefused(const char *rel, size_t size = 64) {
    char out[64];
    TEST_ASSERT_FALSE_MESSAGE(sd_card::JoinPath("/sdcard", rel, out, size), rel);
}

void test_sd_join_path(void) {
    AssertJoin("", "/sdcard");
    AssertJoin("/", "/sdcard");
    AssertJoin("books", "/sdcard/books");
    AssertJoin("/books/", "/sdcard/books");
    AssertJoin("a/b.txt", "/sdcard/a/b.txt");
    AssertJoin("..hidden", "/sdcard/..hidden");  // a name, not the parent
    AssertJoin("a..b", "/sdcard/a..b");
    AssertJoin("\xd0\x9a\xd0\xbd\xd0\xb8\xd0\xb3\xd0\xb8", "/sdcard/\xd0\x9a\xd0\xbd\xd0\xb8\xd0\xb3\xd0\xb8");
}

void test_sd_join_path_refuses(void) {
    AssertRefused("..");
    AssertRefused("../spiffs");
    AssertRefused("books/../..");
    AssertRefused("a\nb");
    AssertRefused("a-name-long-enough-to-overflow-the-buffer", 16);
}

void AssertBytes(uint64_t bytes, const char *expected) {
    char out[16];
    sd_card::FormatBytes(bytes, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING(expected, out);
}

void test_sd_format_bytes(void) {
    AssertBytes(0, "0 B");
    AssertBytes(1023, "1023 B");
    AssertBytes(1024, "1.0 KB");
    AssertBytes(1536, "1.5 KB");
    AssertBytes(5ULL * 1024 * 1024, "5.0 MB");
    AssertBytes(31914983424ULL, "29.7 GB");  // a "32 GB" card
}

}  // namespace

void RegisterSdTests(void) {
    RUN_TEST(test_sd_attempt_clock);
    RUN_TEST(test_sd_retry);
    RUN_TEST(test_sd_join_path);
    RUN_TEST(test_sd_join_path_refuses);
    RUN_TEST(test_sd_format_bytes);
}
