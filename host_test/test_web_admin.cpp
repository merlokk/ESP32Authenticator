// web_admin: the password and FIDO2 pages' requests.

#include <cstring>

#include "unity.h"
#include "web_admin.h"

namespace {

web::AdminRequest g_request;

web::BodyOutcome Parse(const char *body) {
    return web::ParseAdminRequest(body, std::strlen(body), &g_request);
}

bool AllZero(const void *data, size_t size) {
    const unsigned char *p = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < size; ++i) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

void test_a_record_with_every_field(void) {
    const web::BodyOutcome o =
        Parse("{\"id\":7,\"site\":\"example.com\",\"user\":\"me\",\"password\":\"p\\\"w\","
              "\"otp\":\"JBSW Y3DP\"}");
    TEST_ASSERT_EQUAL(web::BodyResult::kOk, o.result);
    TEST_ASSERT_TRUE(g_request.has_id);
    TEST_ASSERT_EQUAL_UINT32(7, g_request.id);
    for (bool has : g_request.has) {
        TEST_ASSERT_TRUE(has);
    }
    TEST_ASSERT_EQUAL_STRING("example.com", web::FieldValue(g_request, vault::Field::kSite));
    TEST_ASSERT_EQUAL_STRING("me", web::FieldValue(g_request, vault::Field::kUser));
    TEST_ASSERT_EQUAL_STRING("p\"w", web::FieldValue(g_request, vault::Field::kPassword));
    TEST_ASSERT_EQUAL_STRING("JBSW Y3DP", web::FieldValue(g_request, vault::Field::kOtp));
    TEST_ASSERT_FALSE(g_request.has_pin);
    TEST_ASSERT_FALSE(g_request.has_credential);
}

void test_an_absent_field_is_not_an_empty_one(void) {
    TEST_ASSERT_EQUAL(web::BodyResult::kOk, Parse("{\"id\":1,\"password\":\"\"}").result);
    TEST_ASSERT_TRUE(g_request.has[static_cast<size_t>(vault::Field::kPassword)]);
    TEST_ASSERT_FALSE(g_request.has[static_cast<size_t>(vault::Field::kOtp)]);
    TEST_ASSERT_FALSE(g_request.has[static_cast<size_t>(vault::Field::kSite)]);
}

void test_an_unknown_field_is_refused_by_name_and_nothing_is_kept(void) {
    const web::BodyOutcome o = Parse("{\"password\":\"secret\",\"admin\":true}");
    TEST_ASSERT_EQUAL(web::BodyResult::kUnknownField, o.result);
    TEST_ASSERT_EQUAL_STRING("admin", o.detail);
    TEST_ASSERT_TRUE(AllZero(&g_request, sizeof g_request));
}

void test_bad_ids_are_refused(void) {
    const char *bad[] = {"{\"id\":0}", "{\"id\":-1}", "{\"id\":1.5}", "{\"id\":\"3\"}",
                         "{\"id\":4294967296}"};
    for (const char *body : bad) {
        TEST_ASSERT_EQUAL_MESSAGE(web::BodyResult::kBadValue, Parse(body).result, body);
    }
    TEST_ASSERT_EQUAL(web::BodyResult::kOk, Parse("{\"id\":4294967295}").result);
    TEST_ASSERT_EQUAL_UINT32(4294967295u, g_request.id);
}

void test_too_long_text_is_refused_not_cut(void) {
    char body[400] = "{\"site\":\"";
    std::memset(body + std::strlen(body), 'a', vault::kSiteMax + 1);
    std::strcat(body, "\"}");
    const web::BodyOutcome o = Parse(body);
    TEST_ASSERT_EQUAL(web::BodyResult::kTooLong, o.result);
    TEST_ASSERT_EQUAL_STRING("site", o.detail);
    TEST_ASSERT_EQUAL(web::BodyResult::kTooLong, Parse("{\"pin\":\"1234567\"}").result);
    TEST_ASSERT_EQUAL(web::BodyResult::kOk, Parse("{\"pin\":\"123456\"}").result);
    TEST_ASSERT_TRUE(g_request.has_pin);
    TEST_ASSERT_EQUAL_STRING("123456", g_request.pin);
}

void test_not_json_or_not_an_object(void) {
    TEST_ASSERT_EQUAL(web::BodyResult::kNotJson, Parse("site=x").result);
    TEST_ASSERT_EQUAL(web::BodyResult::kNotObject, Parse("[1]").result);
    TEST_ASSERT_EQUAL(web::BodyResult::kBadValue, Parse("{\"site\":5}").result);
    TEST_ASSERT_EQUAL(web::BodyResult::kOk, Parse("{}").result);
}

void test_a_credential_id_is_64_hex_digits(void) {
    const char *hex = "00112233445566778899AABBCCDDEEFF00112233445566778899aabbccddeeff";
    char body[128];
    std::strcpy(body, "{\"credential\":\"");
    std::strcat(body, hex);
    std::strcat(body, "\"}");
    TEST_ASSERT_EQUAL(web::BodyResult::kOk, Parse(body).result);
    TEST_ASSERT_TRUE(g_request.has_credential);
    TEST_ASSERT_EQUAL_UINT(0x00u, g_request.credential[0]);
    TEST_ASSERT_EQUAL_UINT(0xAAu, g_request.credential[10]);
    TEST_ASSERT_EQUAL_UINT(0xFFu, g_request.credential[31]);
    uint8_t id[web::kCredentialIdSize];
    TEST_ASSERT_FALSE(web::ParseCredentialId("0011", id));
    TEST_ASSERT_FALSE(web::ParseCredentialId(
        "g0112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", id));
    TEST_ASSERT_EQUAL(web::BodyResult::kBadValue, Parse("{\"credential\":\"00\"}").result);
}

void test_the_verb_is_the_last_path_part(void) {
    TEST_ASSERT_EQUAL(web::VaultVerb::kAdd, web::VaultVerbFromUri("/api/vault/add"));
    TEST_ASSERT_EQUAL(web::VaultVerb::kSet, web::VaultVerbFromUri("/api/vault/set?x=1"));
    TEST_ASSERT_EQUAL(web::VaultVerb::kDelete, web::VaultVerbFromUri("/api/vault/delete"));
    TEST_ASSERT_EQUAL(web::VaultVerb::kUnlock, web::VaultVerbFromUri("/api/vault/unlock"));
    TEST_ASSERT_EQUAL(web::VaultVerb::kLock, web::VaultVerbFromUri("/api/vault/lock#"));
    TEST_ASSERT_EQUAL(web::VaultVerb::kNone, web::VaultVerbFromUri("/api/vault/locked"));
    TEST_ASSERT_EQUAL(web::VaultVerb::kNone, web::VaultVerbFromUri("/api/vault/"));
    TEST_ASSERT_EQUAL(web::VaultVerb::kNone, web::VaultVerbFromUri("/api/fido/delete"));
    TEST_ASSERT_EQUAL(web::VaultVerb::kNone, web::VaultVerbFromUri(nullptr));
    TEST_ASSERT_EQUAL_STRING("unlock", web::VaultVerbName(web::VaultVerb::kUnlock));
}

void test_only_json_is_taken(void) {
    TEST_ASSERT_TRUE(web::IsJsonType("application/json"));
    TEST_ASSERT_TRUE(web::IsJsonType("Application/JSON; charset=utf-8"));
    TEST_ASSERT_FALSE(web::IsJsonType("text/plain"));
    TEST_ASSERT_FALSE(web::IsJsonType("application/x-www-form-urlencoded"));
    TEST_ASSERT_FALSE(web::IsJsonType("application/jsonp"));
    TEST_ASSERT_FALSE(web::IsJsonType(nullptr));
}

void test_the_id_in_a_query(void) {
    uint32_t id = 0;
    TEST_ASSERT_TRUE(web::IdFromQuery("/api/vault/totp?id=12", &id));
    TEST_ASSERT_EQUAL_UINT32(12, id);
    TEST_ASSERT_TRUE(web::IdFromQuery("/api/vault/totp?x=1&id=4294967295", &id));
    TEST_ASSERT_EQUAL_UINT32(4294967295u, id);
    TEST_ASSERT_FALSE(web::IdFromQuery("/api/vault/totp?id=0", &id));
    TEST_ASSERT_FALSE(web::IdFromQuery("/api/vault/totp?id=4294967296", &id));
    TEST_ASSERT_FALSE(web::IdFromQuery("/api/vault/totp?id=1x", &id));
    TEST_ASSERT_FALSE(web::IdFromQuery("/api/vault/totp?id=", &id));
    TEST_ASSERT_FALSE(web::IdFromQuery("/api/vault/totp?pid=3", &id));
    TEST_ASSERT_FALSE(web::IdFromQuery("/api/vault/totp", &id));
}

void RegisterWebAdminTests(void) {
    RUN_TEST(test_a_record_with_every_field);
    RUN_TEST(test_an_absent_field_is_not_an_empty_one);
    RUN_TEST(test_an_unknown_field_is_refused_by_name_and_nothing_is_kept);
    RUN_TEST(test_bad_ids_are_refused);
    RUN_TEST(test_too_long_text_is_refused_not_cut);
    RUN_TEST(test_not_json_or_not_an_object);
    RUN_TEST(test_a_credential_id_is_64_hex_digits);
    RUN_TEST(test_the_verb_is_the_last_path_part);
    RUN_TEST(test_only_json_is_taken);
    RUN_TEST(test_the_id_in_a_query);
}
