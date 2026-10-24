// config: parsing, validation, serialization, network list, and the file
// side (defaults when missing/invalid, atomic save, interrupted-write
// recovery) on a real directory under build/.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "config.h"
#include "unity.h"

namespace {

// Relative and short: config::kMaxDirSize is 32, as on the device.
constexpr const char *kDir = "build/fs";

std::string FilePath(const char *name) { return std::string(kDir) + "/" + name; }

void MountEmptyDir() {
    std::error_code ec;
    std::filesystem::remove_all(kDir, ec);
    std::filesystem::create_directories(kDir, ec);
}

void PutFile(const char *name, const char *contents) {
    FILE *f = std::fopen(FilePath(name).c_str(), "wb");
    TEST_ASSERT_NOT_NULL(f);
    std::fwrite(contents, 1, std::strlen(contents), f);
    std::fclose(f);
}

bool Exists(const char *name) { return std::filesystem::exists(FilePath(name)); }

std::string ReadFile(const char *name) {
    FILE *f = std::fopen(FilePath(name).c_str(), "rb");
    if (f == nullptr) {
        return {};
    }
    std::string out;
    char buf[256];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return out;
}

// Parses `json` over the defaults; returns the result and the error text.
bool ParseText(const char *json, config::Data *out, char *error, size_t error_size) {
    config::FillDefaults(out);
    return config::Parse(json, std::strlen(json), out, error, error_size);
}

void AssertParseFails(const char *json, const char *expected_in_error) {
    config::Data d;
    char error[config::kErrorSize] = {};
    TEST_ASSERT_FALSE_MESSAGE(ParseText(json, &d, error, sizeof error), json);
    TEST_ASSERT_NOT_NULL_MESSAGE(std::strstr(error, expected_in_error), error);
}

void AssertDefaults(const config::Data &d) {
    TEST_ASSERT_FALSE(d.wifi.active);
    TEST_ASSERT_EQUAL_STRING("esp32-auth", d.wifi.hostname);
    TEST_ASSERT_EQUAL_UINT8(0, d.wifi.network_count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(config::WifiMode::kClient), static_cast<int>(d.wifi.mode));
    TEST_ASSERT_EQUAL_UINT8(2, d.wifi.rounds);
    TEST_ASSERT_EQUAL_UINT16(120, d.wifi.ap_window_seconds);
    TEST_ASSERT_EQUAL_STRING("esp32-auth", d.wifi.ap_ssid);
    TEST_ASSERT_EQUAL_STRING("esp32-auth", d.wifi.ap_password);
    TEST_ASSERT_EQUAL_UINT8(6, d.wifi.ap_channel);
    TEST_ASSERT_FALSE(d.ble.active);
    TEST_ASSERT_TRUE(d.internet.check);
    TEST_ASSERT_EQUAL_UINT16(60, d.internet.interval_seconds);
    TEST_ASSERT_EQUAL_UINT16(2000, d.internet.timeout_ms);
    TEST_ASSERT_EQUAL_UINT8(2, d.internet.failures);
    TEST_ASSERT_EQUAL_UINT8(3, d.internet.target_count);
    TEST_ASSERT_EQUAL_STRING("8.8.8.8", d.internet.targets[0]);
    TEST_ASSERT_EQUAL_STRING("9.9.9.9", d.internet.targets[2]);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(config::WebMode::kAuto), static_cast<int>(d.web.mode));
    TEST_ASSERT_TRUE(d.web.write);
    TEST_ASSERT_EQUAL_STRING("", d.web.user);
    TEST_ASSERT_EQUAL_STRING("", d.web.password);
    TEST_ASSERT_EQUAL_STRING("UTC", d.time.zone);
    TEST_ASSERT_EQUAL_STRING("pool.ntp.org", d.time.server);
    TEST_ASSERT_EQUAL_UINT8(6, d.time.sync_hours);
    TEST_ASSERT_EQUAL_UINT8(15, d.led.percent);
    TEST_ASSERT_EQUAL_UINT8(7, d.led.idle_percent);
    TEST_ASSERT_EQUAL_STRING("", d.test.pin);
    TEST_ASSERT_FALSE(d.test.auto_presence);
    TEST_ASSERT_EQUAL_UINT16(10, d.pin.lock_minutes);
    TEST_ASSERT_EQUAL_UINT16(10, d.power.idle_minutes);
    TEST_ASSERT_EQUAL_UINT8(0, d.power.keep_pin_hours);
    TEST_ASSERT_TRUE(d.power.hold_rail);
    TEST_ASSERT_TRUE(d.pin.complexity);
    TEST_ASSERT_EQUAL_STRING("", d.pin.wipe);
}

constexpr const char *kFullJson = R"json({
  "wifi": {
    "active": true,
    "mode": "apsta",
    "hostname": "auth-1",
    "rounds": 3,
    "apWindowSeconds": 300,
    "ap": {"ssid": "My AP", "password": "", "channel": 11},
    "networks": [
      {"ssid": "Home", "password": "password123"},
      {"ssid": "Office WiFi", "password": ""}
    ]
  },
  "ble": {"active": true, "name": "Desk key"},
  "web": {"mode": "on", "write": false, "user": "admin", "password": "a:b c"}
})json";

// --- Defaults ------------------------------------------------------------------

void test_defaults_parse() {
    config::Data d;
    char error[config::kErrorSize] = {};
    config::FillDefaults(&d);
    TEST_ASSERT_TRUE_MESSAGE(config::Parse(config::kDefaultJson,
                                           std::strlen(config::kDefaultJson), &d, error,
                                           sizeof error),
                             error);
    AssertDefaults(d);
}

void test_fill_defaults_values() {
    config::Data d;
    std::memset(&d, 0xAB, sizeof d);
    config::FillDefaults(&d);
    AssertDefaults(d);
}

// --- Parse ---------------------------------------------------------------------

