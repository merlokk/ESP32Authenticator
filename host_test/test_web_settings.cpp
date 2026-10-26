// web_settings: the whitelist, write-only passwords, all-or-nothing, actions.

#include <cstdio>
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

void test_an_absent_ip_keeps_the_static_address(void) {
    config::Data d = Base();
    config::StaticIp &ip = d.wifi.networks[0].ip;  // "home"
    ip.enabled = true;
    std::snprintf(ip.address, sizeof ip.address, "192.168.1.42");
    std::snprintf(ip.netmask, sizeof ip.netmask, "255.255.255.0");
    std::snprintf(ip.gateway, sizeof ip.gateway, "192.168.1.1");
    const config::StaticIp before = ip;
    // The page sends networks with no ip at all, reordered.
    Apply(R"({"wifi":{"networks":[{"ssid":"office"},{"ssid":"home"}]}})", &d);
    TEST_ASSERT_EQUAL_MEMORY(&before, &d.wifi.networks[1].ip, sizeof before);
    TEST_ASSERT_FALSE(d.wifi.networks[0].ip.enabled);
}

void test_an_ip_block_replaces_and_static_false_is_dhcp(void) {
    config::Data d = Base();
    TEST_ASSERT_EQUAL_INT(
        0, static_cast<int>(Apply(R"({"wifi":{"networks":[{"ssid":"home","ip":{"static":true,
            "address":"10.0.0.5","netmask":"255.0.0.0","gateway":"10.0.0.1","dns1":null}}]}})",
                                  &d)
                                .result));
    TEST_ASSERT_TRUE(d.wifi.networks[0].ip.enabled);
    TEST_ASSERT_EQUAL_STRING("10.0.0.5", d.wifi.networks[0].ip.address);
    TEST_ASSERT_EQUAL_STRING("", d.wifi.networks[0].ip.dns1);

    Apply(R"({"wifi":{"networks":[{"ssid":"home","ip":{"static":false}}]}})", &d);
    TEST_ASSERT_FALSE(d.wifi.networks[0].ip.enabled);
}

void test_a_bad_ip_block_is_refused(void) {
    AssertRefused(R"({"wifi":{"networks":[{"ssid":"home","ip":{"static":true,
        "address":"10.0.0.5","netmask":"255.0.0.0"}}]}})", WriteResult::kBadValue, "gateway");
    AssertRefused(R"({"wifi":{"networks":[{"ssid":"home","ip":{"address":"010.0.0.5"}}]}})",
                  WriteResult::kBadValue, "address");
    AssertRefused(R"({"wifi":{"networks":[{"ssid":"home","ip":{"static":"yes"}}]}})",
                  WriteResult::kBadValue, "static");
    AssertRefused(R"({"wifi":{"networks":[{"ssid":"home","ip":{"mtu":1500}}]}})",
                  WriteResult::kUnknownField, "mtu");
    AssertRefused(R"({"wifi":{"networks":[{"ssid":"home","ip":"dhcp"}]}})", WriteResult::kBadValue,
                  "wifi.networks[].ip");
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
    AssertRefused(R"({"ble":{"active":true}})", WriteResult::kUnknownField, "ble.active");
    AssertRefused(R"({"wifi":{"hostname":"x"}})", WriteResult::kUnknownField, "hostname");
    AssertRefused(R"({"wifi":{"active":true}})", WriteResult::kUnknownField, "active");
    AssertRefused(R"({"wifi":{"ap":{"hidden":true}}})", WriteResult::kUnknownField, "hidden");
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

