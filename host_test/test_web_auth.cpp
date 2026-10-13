// web_auth: when a credential is required, what passes, the encoder.
// Ported from approver-esp32.

#include "unity.h"
#include "web_auth.h"

#include <cstring>

namespace {

constexpr const char *kUser = "admin";
constexpr const char *kPassword = "secret";
constexpr const char *kHeader = "Basic YWRtaW46c2VjcmV0";

}  // namespace

void test_no_credentials_in_the_config_is_no_authentication(void) {
    TEST_ASSERT_FALSE(web::AuthRequired("", ""));
    TEST_ASSERT_FALSE(web::AuthRequired(nullptr, nullptr));
}

void test_half_a_credential_is_not_authentication(void) {
    TEST_ASSERT_FALSE(web::AuthRequired(kUser, ""));
    TEST_ASSERT_FALSE(web::AuthRequired("", kPassword));
    TEST_ASSERT_FALSE(web::AuthRequired(kUser, nullptr));
    TEST_ASSERT_FALSE(web::AuthRequired(nullptr, kPassword));
}

void test_both_halves_switch_it_on(void) {
    TEST_ASSERT_TRUE(web::AuthRequired(kUser, kPassword));
}

void test_an_unconfigured_site_lets_everyone_in(void) {
    TEST_ASSERT_TRUE(web::Authorised("", "", nullptr));
    TEST_ASSERT_TRUE(web::Authorised("", "", "Basic bm9uc2Vuc2U="));
}

void test_the_right_credential_gets_in(void) {
    TEST_ASSERT_TRUE(web::Authorised(kUser, kPassword, kHeader));
}

void test_no_header_at_all_is_refused(void) {
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, nullptr));
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, ""));
}

void test_a_wrong_password_is_refused(void) {
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Basic YWRtaW46c2VjcmV6"));
}

void test_a_wrong_user_is_refused(void) {
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Basic YWRtb246c2VjcmV0"));
}

void test_the_colon_cannot_be_moved(void) {
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Basic YWRtaTpuc2VjcmV0"));
}

void test_another_scheme_is_refused(void) {
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Bearer YWRtaW46c2VjcmV0"));
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Digest YWRtaW46c2VjcmV0"));
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "YWRtaW46c2VjcmV0"));
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "BasicX YWRtaW46c2VjcmV0"));
}

void test_the_scheme_is_case_insensitive_and_the_credential_is_not(void) {
    TEST_ASSERT_TRUE(web::Authorised(kUser, kPassword, "basic YWRtaW46c2VjcmV0"));
    TEST_ASSERT_TRUE(web::Authorised(kUser, kPassword, "BASIC YWRtaW46c2VjcmV0"));
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Basic ywrtaw46c2vjcmv0"));
}

void test_whitespace_around_the_credential_is_tolerated(void) {
    TEST_ASSERT_TRUE(web::Authorised(kUser, kPassword, "Basic   YWRtaW46c2VjcmV0"));
    TEST_ASSERT_TRUE(web::Authorised(kUser, kPassword, "Basic YWRtaW46c2VjcmV0  "));
    TEST_ASSERT_TRUE(web::Authorised(kUser, kPassword, "Basic\tYWRtaW46c2VjcmV0\r\n"));
}

void test_nothing_after_the_scheme_is_refused(void) {
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Basic"));
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Basic "));
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Basic     "));
}

void test_a_credential_that_only_starts_right_is_refused(void) {
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Basic YWRtaW46c2VjcmV"));
    TEST_ASSERT_FALSE(web::Authorised(kUser, kPassword, "Basic YWRtaW46c2VjcmV0x"));
}