void test_parse_full() {
    config::Data d;
    char error[config::kErrorSize] = {};
    TEST_ASSERT_TRUE_MESSAGE(ParseText(kFullJson, &d, error, sizeof error), error);
    TEST_ASSERT_TRUE(d.wifi.active);
    TEST_ASSERT_EQUAL_STRING("auth-1", d.wifi.hostname);
    TEST_ASSERT_EQUAL_UINT8(2, d.wifi.network_count);
    TEST_ASSERT_EQUAL_STRING("Home", d.wifi.networks[0].ssid);
    TEST_ASSERT_EQUAL_STRING("password123", d.wifi.networks[0].password);
    TEST_ASSERT_EQUAL_STRING("Office WiFi", d.wifi.networks[1].ssid);
    TEST_ASSERT_EQUAL_STRING("", d.wifi.networks[1].password);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(config::WifiMode::kApSta), static_cast<int>(d.wifi.mode));
    TEST_ASSERT_EQUAL_UINT8(3, d.wifi.rounds);
    TEST_ASSERT_EQUAL_UINT16(300, d.wifi.ap_window_seconds);
    TEST_ASSERT_EQUAL_STRING("My AP", d.wifi.ap_ssid);
    TEST_ASSERT_EQUAL_STRING("", d.wifi.ap_password);
    TEST_ASSERT_EQUAL_UINT8(11, d.wifi.ap_channel);
    TEST_ASSERT_TRUE(d.ble.active);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(config::WebMode::kOn), static_cast<int>(d.web.mode));
    TEST_ASSERT_FALSE(d.web.write);
    TEST_ASSERT_EQUAL_STRING("admin", d.web.user);
    TEST_ASSERT_EQUAL_STRING("a:b c", d.web.password);
}

void test_parse_internet_check() {
    config::Data d;
    char error[config::kErrorSize] = {};
    TEST_ASSERT_TRUE_MESSAGE(
        ParseText(R"({"internet": {"check": false, "intervalSeconds": 30, "timeoutMs": 500,
            "failures": 3, "targets": ["192.168.1.1"]}})", &d, error, sizeof error),
        error);
    TEST_ASSERT_FALSE(d.internet.check);
    TEST_ASSERT_EQUAL_UINT16(30, d.internet.interval_seconds);
    TEST_ASSERT_EQUAL_UINT16(500, d.internet.timeout_ms);
    TEST_ASSERT_EQUAL_UINT8(3, d.internet.failures);
    TEST_ASSERT_EQUAL_UINT8(1, d.internet.target_count);
    TEST_ASSERT_EQUAL_STRING("192.168.1.1", d.internet.targets[0]);

    TEST_ASSERT_TRUE(ParseText(R"({"internet": {"targets": []}})", &d, error, sizeof error));
    TEST_ASSERT_EQUAL_UINT8(0, d.internet.target_count);

    char json[config::kMaxFileSize];
    config::FillDefaults(&d);
    TEST_ASSERT_GREATER_THAN(0, config::Serialize(d, json, sizeof json));
    config::Data out;
    TEST_ASSERT_TRUE_MESSAGE(ParseText(json, &out, error, sizeof error), error);
    TEST_ASSERT_EQUAL_MEMORY(&d.internet, &out.internet, sizeof d.internet);
}

void test_parse_rejects_bad_internet_check() {
    AssertParseFails(R"({"internet": {"targets": ["dns.google"]}})", "internet.targets[0]");
    AssertParseFails(R"({"internet": {"targets": ["1.1.1.1", "8.8.8.08"]}})", "internet.targets[1]");
    AssertParseFails(R"({"internet": {"targets": ["1.1.1.1", "1.0.0.1", "8.8.8.8", "8.8.4.4",
        "9.9.9.9"]}})", "internet.targets");
    AssertParseFails(R"({"internet": {"targets": "8.8.8.8"}})", "internet.targets");
    AssertParseFails(R"({"internet": {"intervalSeconds": 1}})", "internet.intervalSeconds");
    AssertParseFails(R"({"internet": {"timeoutMs": 50}})", "internet.timeoutMs");
    AssertParseFails(R"({"internet": {"failures": 0}})", "internet.failures");
    AssertParseFails(R"({"internet": {"check": "yes"}})", "internet.check");
    AssertParseFails(R"({"internet": []})", "internet");
}

void test_parse_ipv4() {
    uint32_t value = 0;
    TEST_ASSERT_TRUE(config::ParseIpv4("192.168.1.42", &value));
    TEST_ASSERT_EQUAL_HEX32(0x2A01A8C0, value);  // first octet in the low byte
    TEST_ASSERT_TRUE(config::ParseIpv4("0.0.0.0", &value));
    TEST_ASSERT_TRUE(config::ParseIpv4("255.255.255.255", &value));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFF, value);
    const char *bad[] = {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "010.1.1.1", "1..2.3",
                         " 1.2.3.4", "1.2.3.4 ", "0x7f.0.0.1", "-1.2.3.4", "1.2.3.4\n",
                         "1234.1.1.1"};
    for (const char *text : bad) {
        value = 7;
        TEST_ASSERT_FALSE_MESSAGE(config::ParseIpv4(text, &value), text);
        TEST_ASSERT_EQUAL_UINT32(7, value);
    }
    TEST_ASSERT_FALSE(config::ParseIpv4(nullptr, &value));
}

void test_parse_static_ip() {
    config::Data d;
    char error[config::kErrorSize] = {};
    TEST_ASSERT_TRUE_MESSAGE(
        ParseText(R"({"wifi": {"networks": [{"ssid": "Office", "ip": {"static": true,
            "address": "10.0.0.5", "netmask": "255.255.255.0", "gateway": "10.0.0.1",
            "dns1": "1.1.1.1"}}, {"ssid": "Home"}]}})",
                  &d, error, sizeof error),
        error);
    const config::StaticIp &ip = d.wifi.networks[0].ip;
    TEST_ASSERT_TRUE(ip.enabled);
    TEST_ASSERT_EQUAL_STRING("10.0.0.5", ip.address);
    TEST_ASSERT_EQUAL_STRING("255.255.255.0", ip.netmask);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", ip.gateway);
    TEST_ASSERT_EQUAL_STRING("1.1.1.1", ip.dns1);
    TEST_ASSERT_EQUAL_STRING("", ip.dns2);
    TEST_ASSERT_FALSE(d.wifi.networks[1].ip.enabled);

    // Off, with the typed address kept.
    TEST_ASSERT_TRUE(ParseText(R"({"wifi": {"networks": [{"ssid": "A", "ip": {"static": false,
            "address": "10.0.0.5"}}]}})", &d, error, sizeof error));
    TEST_ASSERT_FALSE(d.wifi.networks[0].ip.enabled);
    TEST_ASSERT_EQUAL_STRING("10.0.0.5", d.wifi.networks[0].ip.address);
}