void test_ble_name(void) {
    config::Data d = Base();
    web::WriteOutcome out = Apply(R"({"ble": {"name": "ESP32 Auth X4Pro"}})", &d);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WriteResult::kOk), static_cast<int>(out.result));
    TEST_ASSERT_TRUE(out.ble_changed);
    TEST_ASSERT_FALSE(out.wifi_changed);
    TEST_ASSERT_EQUAL_STRING("ESP32 Auth X4Pro", d.ble.name);
    // Only the name: the site cannot switch the radio or anything else here.
    AssertRefused(R"({"ble": {"active": true}})", WriteResult::kUnknownField, "ble.active");
    AssertRefused(R"({"ble": {"name": ""}})", WriteResult::kBadValue, "ble.name");
    AssertRefused(R"({"ble": {"name": 5}})", WriteResult::kBadValue, "ble.name");
    AssertRefused(R"({"ble": {"name": "a name of thirty bytes, too lo"}})", WriteResult::kBadValue,
                  "ble.name");
    AssertRefused(R"({"ble": "x"})", WriteResult::kBadValue, "ble");
    // A refusal elsewhere drops the name too.
    d = Base();
    out = Apply(R"({"ble": {"name": "New"}, "web": {}})", &d);
    TEST_ASSERT_FALSE(out.ble_changed);
    TEST_ASSERT_EQUAL_STRING("", d.ble.name);
}

void test_display_wallpaper(void) {
    config::Data d = Base();
    web::WriteOutcome out = Apply(R"({"display": {"wallpaper": "bw"}})", &d);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WriteResult::kOk), static_cast<int>(out.result));
    TEST_ASSERT_TRUE(d.display.wallpaper == config::WallpaperMode::kBw);
    TEST_ASSERT_FALSE(out.wifi_changed);
    TEST_ASSERT_FALSE(out.ble_changed);
    out = Apply(R"({"display": {"wallpaper": "grey"}})", &d);
    TEST_ASSERT_TRUE(d.display.wallpaper == config::WallpaperMode::kGrey);
    AssertRefused(R"({"display": {"wallpaper": "gray"}})", WriteResult::kBadValue,
                  "display.wallpaper");
    AssertRefused(R"({"display": {"wallpaper": 1}})", WriteResult::kBadValue, "display.wallpaper");
    AssertRefused(R"({"display": {"other": "bw"}})", WriteResult::kUnknownField, "display.other");
    out = Apply(R"({"display": {"fastRefreshes": 0, "fullFromPercent": 100}})", &d);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WriteResult::kOk), static_cast<int>(out.result));
    TEST_ASSERT_EQUAL_UINT16(0, d.display.fast_refreshes);
    TEST_ASSERT_EQUAL_UINT8(100, d.display.full_from_percent);
    out = Apply(R"({"display": {"fastRefreshes": 200, "fullFromPercent": 0}})", &d);
    TEST_ASSERT_EQUAL_UINT16(200, d.display.fast_refreshes);
    TEST_ASSERT_EQUAL_UINT8(0, d.display.full_from_percent);
    AssertRefused(R"({"display": {"fastRefreshes": 201}})", WriteResult::kBadValue,
                  "display.fastRefreshes");
    AssertRefused(R"({"display": {"fastRefreshes": 1.5}})", WriteResult::kBadValue,
                  "display.fastRefreshes");
    AssertRefused(R"({"display": {"fullFromPercent": 101}})", WriteResult::kBadValue,
                  "display.fullFromPercent");
    AssertRefused(R"({"display": {"fullFromPercent": "3"}})", WriteResult::kBadValue,
                  "display.fullFromPercent");
    AssertRefused(R"({"display": "bw"})", WriteResult::kBadValue, "display");
    // A refusal elsewhere drops it too.
    d = Base();
    out = Apply(R"({"display": {"wallpaper": "bw"}, "web": {}})", &d);
    TEST_ASSERT_TRUE(d.display.wallpaper == config::WallpaperMode::kGrey);
}

