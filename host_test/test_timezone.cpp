// zones: names -> POSIX rules, and telling a rule from a misspelled name
// (libc reads a misspelled zone as UTC and says nothing). Ported from
// approver-esp32. Applying a rule (timezone.cpp) is not here: the Windows CRT
// does not parse the transition rules.

#include <cstring>

#include "unity.h"
#include "zones.h"

namespace {

void test_tz_every_row_is_complete(void) {
    TEST_ASSERT_TRUE(tz::Count() > 0);
    for (size_t i = 0; i < tz::Count(); ++i) {
        const tz::Zone &zone = tz::At(i);
        TEST_ASSERT_NOT_NULL(zone.name);
        TEST_ASSERT_NOT_NULL(zone.posix);
        TEST_ASSERT_TRUE(zone.name[0] != '\0');
        TEST_ASSERT_TRUE(std::strlen(zone.name) < tz::kMaxNameLength);
        TEST_ASSERT_TRUE(std::strlen(zone.posix) < tz::kMaxPosixLength);
    }
}

void test_tz_an_index_past_the_end_answers_the_first_row(void) {
    TEST_ASSERT_EQUAL_STRING("UTC", tz::At(tz::Count()).name);
    TEST_ASSERT_EQUAL_STRING("UTC", tz::At(tz::Count() + 1000).name);
}

void test_tz_every_name_looks_itself_up(void) {
    for (size_t i = 0; i < tz::Count(); ++i) {
        const tz::Zone &zone = tz::At(i);
        const char *found = tz::Lookup(zone.name);
        TEST_ASSERT_NOT_NULL_MESSAGE(found, zone.name);
        TEST_ASSERT_EQUAL_STRING(zone.posix, found);
    }
}

void test_tz_every_rule_is_a_rule_and_no_name_is(void) {
    for (size_t i = 0; i < tz::Count(); ++i) {
        TEST_ASSERT_TRUE_MESSAGE(tz::LooksLikePosix(tz::At(i).posix), tz::At(i).posix);
        TEST_ASSERT_FALSE_MESSAGE(tz::LooksLikePosix(tz::At(i).name), tz::At(i).name);
    }
}

void test_tz_lookup_ignores_case_and_follows_aliases(void) {
    TEST_ASSERT_EQUAL_STRING("EET-2EEST,M3.5.0/3,M10.5.0/4", tz::Lookup("Europe/Kyiv"));
    TEST_ASSERT_EQUAL_STRING(tz::Lookup("Europe/Kyiv"), tz::Lookup("europe/KYIV"));
    TEST_ASSERT_EQUAL_STRING(tz::Lookup("Europe/Kyiv"), tz::Lookup("Europe/Kiev"));
    TEST_ASSERT_EQUAL_STRING(tz::Lookup("Asia/Kolkata"), tz::Lookup("Asia/Calcutta"));
    TEST_ASSERT_EQUAL_STRING("UTC0", tz::Lookup("GMT"));
}

void test_tz_an_unknown_zone_is_null_not_a_guess(void) {
    TEST_ASSERT_NULL(tz::Lookup("Atlantis/Capital"));
    TEST_ASSERT_NULL(tz::Lookup("Europe/Kyev"));
    TEST_ASSERT_NULL(tz::Lookup("Europe/Kyiv "));
    TEST_ASSERT_NULL(tz::Lookup(""));
    TEST_ASSERT_NULL(tz::Lookup(nullptr));
}

void test_tz_a_shared_rule_is_named_after_its_family(void) {
    TEST_ASSERT_EQUAL_STRING("EET", tz::NameFor(tz::Lookup("Europe/Kyiv")));
    TEST_ASSERT_EQUAL_STRING("CET", tz::NameFor(tz::Lookup("Europe/Paris")));
    TEST_ASSERT_EQUAL_STRING("UTC", tz::NameFor("UTC0"));
    TEST_ASSERT_NULL(tz::NameFor("XYZ-9XYZDST,M1.1.0,M2.2.0"));
    TEST_ASSERT_NULL(tz::NameFor(nullptr));
}

void test_tz_tells_a_rule_from_a_name(void) {
    TEST_ASSERT_TRUE(tz::LooksLikePosix("UTC0"));
    TEST_ASSERT_TRUE(tz::LooksLikePosix("EET-2EEST,M3.5.0/3,M10.5.0/4"));
    TEST_ASSERT_TRUE(tz::LooksLikePosix("IST-5:30"));
    TEST_ASSERT_FALSE(tz::LooksLikePosix("Europe/Kyiv"));
    TEST_ASSERT_FALSE(tz::LooksLikePosix("EET"));         // too short: a name
    TEST_ASSERT_FALSE(tz::LooksLikePosix("Antarctica"));  // no offset
    TEST_ASSERT_FALSE(tz::LooksLikePosix("UTC 0"));
    TEST_ASSERT_FALSE(tz::LooksLikePosix(""));
    TEST_ASSERT_FALSE(tz::LooksLikePosix(nullptr));
    TEST_ASSERT_FALSE(tz::LooksLikePosix("XXX-1XXXX,M3.5.0/3,M10.5.0/4,M10.5.0/4,M10.5.0"));
}

void test_tz_resolve_takes_a_name_or_a_rule(void) {
    TEST_ASSERT_EQUAL_STRING("MSK-3", tz::Resolve("europe/moscow"));
    TEST_ASSERT_EQUAL_STRING("<+05>-5", tz::Resolve("<+05>-5"));
    TEST_ASSERT_NULL(tz::Resolve("Europe/Kyev"));
    TEST_ASSERT_NULL(tz::Resolve(""));
}

}  // namespace

void RegisterTimezoneTests(void) {
    RUN_TEST(test_tz_every_row_is_complete);
    RUN_TEST(test_tz_an_index_past_the_end_answers_the_first_row);
    RUN_TEST(test_tz_every_name_looks_itself_up);
    RUN_TEST(test_tz_every_rule_is_a_rule_and_no_name_is);
    RUN_TEST(test_tz_lookup_ignores_case_and_follows_aliases);
    RUN_TEST(test_tz_an_unknown_zone_is_null_not_a_guess);
    RUN_TEST(test_tz_a_shared_rule_is_named_after_its_family);
    RUN_TEST(test_tz_tells_a_rule_from_a_name);
    RUN_TEST(test_tz_resolve_takes_a_name_or_a_rule);
}
