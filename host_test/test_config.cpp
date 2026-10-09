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
    TEST_ASSERT_FALSE(d.ble.active);
}

constexpr const char *kFullJson = R"json({
  "wifi": {
    "active": true,
    "hostname": "auth-1",
    "networks": [
      {"ssid": "Home", "password": "password123"},
      {"ssid": "Office WiFi", "password": ""}
    ]
  },
  "ble": {"active": true}
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
    TEST_ASSERT_TRUE(d.ble.active);
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
}
