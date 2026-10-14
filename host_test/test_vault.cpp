// vault records: what the user typed is what is kept, sorted, unique per
// (site, user), and survives a serialize / parse round trip byte for byte.

#include <cstring>

#include "records.h"
#include "unity.h"

namespace {

using vault::Error;
using vault::Field;
using vault::Record;
using vault::Records;

Record g_slots[8];
uint8_t g_buffer[vault::MaxSerialized(8)];
Record g_other_slots[8];

void test_vault_add_keeps_sites_sorted_and_users_apart(void) {
    Records r(g_slots, 8);
    uint32_t id = 0;
    TEST_ASSERT_EQUAL(Error::kOk, r.Add("github.com", "bob", "pw1", "", 100, &id));
    TEST_ASSERT_EQUAL_UINT32(1, id);
    TEST_ASSERT_EQUAL(Error::kOk, r.Add("Amazon", "x", "", "", 0, nullptr));
    TEST_ASSERT_EQUAL(Error::kOk, r.Add("GitHub.com", "alice", "pw2", "", 0, &id));
    TEST_ASSERT_EQUAL_UINT32(3, id);
    TEST_ASSERT_EQUAL_UINT32(3, r.Count());
    TEST_ASSERT_EQUAL_STRING("Amazon", r.At(0).site);
    TEST_ASSERT_EQUAL_STRING("alice", r.At(1).user);  // same site, users sorted
    TEST_ASSERT_EQUAL_STRING("bob", r.At(2).user);
    TEST_ASSERT_EQUAL(100, r.At(2).modified_utc);
}

void test_vault_the_same_user_on_a_site_is_a_duplicate(void) {
    Records r(g_slots, 8);
    TEST_ASSERT_EQUAL(Error::kOk, r.Add("site", "Bob", "", "", 0, nullptr));
    TEST_ASSERT_EQUAL(Error::kDuplicate, r.Add("SITE", "bob", "", "", 0, nullptr));
    TEST_ASSERT_EQUAL(Error::kOk, r.Add("site", "", "", "", 0, nullptr));  // no user: once
    TEST_ASSERT_EQUAL(Error::kDuplicate, r.Add("site", "", "", "", 0, nullptr));
    TEST_ASSERT_EQUAL_UINT32(2, r.Count());
}

void test_vault_fields_are_checked(void) {
    Records r(g_slots, 8);
    char long_site[vault::kSiteMax + 2];
    memset(long_site, 'a', sizeof long_site - 1);
    long_site[sizeof long_site - 1] = '\0';
    TEST_ASSERT_EQUAL(Error::kSiteEmpty, r.Add("", "u", "", "", 0, nullptr));
    TEST_ASSERT_EQUAL(Error::kTooLong, r.Add(long_site, "u", "", "", 0, nullptr));
    TEST_ASSERT_EQUAL(Error::kBadText, r.Add("a\tb", "u", "", "", 0, nullptr));
    TEST_ASSERT_EQUAL(Error::kBadOtp, r.Add("a", "u", "", "not base32!", 0, nullptr));
    TEST_ASSERT_EQUAL(Error::kOk, r.Add("a", "u", "p@ss w\xC3\xB6rd", "", 0, nullptr));  // UTF-8
    TEST_ASSERT_EQUAL_UINT32(1, r.Count());
}

void test_vault_otp_is_normalized_base32(void) {
    Records r(g_slots, 8);
    TEST_ASSERT_EQUAL(Error::kOk, r.Add("a", "u", "", "jbsw y3dp-ehpk 3pxp==", 0, nullptr));
    TEST_ASSERT_EQUAL_STRING("JBSWY3DPEHPK3PXP", r.At(0).otp);
}

void test_vault_full_store_refuses(void) {
    Records r(g_slots, 2);
    TEST_ASSERT_EQUAL(Error::kOk, r.Add("a", "", "", "", 0, nullptr));
    TEST_ASSERT_EQUAL(Error::kOk, r.Add("b", "", "", "", 0, nullptr));
    TEST_ASSERT_EQUAL(Error::kFull, r.Add("c", "", "", "", 0, nullptr));
}

void test_vault_set_resorts_and_keeps_the_id(void) {
    Records r(g_slots, 8);
    uint32_t id = 0;
    r.Add("b", "u", "old", "", 0, &id);
    r.Add("c", "u", "", "", 0, nullptr);
    TEST_ASSERT_EQUAL(Error::kOk, r.Set(id, Field::kSite, "d", 50));
    TEST_ASSERT_EQUAL_STRING("c", r.At(0).site);
    TEST_ASSERT_EQUAL_STRING("d", r.At(1).site);
    TEST_ASSERT_EQUAL_UINT32(id, r.At(1).id);
    TEST_ASSERT_EQUAL(50, r.At(1).modified_utc);
    TEST_ASSERT_EQUAL(Error::kOk, r.Set(id, Field::kPassword, "new", 60));
    TEST_ASSERT_EQUAL_STRING("new", r.At(1).password);
    TEST_ASSERT_EQUAL(Error::kDuplicate, r.Set(id, Field::kSite, "C", 0));
    TEST_ASSERT_EQUAL(Error::kOk, r.Set(id, Field::kSite, "D", 0));  // its own case change
    TEST_ASSERT_EQUAL(Error::kNotFound, r.Set(999, Field::kUser, "x", 0));
    TEST_ASSERT_EQUAL(Error::kBadOtp, r.Set(id, Field::kOtp, "1", 0));
}

void test_vault_remove_and_ids_never_repeat(void) {
    Records r(g_slots, 8);
    uint32_t a = 0;
    uint32_t b = 0;
    r.Add("a", "", "", "", 0, &a);
    r.Add("b", "", "", "", 0, &b);
    TEST_ASSERT_EQUAL(Error::kOk, r.Remove(a));
    TEST_ASSERT_EQUAL(Error::kNotFound, r.Remove(a));
    TEST_ASSERT_EQUAL_UINT32(1, r.Count());
    TEST_ASSERT_EQUAL_STRING("b", r.At(0).site);
    uint32_t c = 0;
    r.Add("a", "", "", "", 0, &c);
    TEST_ASSERT_EQUAL_UINT32(3, c);
}

void test_vault_round_trip(void) {
    Records r(g_slots, 8);
    r.Add("site", "user", "secret", "JBSWY3DPEHPK3PXP", 1700000000, nullptr);
    r.Add("other", "", "", "", 0, nullptr);
    r.Remove(2);
    r.Add("third", "x", "y", "", -1, nullptr);
    const size_t n = r.Serialize(g_buffer, sizeof g_buffer);
    TEST_ASSERT_NOT_EQUAL(0, n);
    Records back(g_other_slots, 8);
    TEST_ASSERT_EQUAL(Error::kOk, back.Parse(g_buffer, n));
    TEST_ASSERT_EQUAL_UINT32(2, back.Count());
    TEST_ASSERT_EQUAL_STRING("secret", back.At(0).password);
    TEST_ASSERT_EQUAL_STRING("JBSWY3DPEHPK3PXP", back.At(0).otp);
    TEST_ASSERT_TRUE(back.At(0).modified_utc == 1700000000);
    TEST_ASSERT_TRUE(back.At(1).modified_utc == -1);
    uint32_t id = 0;
    back.Add("fourth", "", "", "", 0, &id);
    TEST_ASSERT_EQUAL_UINT32(4, id);  // next id travels with the data
    TEST_ASSERT_EQUAL(0, r.Serialize(g_buffer, n - 1));  // no room: nothing
}

void test_vault_parse_refuses_malformed_data(void) {
    Records r(g_slots, 8);
    r.Add("site", "user", "pw", "", 0, nullptr);
    const size_t n = r.Serialize(g_buffer, sizeof g_buffer);
    Records back(g_other_slots, 8);
    TEST_ASSERT_EQUAL(Error::kBadFormat, back.Parse(g_buffer, n - 1));  // truncated
    g_buffer[n] = 0;
    TEST_ASSERT_EQUAL(Error::kBadFormat, back.Parse(g_buffer, n + 1));  // trailing byte
    g_buffer[0] = 2;
    TEST_ASSERT_EQUAL(Error::kBadFormat, back.Parse(g_buffer, n));  // other format
    g_buffer[0] = 1;
    g_buffer[19] = 200;  // site length beyond the data
    TEST_ASSERT_EQUAL(Error::kBadFormat, back.Parse(g_buffer, n));
    TEST_ASSERT_EQUAL_UINT32(0, back.Count());
    Records small(g_other_slots, 0);
    r.Serialize(g_buffer, sizeof g_buffer);
    TEST_ASSERT_EQUAL(Error::kFull, small.Parse(g_buffer, n));
}

void test_vault_clear_wipes_secrets(void) {
    Records r(g_slots, 8);
    r.Add("site", "user", "secret", "", 0, nullptr);
    r.Clear();
    for (size_t i = 0; i < sizeof g_slots; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0, reinterpret_cast<const uint8_t *>(g_slots)[i]);
    }
}

}  // namespace

void RegisterVaultTests(void) {
    RUN_TEST(test_vault_add_keeps_sites_sorted_and_users_apart);
    RUN_TEST(test_vault_the_same_user_on_a_site_is_a_duplicate);
    RUN_TEST(test_vault_fields_are_checked);
    RUN_TEST(test_vault_otp_is_normalized_base32);
    RUN_TEST(test_vault_full_store_refuses);
    RUN_TEST(test_vault_set_resorts_and_keeps_the_id);
    RUN_TEST(test_vault_remove_and_ids_never_repeat);
    RUN_TEST(test_vault_round_trip);
    RUN_TEST(test_vault_parse_refuses_malformed_data);
    RUN_TEST(test_vault_clear_wipes_secrets);
}
