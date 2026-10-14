// store_header: the plain header in front of store files. A wrong byte here
// either loses the user's store or lets an old copy back in.

#include <cstring>

#include "store_header.h"
#include "unity.h"

namespace {

using storefile::Header;
using storefile::kHeaderSize;
using storefile::Kind;
using storefile::Parse;
using storefile::Verdict;

Header Sample() {
    Header h;
    h.kind = Kind::kFido;
    h.version = 0x01020304;
    h.length = 100;
    h.modified_utc = 0x0000000167A1B2C3LL;
    return h;
}

void test_storefile_layout_is_fixed_little_endian(void) {
    uint8_t out[kHeaderSize];
    storefile::Encode(Sample(), out);
    const uint8_t want[24] = {'E', 'S', 'A', 'F', 1, 1, 0, 0,  0x04, 0x03, 0x02, 0x01,
                              100, 0,   0,   0,   0xC3, 0xB2, 0xA1, 0x67, 1, 0, 0, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, out, sizeof want);
    for (size_t i = 24; i < kHeaderSize; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0, out[i]);  // nonce and reserved: zero when plain
    }
}

void test_storefile_round_trips(void) {
    uint8_t out[kHeaderSize];
    Header in = Sample();
    in.modified_utc = -5;  // whatever was written comes back
    storefile::Encode(in, out);
    Header back;
    TEST_ASSERT_EQUAL(Parse::kOk, storefile::Decode(out, sizeof out, kHeaderSize + 100,
                                                    Kind::kFido, &back));
    TEST_ASSERT_EQUAL_UINT32(in.version, back.version);
    TEST_ASSERT_EQUAL_UINT32(in.length, back.length);
    TEST_ASSERT_TRUE(back.modified_utc == -5);
    TEST_ASSERT_EQUAL(storefile::Cipher::kPlain, back.cipher);
}

void test_storefile_a_file_without_magic_has_no_header(void) {
    // A pre-header fido.bin starts with lionkey's item header (key 1 or 2).
    const uint8_t old[kHeaderSize] = {2, 0, 0, 0};
    Header h;
    TEST_ASSERT_EQUAL(Parse::kNoHeader,
                      storefile::Decode(old, sizeof old, 500, Kind::kFido, &h));
    TEST_ASSERT_EQUAL(Parse::kNoHeader, storefile::Decode(old, 0, 0, Kind::kFido, &h));
}

void test_storefile_refuses_a_wrong_size_kind_format_or_cipher(void) {
    uint8_t out[kHeaderSize];
    storefile::Encode(Sample(), out);
    Header h;
    TEST_ASSERT_EQUAL(Parse::kBad, storefile::Decode(out, sizeof out, kHeaderSize + 99,
                                                     Kind::kFido, &h));  // truncated
    TEST_ASSERT_EQUAL(Parse::kBad, storefile::Decode(out, sizeof out, kHeaderSize + 101,
                                                     Kind::kFido, &h));  // padded
    TEST_ASSERT_EQUAL(Parse::kBad, storefile::Decode(out, 20, kHeaderSize + 100,
                                                     Kind::kFido, &h));  // short read
    TEST_ASSERT_EQUAL(Parse::kBad, storefile::Decode(out, sizeof out, kHeaderSize + 100,
                                                     Kind::kRecords, &h));
    out[4] = 2;
    TEST_ASSERT_EQUAL(Parse::kBad, storefile::Decode(out, sizeof out, kHeaderSize + 100,
                                                     Kind::kFido, &h));
    out[4] = 1;
    out[6] = 2;
    TEST_ASSERT_EQUAL(Parse::kBad, storefile::Decode(out, sizeof out, kHeaderSize + 100,
                                                     Kind::kFido, &h));
}

void test_storefile_an_encrypted_file_counts_its_tag(void) {
    Header in = Sample();
    in.cipher = storefile::Cipher::kAesGcm;
    in.nonce[0] = 0xAA;
    uint8_t out[kHeaderSize];
    storefile::Encode(in, out);
    Header back;
    TEST_ASSERT_EQUAL(Parse::kBad, storefile::Decode(out, sizeof out, kHeaderSize + 100,
                                                     Kind::kFido, &back));
    TEST_ASSERT_EQUAL(Parse::kOk, storefile::Decode(out, sizeof out,
                                                    kHeaderSize + 100 + storefile::kTagSize,
                                                    Kind::kFido, &back));
    TEST_ASSERT_EQUAL_UINT8(0xAA, back.nonce[0]);
}

void test_storefile_an_older_version_is_a_rollback(void) {
    TEST_ASSERT_EQUAL(Verdict::kRollback, storefile::Check(Parse::kOk, 4, true, 5));
    TEST_ASSERT_EQUAL(Verdict::kLoad, storefile::Check(Parse::kOk, 5, true, 5));
    // Newer than NVS: written just before a power loss, or synced in.
    TEST_ASSERT_EQUAL(Verdict::kLoad, storefile::Check(Parse::kOk, 6, true, 5));
    TEST_ASSERT_EQUAL(Verdict::kLoad, storefile::Check(Parse::kOk, 1, false, 0));
}

void test_storefile_a_headerless_file_is_migrated_only_once(void) {
    TEST_ASSERT_EQUAL(Verdict::kMigrate, storefile::Check(Parse::kNoHeader, 0, false, 0));
    TEST_ASSERT_EQUAL(Verdict::kRollback, storefile::Check(Parse::kNoHeader, 0, true, 1));
}

void test_storefile_next_version_is_above_both(void) {
    TEST_ASSERT_EQUAL_UINT32(6, storefile::NextVersion(5, 3));
    TEST_ASSERT_EQUAL_UINT32(8, storefile::NextVersion(5, 7));
    TEST_ASSERT_EQUAL_UINT32(1, storefile::NextVersion(0, 0));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, storefile::NextVersion(UINT32_MAX, 0));
}

void test_storefile_sync_later_time_wins_then_higher_version(void) {
    Header a = Sample();
    Header b = Sample();
    a.modified_utc = 2000;
    a.version = 1;
    b.modified_utc = 1000;
    b.version = 9;
    TEST_ASSERT_TRUE(storefile::Newer(a, b));
    TEST_ASSERT_FALSE(storefile::Newer(b, a));
    b.modified_utc = 0;  // unknown time loses to any known one
    b.version = 50;
    TEST_ASSERT_TRUE(storefile::Newer(a, b));
    b.modified_utc = 2000;
    TEST_ASSERT_TRUE(storefile::Newer(b, a));
    TEST_ASSERT_FALSE(storefile::Newer(a, a));
}

}  // namespace

void RegisterStorefileTests(void) {
    RUN_TEST(test_storefile_layout_is_fixed_little_endian);
    RUN_TEST(test_storefile_round_trips);
    RUN_TEST(test_storefile_a_file_without_magic_has_no_header);
    RUN_TEST(test_storefile_refuses_a_wrong_size_kind_format_or_cipher);
    RUN_TEST(test_storefile_an_encrypted_file_counts_its_tag);
    RUN_TEST(test_storefile_an_older_version_is_a_rollback);
    RUN_TEST(test_storefile_a_headerless_file_is_migrated_only_once);
    RUN_TEST(test_storefile_next_version_is_above_both);
    RUN_TEST(test_storefile_sync_later_time_wins_then_higher_version);
}