void test_parse_rejects_bad_static_ip() {
    AssertParseFails(R"({"wifi": {"networks": [{"ssid": "A", "ip": {"static": true,
        "address": "10.0.0.5", "netmask": "255.255.255.0"}}]}})", "gateway");
    AssertParseFails(R"({"wifi": {"networks": [{"ssid": "A", "ip": {"static": true,
        "address": "10.0.0.300", "netmask": "255.255.255.0", "gateway": "10.0.0.1"}}]}})",
                     "address");
    AssertParseFails(R"({"wifi": {"networks": [{"ssid": "A", "ip": {"dns1": "x"}}]}})", "dns1");
    AssertParseFails(R"({"wifi": {"networks": [{"ssid": "A", "ip": []}]}})", "ip");
    AssertParseFails(R"({"wifi": {"networks": [{"ssid": "A", "ip": {"static": 1}}]}})", "static");
}

void test_parse_rejects_bad_wifi_and_web_fields() {
    AssertParseFails(R"({"wifi": {"mode": "station"}})", "wifi.mode");
    AssertParseFails(R"({"wifi": {"rounds": 0}})", "wifi.rounds");
    AssertParseFails(R"({"wifi": {"rounds": 1.5}})", "wifi.rounds");
    AssertParseFails(R"({"wifi": {"apWindowSeconds": 10}})", "wifi.apWindowSeconds");
    AssertParseFails(R"({"wifi": {"ap": []}})", "wifi.ap");
    AssertParseFails(R"({"wifi": {"ap": {"ssid": ""}}})", "wifi.ap.ssid");
    AssertParseFails(R"({"wifi": {"ap": {"password": "short"}}})", "wifi.ap.password");
    AssertParseFails(R"({"wifi": {"ap": {"channel": 14}}})", "wifi.ap.channel");
    AssertParseFails(R"({"web": {"mode": "always"}})", "web.mode");
    AssertParseFails(R"({"web": {"write": 1}})", "web.write");
    AssertParseFails(R"({"web": {"user": "a:b"}})", "web.user");
    AssertParseFails(R"({"web": []})", "web");
}

void test_parse_empty_object_keeps_defaults() {
    config::Data d;
    char error[config::kErrorSize] = {};
    TEST_ASSERT_TRUE(ParseText("{}", &d, error, sizeof error));
    AssertDefaults(d);
}

void test_parse_partial_keeps_other_defaults() {
    config::Data d;
    char error[config::kErrorSize] = {};
    TEST_ASSERT_TRUE(ParseText(R"({"wifi": {"active": true}})", &d, error, sizeof error));
    TEST_ASSERT_TRUE(d.wifi.active);
    TEST_ASSERT_EQUAL_STRING("esp32-auth", d.wifi.hostname);
    TEST_ASSERT_FALSE(d.ble.active);
}

void test_parse_ignores_unknown_fields() {
    config::Data d;
    char error[config::kErrorSize] = {};
    TEST_ASSERT_TRUE(ParseText(R"({"future": 1, "wifi": {"x": "y", "active": true}})", &d, error,
                               sizeof error));
    TEST_ASSERT_TRUE(d.wifi.active);
}

void test_parse_password_optional() {
    config::Data d;
    char error[config::kErrorSize] = {};
    TEST_ASSERT_TRUE(
        ParseText(R"({"wifi": {"networks": [{"ssid": "Open"}]}})", &d, error, sizeof error));
    TEST_ASSERT_EQUAL_UINT8(1, d.wifi.network_count);
    TEST_ASSERT_EQUAL_STRING("", d.wifi.networks[0].password);
}

void test_parse_hex_psk() {
    config::Data d;
    char error[config::kErrorSize] = {};
    const char *json =
        R"({"wifi": {"networks": [{"ssid": "A", "password": )"
        R"("0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef"}]}})";
    TEST_ASSERT_TRUE_MESSAGE(ParseText(json, &d, error, sizeof error), error);
    TEST_ASSERT_EQUAL(64, std::strlen(d.wifi.networks[0].password));
}

void test_parse_failure_leaves_output_unchanged() {
    config::Data d;
    config::FillDefaults(&d);
    d.wifi.active = true;
    char error[config::kErrorSize] = {};
    const char *bad = R"({"wifi": {"active": false, "hostname": "-bad"}})";
    TEST_ASSERT_FALSE(config::Parse(bad, std::strlen(bad), &d, error, sizeof error));
    TEST_ASSERT_TRUE(d.wifi.active);
}

void test_parse_rejects_invalid_json() { AssertParseFails("{\"wifi\": ", "not valid JSON"); }

void test_parse_rejects_non_object() { AssertParseFails("[]", "not a JSON object"); }

void test_parse_rejects_wrong_types() {
    AssertParseFails(R"({"wifi": 1})", "wifi: expected an object");
    AssertParseFails(R"({"wifi": {"active": "yes"}})", "wifi.active");
    AssertParseFails(R"({"wifi": {"hostname": 5}})", "wifi.hostname: expected a string");
    AssertParseFails(R"({"wifi": {"networks": {}}})", "wifi.networks: expected an array");
    AssertParseFails(R"({"wifi": {"networks": ["Home"]}})", "wifi.networks[0]: expected an object");
    AssertParseFails(R"({"ble": {"active": 1}})", "ble.active");
    AssertParseFails(R"({"ble": {"name": 5}})", "ble.name: expected a string");
    AssertParseFails(R"({"ble": {"name": "a name of thirty bytes, too lo"}})", "ble.name");
}

void test_parse_rejects_bad_ssid() {
    AssertParseFails(R"({"wifi": {"networks": [{"password": "password123"}]}})",
                     "wifi.networks[0].ssid: missing");
    AssertParseFails(R"({"wifi": {"networks": [{"ssid": ""}]}})", "wifi.networks[0].ssid");
    AssertParseFails(R"({"wifi": {"networks": [{"ssid": "123456789012345678901234567890123"}]}})",
                     "wifi.networks[0].ssid");
}

