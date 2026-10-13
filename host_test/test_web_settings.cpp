// web_settings: the whitelist, write-only passwords, all-or-nothing, actions.

#include <cstring>

#include "config.h"
#include "unity.h"
#include "web_settings.h"

namespace {

using web::WriteResult;

config::Data Base() {
    config::Data d;
    config::FillDefaults(&d);
    char error[config::kErrorSize] = {};
    TEST_ASSERT_TRUE(config::SetNetwork(&d.wifi, "home", "password123", error, sizeof error));
    TEST_ASSERT_TRUE(config::SetNetwork(&d.wifi, "office", "", error, sizeof error));
    return d;
}

web::WriteOutcome Apply(const char *json, config::Data *d) {
    return web::ApplySettings(json, std::strlen(json), d);
}

void AssertRefused(const char *json, WriteResult result, const char *detail) {
    config::Data d = Base();
    const config::Data before = d;
    const web::WriteOutcome out = Apply(json, &d);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(result), static_cast<int>(out.result), json);
    if (detail != nullptr) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(detail, out.detail, json);
    }
    TEST_ASSERT_FALSE(out.wifi_changed);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, &d, sizeof d, json);
}

void test_mode_words(void) {
    config::Data d = Base();
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(Apply(R"({"wifi":{"mode":"apsta"}})", &d).result));
    TEST_ASSERT_TRUE(d.wifi.active);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(config::WifiMode::kApSta), static_cast<int>(d.wifi.mode));

    Apply(R"({"wifi":{"mode":"off"}})", &d);
    TEST_ASSERT_FALSE(d.wifi.active);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(config::WifiMode::kApSta), static_cast<int>(d.wifi.mode));

    Apply(R"({"wifi":{"mode":"ap"}})", &d);
    TEST_ASSERT_TRUE(d.wifi.active);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(config::WifiMode::kAp), static_cast<int>(d.wifi.mode));

    AssertRefused(R"({"wifi":{"mode":"yes"}})", WriteResult::kBadValue, "yes");
    AssertRefused(R"({"wifi":{"mode":1}})", WriteResult::kBadValue, "wifi.mode");
}

void test_an_absent_password_keeps_the_one_there_matched_by_ssid(void) {
    config::Data d = Base();
    // Reordered, no passwords: both keep theirs.
    const web::WriteOutcome out =
        Apply(R"({"wifi":{"networks":[{"ssid":"office"},{"ssid":"home","password":null}]}})", &d);
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(out.result));
    TEST_ASSERT_TRUE(out.wifi_changed);
    TEST_ASSERT_EQUAL_UINT8(2, d.wifi.network_count);
    TEST_ASSERT_EQUAL_STRING("office", d.wifi.networks[0].ssid);
    TEST_ASSERT_EQUAL_STRING("", d.wifi.networks[0].password);
    TEST_ASSERT_EQUAL_STRING("home", d.wifi.networks[1].ssid);
    TEST_ASSERT_EQUAL_STRING("password123", d.wifi.networks[1].password);
}

void test_an_empty_password_clears_and_a_new_one_replaces(void) {
    config::Data d = Base();
    Apply(R"({"wifi":{"networks":[{"ssid":"home","password":""},{"ssid":"new","password":"abcdefgh"}]}})",
          &d);
    TEST_ASSERT_EQUAL_STRING("", d.wifi.networks[0].password);
    TEST_ASSERT_EQUAL_STRING("new", d.wifi.networks[1].ssid);
    TEST_ASSERT_EQUAL_STRING("abcdefgh", d.wifi.networks[1].password);
}

void test_a_renamed_network_does_not_inherit_a_password(void) {
    config::Data d = Base();
    Apply(R"({"wifi":{"networks":[{"ssid":"home2"}]}})", &d);
    TEST_ASSERT_EQUAL_UINT8(1, d.wifi.network_count);
    TEST_ASSERT_EQUAL_STRING("", d.wifi.networks[0].password);
}

void test_the_ap_block(void) {
    config::Data d = Base();
    TEST_ASSERT_EQUAL_INT(
        0, static_cast<int>(
               Apply(R"({"wifi":{"ap":{"ssid":"mine","password":"12345678","channel":11}}})", &d)
                   .result));
    TEST_ASSERT_EQUAL_STRING("mine", d.wifi.ap_ssid);
    TEST_ASSERT_EQUAL_STRING("12345678", d.wifi.ap_password);
    TEST_ASSERT_EQUAL_UINT8(11, d.wifi.ap_channel);

    Apply(R"({"wifi":{"ap":{"ssid":"other"}}})", &d);  // password kept
    TEST_ASSERT_EQUAL_STRING("12345678", d.wifi.ap_password);
    Apply(R"({"wifi":{"ap":{"password":""}}})", &d);  // open
    TEST_ASSERT_EQUAL_STRING("", d.wifi.ap_password);
}