void test_the_encoder_agrees_with_the_rest_of_the_world(void) {
    struct Case {
        const char *user;
        const char *password;
        const char *expected;
    };
    const Case cases[] = {
        {"admin", "secret", "YWRtaW46c2VjcmV0"},  // 12 bytes, no padding
        {"a", "b", "YTpi"},                       // 3 bytes, no padding
        {"a", "bc", "YTpiYw=="},                  // 4 bytes, two pad
        {"a", "bcd", "YTpiY2Q="},                 // 5 bytes, one pad
        {"", "", "Og=="},                         // just the colon
    };
    for (const Case &item : cases) {
        char out[web::kMaxEncodedSize] = {};
        TEST_ASSERT_TRUE_MESSAGE(
            web::BasicCredential(item.user, item.password, out, sizeof out), item.expected);
        TEST_ASSERT_EQUAL_STRING(item.expected, out);
    }
}

void test_the_longest_credential_this_device_holds_still_encodes(void) {
    char user[33] = {};
    char password[65] = {};
    std::memset(user, 'u', sizeof user - 1);
    std::memset(password, 'p', sizeof password - 1);

    char out[web::kMaxEncodedSize] = {};
    TEST_ASSERT_TRUE(web::BasicCredential(user, password, out, sizeof out));
    TEST_ASSERT_EQUAL(0, std::strlen(out) % 4);
}

void test_a_credential_too_long_to_encode_is_refused_not_truncated(void) {
    char password[web::kMaxCredentialSize + 8] = {};
    std::memset(password, 'p', sizeof password - 1);

    char out[web::kMaxEncodedSize] = {'!', '\0'};
    TEST_ASSERT_FALSE(web::BasicCredential("admin", password, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("!", out);

    TEST_ASSERT_FALSE(web::Authorised("admin", password, "Basic YWRtaW46c2VjcmV0"));
}

void test_a_buffer_too_small_is_refused(void) {
    char out[4] = {};
    TEST_ASSERT_FALSE(web::BasicCredential("admin", "secret", out, sizeof out));
    TEST_ASSERT_FALSE(web::BasicCredential("admin", "secret", nullptr, 64));
}

void test_a_colon_in_the_user_is_refused(void) {
    char out[web::kMaxEncodedSize] = {};
    TEST_ASSERT_FALSE(web::BasicCredential("ad:min", "secret", out, sizeof out));
    TEST_ASSERT_TRUE(web::AuthRequired("ad:min", "secret"));
    TEST_ASSERT_FALSE(web::Authorised("ad:min", "secret", "Basic YWQ6bWluOnNlY3JldA=="));
}

void test_a_password_may_contain_a_colon(void) {
    char out[web::kMaxEncodedSize] = {};
    TEST_ASSERT_TRUE(web::BasicCredential("a", "b:c", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("YTpiOmM=", out);
    TEST_ASSERT_TRUE(web::Authorised("a", "b:c", "Basic YTpiOmM="));
}

void RegisterWebAuthTests(void) {
    RUN_TEST(test_no_credentials_in_the_config_is_no_authentication);
    RUN_TEST(test_half_a_credential_is_not_authentication);
    RUN_TEST(test_both_halves_switch_it_on);

    RUN_TEST(test_an_unconfigured_site_lets_everyone_in);
    RUN_TEST(test_the_right_credential_gets_in);
    RUN_TEST(test_no_header_at_all_is_refused);
    RUN_TEST(test_a_wrong_password_is_refused);
    RUN_TEST(test_a_wrong_user_is_refused);
    RUN_TEST(test_the_colon_cannot_be_moved);
    RUN_TEST(test_another_scheme_is_refused);
    RUN_TEST(test_the_scheme_is_case_insensitive_and_the_credential_is_not);
    RUN_TEST(test_whitespace_around_the_credential_is_tolerated);
    RUN_TEST(test_nothing_after_the_scheme_is_refused);
    RUN_TEST(test_a_credential_that_only_starts_right_is_refused);

    RUN_TEST(test_the_encoder_agrees_with_the_rest_of_the_world);
    RUN_TEST(test_the_longest_credential_this_device_holds_still_encodes);
    RUN_TEST(test_a_credential_too_long_to_encode_is_refused_not_truncated);
    RUN_TEST(test_a_buffer_too_small_is_refused);
    RUN_TEST(test_a_colon_in_the_user_is_refused);
    RUN_TEST(test_a_password_may_contain_a_colon);
}