void test_parse_accepts_32_byte_ssid() {
    config::Data d;
    char error[config::kErrorSize] = {};
    TEST_ASSERT_TRUE_MESSAGE(
        ParseText(R"({"wifi": {"networks": [{"ssid": "12345678901234567890123456789012"}]}})", &d,
                  error, sizeof error),
        error);
}

void test_parse_rejects_bad_password() {
    AssertParseFails(R"({"wifi": {"networks": [{"ssid": "A", "password": "short"}]}})",
                     "wifi.networks[0].password");
    // 64 characters must be hex.
    AssertParseFails(
        R"({"wifi": {"networks": [{"ssid": "A", "password": )"
        R"("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"}]}})",
        "wifi.networks[0].password");
}

void test_parse_rejects_too_many_networks() {
    AssertParseFails(R"({"wifi": {"networks": [{"ssid": "1"}, {"ssid": "2"}, {"ssid": "3"},)"
                     R"( {"ssid": "4"}, {"ssid": "5"}]}})",
                     "at most 4");
}

void test_parse_rejects_duplicate_ssid() {
    AssertParseFails(R"({"wifi": {"networks": [{"ssid": "A"}, {"ssid": "A"}]}})", "duplicate");
}

void test_parse_rejects_bad_hostname() {
    AssertParseFails(R"({"wifi": {"hostname": ""}})", "wifi.hostname");
    AssertParseFails(R"({"wifi": {"hostname": "has space"}})", "wifi.hostname");
    AssertParseFails(R"({"wifi": {"hostname": "end-"}})", "wifi.hostname");
}

// --- Time ----------------------------------------------------------------------

void test_parse_time() {
    config::Data d;
    config::FillDefaults(&d);
    char error[config::kErrorSize] = {};
    TEST_ASSERT_TRUE_MESSAGE(
        ParseText(R"({"time": {"zone": "Europe/Kyiv", "server": "10.0.0.1", "syncHours": 0}})", &d,
                  error, sizeof error),
        error);
    TEST_ASSERT_EQUAL_STRING("Europe/Kyiv", d.time.zone);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", d.time.server);
    TEST_ASSERT_EQUAL_UINT8(0, d.time.sync_hours);

    // A raw POSIX rule, and an empty server (no sync).
    TEST_ASSERT_TRUE_MESSAGE(
        ParseText(R"({"time": {"zone": "<+05>-5", "server": "", "syncHours": 168}})", &d, error,
                  sizeof error),
        error);
    TEST_ASSERT_EQUAL_STRING("<+05>-5", d.time.zone);
    TEST_ASSERT_EQUAL_STRING("", d.time.server);
    TEST_ASSERT_EQUAL_UINT8(168, d.time.sync_hours);

    char json[config::kMaxFileSize];
    TEST_ASSERT_GREATER_THAN(0, config::Serialize(d, json, sizeof json));
    config::Data out;
    config::FillDefaults(&out);
    TEST_ASSERT_TRUE_MESSAGE(ParseText(json, &out, error, sizeof error), error);
    TEST_ASSERT_EQUAL_STRING(d.time.zone, out.time.zone);
    TEST_ASSERT_EQUAL_STRING(d.time.server, out.time.server);
    TEST_ASSERT_EQUAL_UINT8(d.time.sync_hours, out.time.sync_hours);
}

void test_parse_rejects_bad_time() {
    AssertParseFails(R"({"time": 1})", "time: expected an object");
    AssertParseFails(R"({"time": {"zone": "Europe/Kyev"}})", "time.zone");
    AssertParseFails(R"({"time": {"zone": ""}})", "time.zone");
    AssertParseFails(R"({"time": {"server": "ntp server"}})", "time.server");
    AssertParseFails(R"({"time": {"server": ".pool.ntp.org"}})", "time.server");
    AssertParseFails(R"({"time": {"server": "pool..ntp.org"}})", "time.server");
    AssertParseFails(R"({"time": {"syncHours": 169}})", "time.syncHours");
    AssertParseFails(R"({"time": {"syncHours": -1}})", "time.syncHours");
}

void test_parse_led() {
    config::Data d;
    config::FillDefaults(&d);
    char error[config::kErrorSize] = {};
    TEST_ASSERT_TRUE_MESSAGE(
        ParseText(R"({"led": {"brightness": 100, "idleBrightness": 0}})", &d, error, sizeof error),
        error);
    TEST_ASSERT_EQUAL_UINT8(100, d.led.percent);
    TEST_ASSERT_EQUAL_UINT8(0, d.led.idle_percent);

    char json[config::kMaxFileSize];
    TEST_ASSERT_GREATER_THAN(0, config::Serialize(d, json, sizeof json));
    config::Data out;
    config::FillDefaults(&out);
    TEST_ASSERT_TRUE_MESSAGE(ParseText(json, &out, error, sizeof error), error);
    TEST_ASSERT_EQUAL_UINT8(100, out.led.percent);
    TEST_ASSERT_EQUAL_UINT8(0, out.led.idle_percent);

    AssertParseFails(R"({"led": 1})", "led: expected an object");
    AssertParseFails(R"({"test": 1})", "test: expected an object");
    AssertParseFails(R"({"pin": {"lockMinutes": 0}})", "pin.lockMinutes");
    AssertParseFails(R"({"pin": {"lockMinutes": 1441}})", "pin.lockMinutes");
    AssertParseFails(R"({"pin": {"complexity": 1}})", "pin.complexity");
    AssertParseFails(R"({"pin": {"wipe": "123456"}})", "pin.wipe");  // never the PIN itself
    const char *wipe_json =
        R"({"pin": {"lockMinutes": 30, "wipe": "00112233445566778899aabbccddeeff:)"
        R"(000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"}})";
    TEST_ASSERT_TRUE_MESSAGE(ParseText(wipe_json, &d, error, sizeof error), error);
    TEST_ASSERT_EQUAL_UINT16(30, d.pin.lock_minutes);
    TEST_ASSERT_EQUAL_CHAR(':', d.pin.wipe[32]);
    AssertParseFails(R"({"test": {"pin": "12345"}})", "test.pin");
    AssertParseFails(R"({"test": {"autoPresence": "yes"}})", "test.autoPresence");
    AssertParseFails(R"({"test": {"pin": "12345a"}})", "test.pin");
    AssertParseFails(R"({"test": {"pin": "1234567"}})", "test.pin");
    TEST_ASSERT_TRUE_MESSAGE(ParseText(R"({"test": {"pin": "012345"}})", &d, error, sizeof error),
                             error);
    TEST_ASSERT_EQUAL_STRING("012345", d.test.pin);
    TEST_ASSERT_GREATER_THAN(0, config::Serialize(d, json, sizeof json));
    TEST_ASSERT_NOT_NULL(strstr(json, "012345"));
    AssertParseFails(R"({"led": {"brightness": 101}})", "led.brightness");
    AssertParseFails(R"({"led": {"idleBrightness": -1}})", "led.idleBrightness");
    AssertParseFails(R"({"led": {"brightness": 1.5}})", "led.brightness");
}

