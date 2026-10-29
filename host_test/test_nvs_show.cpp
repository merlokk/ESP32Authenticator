// `nvs ls` values (components/cli/nvs_show.h): what is masked, how a string
// is shown.

#include <cstring>

#include "nvs_show.h"
#include "unity.h"

namespace {

using console::nvs_show::Printable;
using console::nvs_show::SecretKey;

void test_nvs_secret_keys() {
    // The stock firmware's Wi-Fi and ESP-IDF's.
    TEST_ASSERT_TRUE(SecretKey("sta_pwd"));
    TEST_ASSERT_TRUE(SecretKey("wifi_creds"));
    TEST_ASSERT_TRUE(SecretKey("sta.pswd"));
    TEST_ASSERT_TRUE(SecretKey("Password"));
    TEST_ASSERT_TRUE(SecretKey("local_irk_1"));
    TEST_ASSERT_TRUE(SecretKey("api_token"));
    // Shown.
    TEST_ASSERT_FALSE(SecretKey("sta_ssid"));
    TEST_ASSERT_FALSE(SecretKey("cloud_dev_id"));
    TEST_ASSERT_FALSE(SecretKey("screenType"));
    TEST_ASSERT_FALSE(SecretKey("sysFontSm"));
    TEST_ASSERT_FALSE(SecretKey("store_ver"));
}

void test_nvs_printable() {
    char out[console::nvs_show::kMaxShown + 4];
    Printable("home-wifi", out);
    TEST_ASSERT_EQUAL_STRING("home-wifi", out);
    Printable("a\tb\x01", out);
    TEST_ASSERT_EQUAL_STRING("a.b.", out);
    char longer[100];
    std::memset(longer, 'x', sizeof longer - 1);
    longer[sizeof longer - 1] = '\0';
    Printable(longer, out);
    TEST_ASSERT_EQUAL_size_t(console::nvs_show::kMaxShown + 3, std::strlen(out));
    TEST_ASSERT_EQUAL_STRING("...", out + console::nvs_show::kMaxShown);
}

}  // namespace

void RegisterNvsShowTests(void) {
    RUN_TEST(test_nvs_secret_keys);
    RUN_TEST(test_nvs_printable);
}