void test_frontlight_level(void) {
    config::Data d = Base();
    web::WriteOutcome out = Apply(R"({"frontlight": {"brightness": 60, "warmth": 0}})", &d);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WriteResult::kOk), static_cast<int>(out.result));
    TEST_ASSERT_TRUE(out.frontlight_changed);
    TEST_ASSERT_EQUAL_UINT8(60, d.frontlight.brightness);
    TEST_ASSERT_EQUAL_UINT8(0, d.frontlight.warmth);
    out = Apply(R"({"frontlight": {"warmth": 100}})", &d);
    TEST_ASSERT_EQUAL_UINT8(60, d.frontlight.brightness);
    TEST_ASSERT_EQUAL_UINT8(100, d.frontlight.warmth);
    AssertRefused(R"({"frontlight": {"brightness": 0}})", WriteResult::kBadValue,
                  "frontlight.brightness");
    AssertRefused(R"({"frontlight": {"brightness": 101}})", WriteResult::kBadValue,
                  "frontlight.brightness");
    AssertRefused(R"({"frontlight": {"warmth": 50.5}})", WriteResult::kBadValue,
                  "frontlight.warmth");
    AssertRefused(R"({"frontlight": {"warmth": "50"}})", WriteResult::kBadValue,
                  "frontlight.warmth");
    AssertRefused(R"({"frontlight": {"on": true}})", WriteResult::kUnknownField, "frontlight.on");
    AssertRefused(R"({"frontlight": 5})", WriteResult::kBadValue, "frontlight");
    d = Base();
    out = Apply(R"({"frontlight": {"brightness": 60}, "web": {}})", &d);
    TEST_ASSERT_FALSE(out.frontlight_changed);
    TEST_ASSERT_EQUAL_UINT8(20, d.frontlight.brightness);
}

void test_power(void) {
    config::Data d = Base();
    web::WriteOutcome out = Apply(R"({"power": {"idleMinutes": 0, "holdRail": false}})", &d);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WriteResult::kOk), static_cast<int>(out.result));
    TEST_ASSERT_EQUAL_UINT16(0, d.power.idle_minutes);
    TEST_ASSERT_FALSE(d.power.hold_rail);
    TEST_ASSERT_FALSE(out.frontlight_changed);
    Apply(R"({"power": {"idleMinutes": 1440}})", &d);
    TEST_ASSERT_EQUAL_UINT16(1440, d.power.idle_minutes);
    TEST_ASSERT_FALSE(d.power.hold_rail);
    AssertRefused(R"({"power": {"idleMinutes": 1441}})", WriteResult::kBadValue,
                  "power.idleMinutes");
    AssertRefused(R"({"power": {"idleMinutes": -1}})", WriteResult::kBadValue,
                  "power.idleMinutes");
    AssertRefused(R"({"power": {"holdRail": 1}})", WriteResult::kBadValue, "power.holdRail");
    // The PIN's hours stay a console matter.
    AssertRefused(R"({"power": {"keepPinHours": 168}})", WriteResult::kUnknownField,
                  "power.keepPinHours");
    AssertRefused(R"({"power": []})", WriteResult::kBadValue, "power");
}

void test_light_actions(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(web::Action::kLightOn),
                          static_cast<int>(web::ActionFromUri("/api/action?do=light-on")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(web::Action::kLightOff),
                          static_cast<int>(web::ActionFromUri("/api/action?do=light-off")));
    TEST_ASSERT_EQUAL_STRING("light-on", web::ActionName(web::Action::kLightOn));
}

}  // namespace

void RegisterWebSettingsTests(void) {
    RUN_TEST(test_mode_words);
    RUN_TEST(test_an_absent_password_keeps_the_one_there_matched_by_ssid);
    RUN_TEST(test_an_empty_password_clears_and_a_new_one_replaces);
    RUN_TEST(test_a_renamed_network_does_not_inherit_a_password);
    RUN_TEST(test_the_ap_block);
    RUN_TEST(test_an_absent_ip_keeps_the_static_address);
    RUN_TEST(test_an_ip_block_replaces_and_static_false_is_dhcp);
    RUN_TEST(test_a_bad_ip_block_is_refused);
    RUN_TEST(test_values_config_would_refuse_are_refused);
    RUN_TEST(test_anything_off_the_whitelist_is_refused_by_name);
    RUN_TEST(test_one_bad_field_refuses_the_whole_document);
    RUN_TEST(test_not_a_document);
    RUN_TEST(test_actions);
    RUN_TEST(test_ble_name);
    RUN_TEST(test_display_wallpaper);
    RUN_TEST(test_frontlight_level);
    RUN_TEST(test_power);
    RUN_TEST(test_light_actions);
}