void test_parse_frontlight() {
    config::Data d;
    config::FillDefaults(&d);
    TEST_ASSERT_EQUAL_UINT8(20, d.frontlight.brightness);  // the level; off at boot
    TEST_ASSERT_EQUAL_UINT8(50, d.frontlight.warmth);
    char error[config::kErrorSize] = {};
    TEST_ASSERT_TRUE_MESSAGE(
        ParseText(R"({"frontlight": {"brightness": 40, "warmth": 100}})", &d, error, sizeof error),
        error);
    char json[config::kMaxFileSize];
    TEST_ASSERT_GREATER_THAN(0, config::Serialize(d, json, sizeof json));
    config::Data out;
    config::FillDefaults(&out);
    TEST_ASSERT_TRUE_MESSAGE(ParseText(json, &out, error, sizeof error), error);
    TEST_ASSERT_EQUAL_UINT8(40, out.frontlight.brightness);
    TEST_ASSERT_EQUAL_UINT8(100, out.frontlight.warmth);
    AssertParseFails(R"({"frontlight": 1})", "frontlight: expected an object");
    AssertParseFails(R"({"frontlight": {"brightness": 101}})", "frontlight.brightness");
    AssertParseFails(R"({"frontlight": {"brightness": 0}})", "frontlight.brightness");
    AssertParseFails(R"({"frontlight": {"warmth": -1}})", "frontlight.warmth");
}

void test_parse_display() {
    config::Data d;
    config::FillDefaults(&d);
    TEST_ASSERT_TRUE(d.display.wallpaper == config::WallpaperMode::kGrey);
    char error[config::kErrorSize] = {};
    TEST_ASSERT_TRUE_MESSAGE(ParseText(R"({"display": {"wallpaper": "bw"}})", &d, error, sizeof error),
                             error);
    TEST_ASSERT_TRUE(d.display.wallpaper == config::WallpaperMode::kBw);
    char json[config::kMaxFileSize];
    TEST_ASSERT_GREATER_THAN(0, config::Serialize(d, json, sizeof json));
    config::Data out;
    config::FillDefaults(&out);
    TEST_ASSERT_TRUE_MESSAGE(ParseText(json, &out, error, sizeof error), error);
    TEST_ASSERT_TRUE(out.display.wallpaper == config::WallpaperMode::kBw);
    AssertParseFails(R"({"display": 1})", "display: expected an object");
    AssertParseFails(R"({"display": {"wallpaper": "gray"}})", "display.wallpaper");

    // The touch's calibration: 1000 / 0 by default, kept through a save.
    TEST_ASSERT_EQUAL_INT16(1000, out.display.touch.x_scale);
    TEST_ASSERT_EQUAL_INT16(0, out.display.touch.y_offset);
    TEST_ASSERT_TRUE_MESSAGE(
        ParseText(R"({"display": {"touch": {"xScale": 1020, "xOffset": -7, "yScale": 990, "yOffset": 12}}})",
                  &d, error, sizeof error),
        error);
    TEST_ASSERT_EQUAL_INT16(-7, d.display.touch.x_offset);
    TEST_ASSERT_GREATER_THAN(0, config::Serialize(d, json, sizeof json));
    config::FillDefaults(&out);
    TEST_ASSERT_TRUE_MESSAGE(ParseText(json, &out, error, sizeof error), error);
    TEST_ASSERT_EQUAL_INT16(1020, out.display.touch.x_scale);
    TEST_ASSERT_EQUAL_INT16(-7, out.display.touch.x_offset);
    TEST_ASSERT_EQUAL_INT16(990, out.display.touch.y_scale);
    TEST_ASSERT_EQUAL_INT16(12, out.display.touch.y_offset);
    AssertParseFails(R"({"display": {"touch": 3}})", "display.touch: expected an object");
    AssertParseFails(R"({"display": {"touch": {"xScale": 400}}})", "display.touch.xScale");
    AssertParseFails(R"({"display": {"touch": {"yOffset": -201}}})", "display.touch.yOffset");
    AssertParseFails(R"({"display": {"touch": {"xOffset": 1.5}}})", "display.touch.xOffset");
}

void test_parse_power() {
    config::Data d;
    config::FillDefaults(&d);
    char error[config::kErrorSize] = {};
    TEST_ASSERT_TRUE_MESSAGE(
        ParseText(R"({"power": {"idleMinutes": 0, "keepPinHours": 168, "holdRail": false}})", &d, error, sizeof error),
        error);
    TEST_ASSERT_EQUAL_UINT16(0, d.power.idle_minutes);
    TEST_ASSERT_EQUAL_UINT8(168, d.power.keep_pin_hours);
    TEST_ASSERT_FALSE(d.power.hold_rail);
    char json[config::kMaxFileSize];
    TEST_ASSERT_GREATER_THAN(0, config::Serialize(d, json, sizeof json));
    config::Data out;
    config::FillDefaults(&out);
    TEST_ASSERT_TRUE_MESSAGE(ParseText(json, &out, error, sizeof error), error);
    TEST_ASSERT_EQUAL_UINT16(0, out.power.idle_minutes);
    TEST_ASSERT_EQUAL_UINT8(168, out.power.keep_pin_hours);
    TEST_ASSERT_FALSE(out.power.hold_rail);
    AssertParseFails(R"({"power": 1})", "power: expected an object");
    AssertParseFails(R"({"power": {"idleMinutes": 1441}})", "power.idleMinutes");
    AssertParseFails(R"({"power": {"keepPinHours": 169}})", "power.keepPinHours");
    AssertParseFails(R"({"power": {"holdRail": 1}})", "power.holdRail");
}