void test_values_config_would_refuse_are_refused(void) {
    AssertRefused(R"({"wifi":{"ap":{"password":"short"}}})", WriteResult::kBadValue,
                  "wifi.ap.password");
    AssertRefused(R"({"wifi":{"ap":{"ssid":""}}})", WriteResult::kBadValue, "wifi.ap.ssid");
    AssertRefused(R"({"wifi":{"ap":{"channel":14}}})", WriteResult::kBadValue, "wifi.ap.channel");
    AssertRefused(R"({"wifi":{"ap":{"channel":"6"}}})", WriteResult::kBadValue, "wifi.ap.channel");
    AssertRefused(R"({"wifi":{"networks":[{"ssid":"x","password":"short"}]}})",
                  WriteResult::kBadValue, "wifi.networks[].password");
    AssertRefused(R"({"wifi":{"networks":[{"ssid":""}]}})", WriteResult::kBadValue,
                  "wifi.networks[].ssid");
    AssertRefused(R"({"wifi":{"networks":[{"password":"abcdefgh"}]}})", WriteResult::kBadValue,
                  "wifi.networks[].ssid");
    AssertRefused(R"({"wifi":{"networks":[{"ssid":"a"},{"ssid":"a"}]}})", WriteResult::kBadValue,
                  "a");
    AssertRefused(
        R"({"wifi":{"networks":[{"ssid":"a"},{"ssid":"b"},{"ssid":"c"},{"ssid":"d"},{"ssid":"e"}]}})",
        WriteResult::kTooMany, "wifi.networks");
    AssertRefused(R"({"wifi":{"networks":[{"ssid":"123456789012345678901234567890123"}]}})",
                  WriteResult::kTooLong, "wifi.networks[].ssid");
}

void test_anything_off_the_whitelist_is_refused_by_name(void) {
    AssertRefused(R"({"web":{"user":"attacker","password":"x"}})", WriteResult::kUnknownField,
                  "web");
    AssertRefused(R"({"web":{"write":true}})", WriteResult::kUnknownField, "web");
    AssertRefused(R"({"ble":{"active":true}})", WriteResult::kUnknownField, "ble");
    AssertRefused(R"({"wifi":{"hostname":"x"}})", WriteResult::kUnknownField, "hostname");
    AssertRefused(R"({"wifi":{"active":true}})", WriteResult::kUnknownField, "active");
    AssertRefused(R"({"wifi":{"ap":{"hidden":true}}})", WriteResult::kUnknownField, "hidden");
    AssertRefused(R"({"wifi":{"networks":[{"ssid":"a","ip":{}}]}})", WriteResult::kUnknownField,
                  "ip");
}

void test_one_bad_field_refuses_the_whole_document(void) {
    // The mode is fine and comes first; the network after it is not.
    AssertRefused(R"({"wifi":{"mode":"ap","networks":[{"ssid":""}]}})", WriteResult::kBadValue,
                  "wifi.networks[].ssid");
}

void test_not_a_document(void) {
    AssertRefused("{", WriteResult::kNotJson, nullptr);
    AssertRefused("[]", WriteResult::kNotObject, nullptr);
    AssertRefused(R"({"wifi":[]})", WriteResult::kBadValue, "wifi");
    config::Data d = Base();
    char big[web::kMaxSettingsBody + 2];
    std::memset(big, ' ', sizeof big);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WriteResult::kTooBig),
                          static_cast<int>(web::ApplySettings(big, sizeof big, &d).result));
}

void test_actions(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(web::Action::kSave),
                          static_cast<int>(web::ActionFromUri("/api/action?do=save")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(web::Action::kReload),
                          static_cast<int>(web::ActionFromUri("/api/action?x=1&do=reload")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(web::Action::kWifiRetry),
                          static_cast<int>(web::ActionFromUri("/api/action?do=retry#x")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(web::Action::kNone),
                          static_cast<int>(web::ActionFromUri("/api/action?do=saves")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(web::Action::kNone),
                          static_cast<int>(web::ActionFromUri("/api/action")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(web::Action::kNone),
                          static_cast<int>(web::ActionFromUri(nullptr)));
}

}  // namespace

void RegisterWebSettingsTests(void) {
    RUN_TEST(test_mode_words);
    RUN_TEST(test_an_absent_password_keeps_the_one_there_matched_by_ssid);
    RUN_TEST(test_an_empty_password_clears_and_a_new_one_replaces);
    RUN_TEST(test_a_renamed_network_does_not_inherit_a_password);
    RUN_TEST(test_the_ap_block);
    RUN_TEST(test_values_config_would_refuse_are_refused);
    RUN_TEST(test_anything_off_the_whitelist_is_refused_by_name);
    RUN_TEST(test_one_bad_field_refuses_the_whole_document);
    RUN_TEST(test_not_a_document);
    RUN_TEST(test_actions);
}