void test_valid_host() {
    TEST_ASSERT_TRUE(config::ValidHost(""));
    TEST_ASSERT_TRUE(config::ValidHost("pool.ntp.org"));
    TEST_ASSERT_TRUE(config::ValidHost("time-a.nist.gov"));
    TEST_ASSERT_TRUE(config::ValidHost("192.168.1.1"));
    TEST_ASSERT_FALSE(config::ValidHost("ntp.org."));
    TEST_ASSERT_FALSE(config::ValidHost("-ntp.org"));
    TEST_ASSERT_FALSE(config::ValidHost("ntp-.org"));
    TEST_ASSERT_FALSE(config::ValidHost("ntp_1.org"));
    TEST_ASSERT_FALSE(config::ValidHost("http://ntp.org"));
}

void test_valid_ble_name() {
    TEST_ASSERT_TRUE(config::ValidBleName(""));  // the board's name
    TEST_ASSERT_TRUE(config::ValidBleName("ESP32 Auth X4Pro"));
    TEST_ASSERT_TRUE(config::ValidBleName("a name of 29 bytes, just fits"));
    TEST_ASSERT_FALSE(config::ValidBleName("a name of thirty bytes, too lo"));
    // UTF-8: 14 Cyrillic letters (28 bytes) + 1 fits, 15 (30 bytes) does not.
    TEST_ASSERT_TRUE(config::ValidBleName(
        "\xd0\x9a\xd0\xbb\xd1\x8e\xd1\x87\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87"
        "\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xba\xd0\xbb!"));
    TEST_ASSERT_FALSE(config::ValidBleName(
        "\xd0\x9a\xd0\xbb\xd1\x8e\xd1\x87\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87"
        "\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xba\xd0\xbb\xd1\x8e"));
    TEST_ASSERT_FALSE(config::ValidBleName("tab\there"));
    TEST_ASSERT_FALSE(config::ValidBleName("del\x7f"));
    TEST_ASSERT_FALSE(config::ValidBleName("cut \xd0"));      // a lead byte, then the end
    TEST_ASSERT_FALSE(config::ValidBleName("bad \x80 byte"));  // a lone continuation
    TEST_ASSERT_FALSE(config::ValidBleName("\xd0" "A"));       // lead + ASCII
}

// --- Serialize -------------------------------------------------------------------

void test_serialize_round_trip() {
    config::Data in;
    char error[config::kErrorSize] = {};
    TEST_ASSERT_TRUE(ParseText(kFullJson, &in, error, sizeof error));

    char json[config::kMaxFileSize];
    const size_t length = config::Serialize(in, json, sizeof json);
    TEST_ASSERT_GREATER_THAN(0, length);
    TEST_ASSERT_EQUAL(std::strlen(json), length);

    config::Data out;
    TEST_ASSERT_TRUE_MESSAGE(ParseText(json, &out, error, sizeof error), error);
    TEST_ASSERT_EQUAL(in.wifi.active, out.wifi.active);
    TEST_ASSERT_EQUAL_STRING(in.wifi.hostname, out.wifi.hostname);
    TEST_ASSERT_EQUAL_UINT8(in.wifi.network_count, out.wifi.network_count);
    for (uint8_t i = 0; i < in.wifi.network_count; ++i) {
        TEST_ASSERT_EQUAL_STRING(in.wifi.networks[i].ssid, out.wifi.networks[i].ssid);
        TEST_ASSERT_EQUAL_STRING(in.wifi.networks[i].password, out.wifi.networks[i].password);
    }
    TEST_ASSERT_EQUAL(in.ble.active, out.ble.active);
    TEST_ASSERT_EQUAL_STRING(in.ble.name, out.ble.name);
    TEST_ASSERT_EQUAL(static_cast<int>(in.wifi.mode), static_cast<int>(out.wifi.mode));
    TEST_ASSERT_EQUAL_UINT8(in.wifi.rounds, out.wifi.rounds);
    TEST_ASSERT_EQUAL_UINT16(in.wifi.ap_window_seconds, out.wifi.ap_window_seconds);
    TEST_ASSERT_EQUAL_STRING(in.wifi.ap_ssid, out.wifi.ap_ssid);
    TEST_ASSERT_EQUAL_STRING(in.wifi.ap_password, out.wifi.ap_password);
    TEST_ASSERT_EQUAL_UINT8(in.wifi.ap_channel, out.wifi.ap_channel);
    TEST_ASSERT_EQUAL(static_cast<int>(in.web.mode), static_cast<int>(out.web.mode));
    TEST_ASSERT_EQUAL(in.web.write, out.web.write);
    TEST_ASSERT_EQUAL_STRING(in.web.user, out.web.user);
    TEST_ASSERT_EQUAL_STRING(in.web.password, out.web.password);
    TEST_ASSERT_EQUAL_STRING(in.time.zone, out.time.zone);
    TEST_ASSERT_EQUAL_STRING(in.time.server, out.time.server);
    TEST_ASSERT_EQUAL_UINT8(in.time.sync_hours, out.time.sync_hours);
}

void test_serialize_static_ip_round_trip() {
    config::Data in;
    config::FillDefaults(&in);
    char error[config::kErrorSize] = {};
    TEST_ASSERT_TRUE(config::SetNetwork(&in.wifi, "Office", "password123", error, sizeof error));
    TEST_ASSERT_TRUE(config::SetNetwork(&in.wifi, "Home", "", error, sizeof error));
    config::StaticIp &ip = in.wifi.networks[0].ip;
    ip.enabled = true;
    std::snprintf(ip.address, sizeof ip.address, "10.0.0.5");
    std::snprintf(ip.netmask, sizeof ip.netmask, "255.255.255.0");
    std::snprintf(ip.gateway, sizeof ip.gateway, "10.0.0.1");
    std::snprintf(ip.dns2, sizeof ip.dns2, "8.8.8.8");
    char json[config::kMaxFileSize];
    TEST_ASSERT_GREATER_THAN(0, config::Serialize(in, json, sizeof json));
    config::Data out;
    TEST_ASSERT_TRUE_MESSAGE(ParseText(json, &out, error, sizeof error), error);
    TEST_ASSERT_EQUAL_MEMORY(&in.wifi.networks[0].ip, &out.wifi.networks[0].ip,
                             sizeof(config::StaticIp));
    TEST_ASSERT_FALSE(out.wifi.networks[1].ip.enabled);
    // No empty ip block for a DHCP network.
    TEST_ASSERT_NULL(std::strstr(std::strstr(json, "\"Home\""), "\"ip\""));
}

void test_set_network_keeps_the_static_ip() {
    config::Wifi w = {};
    char error[config::kErrorSize] = {};
    TEST_ASSERT_TRUE(config::SetNetwork(&w, "Office", "password123", error, sizeof error));
    w.networks[0].ip.enabled = true;
    std::snprintf(w.networks[0].ip.address, config::kIpTextSize, "10.0.0.5");
    TEST_ASSERT_TRUE(config::SetNetwork(&w, "Office", "newpassword", error, sizeof error));
    TEST_ASSERT_TRUE(w.networks[0].ip.enabled);
    TEST_ASSERT_EQUAL_STRING("10.0.0.5", w.networks[0].ip.address);
}

void test_serialize_escapes_strings() {
    config::Data in;
    config::FillDefaults(&in);
    char error[config::kErrorSize] = {};
    TEST_ASSERT_TRUE(config::SetNetwork(&in.wifi, "My \"Net\\", "pa\"ss\\word", error, sizeof error));
    char json[config::kMaxFileSize];
    TEST_ASSERT_GREATER_THAN(0, config::Serialize(in, json, sizeof json));
    config::Data out;
    TEST_ASSERT_TRUE_MESSAGE(ParseText(json, &out, error, sizeof error), error);
    TEST_ASSERT_EQUAL_STRING("My \"Net\\", out.wifi.networks[0].ssid);
    TEST_ASSERT_EQUAL_STRING("pa\"ss\\word", out.wifi.networks[0].password);
}

void test_serialize_too_small_buffer() {
    config::Data d;
    config::FillDefaults(&d);
    char json[16];
    TEST_ASSERT_EQUAL(0, config::Serialize(d, json, sizeof json));
}

// --- Network list ----------------------------------------------------------------

void test_set_network_adds_and_updates() {
    config::Wifi w = {};
    char error[config::kErrorSize] = {};
    TEST_ASSERT_TRUE(config::SetNetwork(&w, "A", "password1", error, sizeof error));
    TEST_ASSERT_TRUE(config::SetNetwork(&w, "B", "", error, sizeof error));
    TEST_ASSERT_TRUE(config::SetNetwork(&w, "A", "password2", error, sizeof error));
    TEST_ASSERT_EQUAL_UINT8(2, w.network_count);
    TEST_ASSERT_EQUAL_STRING("password2", w.networks[0].password);
}

void test_set_network_validates() {
    config::Wifi w = {};
    char error[config::kErrorSize] = {};
    TEST_ASSERT_FALSE(config::SetNetwork(&w, "", "password1", error, sizeof error));
    TEST_ASSERT_FALSE(config::SetNetwork(&w, "A", "short", error, sizeof error));
    TEST_ASSERT_NOT_NULL(std::strstr(error, "password"));
    TEST_ASSERT_EQUAL_UINT8(0, w.network_count);
}

void test_set_network_full_list() {
    config::Wifi w = {};
    char error[config::kErrorSize] = {};
    const char *names[] = {"1", "2", "3", "4"};
    for (const char *n : names) {
        TEST_ASSERT_TRUE(config::SetNetwork(&w, n, "", error, sizeof error));
    }
    TEST_ASSERT_FALSE(config::SetNetwork(&w, "5", "", error, sizeof error));
    TEST_ASSERT_NOT_NULL(std::strstr(error, "full"));
    TEST_ASSERT_TRUE(config::SetNetwork(&w, "2", "password9", error, sizeof error));  // update ok
}

void test_remove_network_keeps_order() {
    config::Wifi w = {};
    char error[config::kErrorSize] = {};
    config::SetNetwork(&w, "A", "", error, sizeof error);
    config::SetNetwork(&w, "B", "", error, sizeof error);
    config::SetNetwork(&w, "C", "", error, sizeof error);
    TEST_ASSERT_TRUE(config::RemoveNetwork(&w, "B"));
    TEST_ASSERT_FALSE(config::RemoveNetwork(&w, "B"));
    TEST_ASSERT_EQUAL_UINT8(2, w.network_count);
    TEST_ASSERT_EQUAL_STRING("A", w.networks[0].ssid);
    TEST_ASSERT_EQUAL_STRING("C", w.networks[1].ssid);
    TEST_ASSERT_EQUAL_STRING("", w.networks[2].ssid);
}

// --- File ------------------------------------------------------------------------

void test_init_missing_file_uses_defaults() {
    MountEmptyDir();
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, config::Init(kDir));
    TEST_ASSERT_EQUAL(config::Source::kDefault, config::LoadedFrom());
    AssertDefaults(config::Get());
    TEST_ASSERT_NOT_NULL(std::strstr(config::LastError(), "not found"));
    TEST_ASSERT_FALSE(Exists(config::kFileName));  // nothing written at boot
}

void test_init_reads_file() {
    MountEmptyDir();
    PutFile(config::kFileName, kFullJson);
    TEST_ASSERT_EQUAL(ESP_OK, config::Init(kDir));
    TEST_ASSERT_EQUAL(config::Source::kFile, config::LoadedFrom());
    TEST_ASSERT_EQUAL_UINT8(2, config::Get().wifi.network_count);
    TEST_ASSERT_EQUAL_STRING("", config::LastError());
    TEST_ASSERT_EQUAL_STRING("build/fs/config.json", config::Path());
}

void test_init_invalid_file_uses_defaults_and_keeps_file() {
    MountEmptyDir();
    const char *bad = R"({"wifi": {"active": "maybe"}})";
    PutFile(config::kFileName, bad);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config::Init(kDir));
    TEST_ASSERT_EQUAL(config::Source::kDefault, config::LoadedFrom());
    AssertDefaults(config::Get());
    TEST_ASSERT_NOT_NULL(std::strstr(config::LastError(), "wifi.active"));
    TEST_ASSERT_EQUAL_STRING(bad, ReadFile(config::kFileName).c_str());
}

void test_init_oversized_file_uses_defaults() {
    MountEmptyDir();
    std::string big = "{\"pad\": \"" + std::string(config::kMaxFileSize, 'x') + "\"}";
    PutFile(config::kFileName, big.c_str());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, config::Init(kDir));
    AssertDefaults(config::Get());
}

void test_save_then_init_reads_it_back() {
    MountEmptyDir();
    config::Init(kDir);
    char error[config::kErrorSize] = {};
    TEST_ASSERT_TRUE(config::SetNetwork(&config::Get().wifi, "Home", "password123", error,
                                        sizeof error));
    config::Get().ble.active = true;
    TEST_ASSERT_EQUAL(ESP_OK, config::Save());
    TEST_ASSERT_EQUAL(config::Source::kFile, config::LoadedFrom());
    TEST_ASSERT_TRUE(Exists(config::kFileName));
    TEST_ASSERT_FALSE(Exists(config::kTempName));

    config::Get().ble.active = false;  // must come back from the file
    TEST_ASSERT_EQUAL(ESP_OK, config::Init(kDir));
    TEST_ASSERT_TRUE(config::Get().ble.active);
    TEST_ASSERT_EQUAL_STRING("Home", config::Get().wifi.networks[0].ssid);
}

void test_save_replaces_existing_file() {
    MountEmptyDir();
    PutFile(config::kFileName, kFullJson);
    config::Init(kDir);
    config::Get().wifi.active = false;
    TEST_ASSERT_EQUAL(ESP_OK, config::Save());
    TEST_ASSERT_EQUAL(ESP_OK, config::Init(kDir));
    TEST_ASSERT_FALSE(config::Get().wifi.active);
    TEST_ASSERT_EQUAL_UINT8(2, config::Get().wifi.network_count);
}

void test_init_finishes_interrupted_write() {
    // Power cut after remove(config.json), before rename: only the temp is left.
    MountEmptyDir();
    PutFile(config::kTempName, kFullJson);
    TEST_ASSERT_EQUAL(ESP_OK, config::Init(kDir));
    TEST_ASSERT_TRUE(config::Get().wifi.active);
    TEST_ASSERT_TRUE(Exists(config::kFileName));
    TEST_ASSERT_FALSE(Exists(config::kTempName));
}

void test_init_drops_stale_temp() {
    // Cut before remove: config.json is intact, the temp is half a write.
    MountEmptyDir();
    PutFile(config::kFileName, kFullJson);
    PutFile(config::kTempName, "{\"wifi\": {\"act");
    TEST_ASSERT_EQUAL(ESP_OK, config::Init(kDir));
    TEST_ASSERT_TRUE(config::Get().wifi.active);
    TEST_ASSERT_FALSE(Exists(config::kTempName));
}

void test_reload_failure_keeps_values() {
    MountEmptyDir();
    PutFile(config::kFileName, kFullJson);
    config::Init(kDir);
    PutFile(config::kFileName, "not json");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config::Reload());
    TEST_ASSERT_TRUE(config::Get().wifi.active);
    TEST_ASSERT_EQUAL_UINT8(2, config::Get().wifi.network_count);
    TEST_ASSERT_NOT_NULL(std::strstr(config::LastError(), "not valid JSON"));
}

}  // namespace

void RegisterConfigTests(void) {
    RUN_TEST(test_defaults_parse);
    RUN_TEST(test_fill_defaults_values);
    RUN_TEST(test_parse_full);
    RUN_TEST(test_parse_empty_object_keeps_defaults);
    RUN_TEST(test_parse_partial_keeps_other_defaults);
    RUN_TEST(test_parse_ignores_unknown_fields);
    RUN_TEST(test_parse_password_optional);
    RUN_TEST(test_parse_hex_psk);
    RUN_TEST(test_parse_failure_leaves_output_unchanged);
    RUN_TEST(test_parse_rejects_invalid_json);
    RUN_TEST(test_parse_rejects_non_object);
    RUN_TEST(test_parse_rejects_wrong_types);
    RUN_TEST(test_parse_rejects_bad_ssid);
    RUN_TEST(test_parse_accepts_32_byte_ssid);
    RUN_TEST(test_parse_rejects_bad_password);
    RUN_TEST(test_parse_rejects_too_many_networks);
    RUN_TEST(test_parse_rejects_duplicate_ssid);
    RUN_TEST(test_parse_rejects_bad_hostname);
    RUN_TEST(test_parse_rejects_bad_wifi_and_web_fields);
    RUN_TEST(test_parse_internet_check);
    RUN_TEST(test_parse_rejects_bad_internet_check);
    RUN_TEST(test_parse_time);
    RUN_TEST(test_parse_rejects_bad_time);
    RUN_TEST(test_valid_host);
    RUN_TEST(test_parse_led);
    RUN_TEST(test_parse_frontlight);
    RUN_TEST(test_parse_display);
    RUN_TEST(test_parse_power);
    RUN_TEST(test_parse_ipv4);
    RUN_TEST(test_parse_static_ip);
    RUN_TEST(test_parse_rejects_bad_static_ip);
    RUN_TEST(test_serialize_static_ip_round_trip);
    RUN_TEST(test_set_network_keeps_the_static_ip);
    RUN_TEST(test_serialize_round_trip);
    RUN_TEST(test_serialize_escapes_strings);
    RUN_TEST(test_serialize_too_small_buffer);
    RUN_TEST(test_set_network_adds_and_updates);
    RUN_TEST(test_set_network_validates);
    RUN_TEST(test_set_network_full_list);
    RUN_TEST(test_remove_network_keeps_order);
    RUN_TEST(test_init_missing_file_uses_defaults);
    RUN_TEST(test_init_reads_file);
    RUN_TEST(test_init_invalid_file_uses_defaults_and_keeps_file);
    RUN_TEST(test_init_oversized_file_uses_defaults);
    RUN_TEST(test_save_then_init_reads_it_back);
    RUN_TEST(test_save_replaces_existing_file);
    RUN_TEST(test_init_finishes_interrupted_write);
    RUN_TEST(test_init_drops_stale_temp);
    RUN_TEST(test_reload_failure_keeps_values);
    RUN_TEST(test_valid_ble_name);
}
