// web_paths and web_policy: which URL is a page, the reboot word, when the
// server runs. Ported from approver-esp32.

#include "unity.h"
#include "web_paths.h"
#include "web_policy.h"

#include <cstdio>
#include <cstring>

namespace {

bool Name(const char *uri, char *out, size_t capacity = 64) {
    (void)capacity;
    return web::UriToName(uri, out, 64);
}

}  // namespace

void test_the_root_is_the_index(void) {
    char name[64] = {};
    TEST_ASSERT_TRUE(Name("/", name));
    TEST_ASSERT_EQUAL_STRING(web::kIndexName, name);
}

void test_a_page_and_the_things_a_page_is_made_of(void) {
    const char *served[] = {"/index.html", "/style.css", "/app.js",
                            "/logo.png",   "/favicon.ico", "/icon.svg", "/notes.txt"};
    for (const char *uri : served) {
        char name[64] = {};
        TEST_ASSERT_TRUE_MESSAGE(Name(uri, name), uri);
        TEST_ASSERT_EQUAL_STRING(uri + 1, name);
    }
}

void test_a_query_string_is_the_same_page(void) {
    char name[64] = {};
    TEST_ASSERT_TRUE(Name("/index.html?saved=1&x=2", name));
    TEST_ASSERT_EQUAL_STRING("index.html", name);

    TEST_ASSERT_TRUE(Name("/style.css#top", name));
    TEST_ASSERT_EQUAL_STRING("style.css", name);

    TEST_ASSERT_TRUE(Name("/?saved=1", name));
    TEST_ASSERT_EQUAL_STRING(web::kIndexName, name);
}

void test_the_config_file_is_not_a_page(void) {
    const char *never[] = {"/config.json", "/config.init.json", "/registration.json",
                           "/keys.json", "/users.json"};
    for (const char *uri : never) {
        char name[64] = {};
        TEST_ASSERT_FALSE_MESSAGE(Name(uri, name), uri);
    }
}

void test_an_extension_nobody_listed_is_refused(void) {
    const char *never[] = {"/firmware.bin", "/splash.bin", "/alert.wav", "/config",
                           "/index", "/index.html.bak", "/.hidden"};
    for (const char *uri : never) {
        char name[64] = {};
        TEST_ASSERT_FALSE_MESSAGE(Name(uri, name), uri);
    }
}

void test_traversal_is_not_defended_against_it_is_unrepresentable(void) {
    const char *never[] = {"/../config.json", "/..%2fconfig.json", "/a/b.html",
                           "/%2e%2e/config.json", "//config.json", "..",
                           "/subdir/", "/%2econfig.json"};
    for (const char *uri : never) {
        char name[64] = {};
        TEST_ASSERT_FALSE_MESSAGE(Name(uri, name), uri);
    }
}

void test_a_url_that_is_not_a_path_is_refused(void) {
    char name[64] = {};
    TEST_ASSERT_FALSE(Name("", name));
    TEST_ASSERT_FALSE(Name("index.html", name));  // no leading slash
    TEST_ASSERT_FALSE(Name("http://host/index.html", name));
    TEST_ASSERT_FALSE(web::UriToName(nullptr, name, sizeof name));
}

void test_a_name_longer_than_the_filesystem_holds_is_refused(void) {
    char uri[80] = {};
    char name[64] = {};

    std::snprintf(uri, sizeof uri, "/%.*s.html", static_cast<int>(web::kMaxNameLength - 5),
                  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(web::kMaxNameLength),
                             static_cast<uint32_t>(std::strlen(uri) - 1));
    TEST_ASSERT_TRUE(Name(uri, name));  // exactly at the bound

    std::snprintf(uri, sizeof uri, "/%.*s.html", static_cast<int>(web::kMaxNameLength - 4),
                  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    TEST_ASSERT_FALSE(Name(uri, name));  // and one over it
}

void test_a_buffer_too_small_is_refused_rather_than_filled(void) {
    char name[8] = {};
    std::snprintf(name, sizeof name, "%s", "keepme");
    TEST_ASSERT_FALSE(web::UriToName("/index.html", name, sizeof name));
    TEST_ASSERT_EQUAL_STRING("keepme", name);
}

void test_every_served_extension_has_a_type(void) {
    TEST_ASSERT_EQUAL_STRING("text/html", web::ContentType("index.html"));
    TEST_ASSERT_EQUAL_STRING("text/css", web::ContentType("style.css"));
    TEST_ASSERT_EQUAL_STRING("application/javascript", web::ContentType("app.js"));
    TEST_ASSERT_EQUAL_STRING("image/png", web::ContentType("logo.png"));
    TEST_ASSERT_EQUAL_STRING("image/x-icon", web::ContentType("favicon.ico"));
    TEST_ASSERT_EQUAL_STRING("image/svg+xml", web::ContentType("icon.svg"));
    TEST_ASSERT_EQUAL_STRING("text/plain", web::ContentType("notes.txt"));
}

void test_a_type_is_never_null(void) {
    TEST_ASSERT_NOT_NULL(web::ContentType("mystery.zip"));
    TEST_ASSERT_NOT_NULL(web::ContentType(""));
    TEST_ASSERT_NOT_NULL(web::ContentType(nullptr));
}

void test_the_pages_this_site_is_made_of_are_all_reachable(void) {
    const char *site[] = {"/index.html", "/devstatus.html", "/reboot.html",
                          "/404.html",   "/401.html",       "/wifi.html",
                          "/app.js"};
    for (const char *uri : site) {
        char name[64] = {};
        TEST_ASSERT_TRUE_MESSAGE(Name(uri, name), uri);
        TEST_ASSERT_EQUAL_STRING(uri + 1, name);
    }
}

void test_the_not_found_page_is_a_page_like_any_other(void) {
    char name[64] = {};
    char uri[64] = {};
    std::snprintf(uri, sizeof uri, "/%s", web::kNotFoundName);
    TEST_ASSERT_TRUE(Name(uri, name));
    TEST_ASSERT_EQUAL_STRING(web::kNotFoundName, name);
    TEST_ASSERT_EQUAL_STRING("text/html", web::ContentType(web::kNotFoundName));
}

void test_the_unauthorised_page_is_a_page_like_any_other(void) {
    char name[64] = {};
    char uri[64] = {};
    std::snprintf(uri, sizeof uri, "/%s", web::kUnauthorisedName);
    TEST_ASSERT_TRUE(Name(uri, name));
    TEST_ASSERT_EQUAL_STRING(web::kUnauthorisedName, name);
    TEST_ASSERT_EQUAL_STRING("text/html", web::ContentType(web::kUnauthorisedName));
}

void test_a_reboot_needs_the_word(void) {
    TEST_ASSERT_TRUE(web::ConfirmsReboot("/api/reboot?confirm=reboot"));
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/api/reboot"));
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/api/reboot?"));
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/api/reboot?confirm="));
    TEST_ASSERT_FALSE(web::ConfirmsReboot(nullptr));
}

void test_the_word_is_found_wherever_in_the_query_it_sits(void) {
    TEST_ASSERT_TRUE(web::ConfirmsReboot("/api/reboot?t=1&confirm=reboot"));
    TEST_ASSERT_TRUE(web::ConfirmsReboot("/api/reboot?confirm=reboot&t=1"));
    TEST_ASSERT_TRUE(web::ConfirmsReboot("/api/reboot?a=1&confirm=reboot&b=2"));
    TEST_ASSERT_TRUE(web::ConfirmsReboot("/api/reboot?confirm=reboot#done"));
}

void test_something_that_merely_contains_the_word_is_not_the_word(void) {
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/api/reboot?xconfirm=reboot"));
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/api/reboot?confirm=reboots"));
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/api/reboot?confirm=rebo"));
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/api/reboot?confirmed=reboot"));
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/api/reboot?Confirm=reboot"));
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/api/reboot?confirm=Reboot"));
}

void test_the_confirmation_is_not_a_path(void) {
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/confirm=reboot"));
    TEST_ASSERT_FALSE(web::ConfirmsReboot("/api/reboot/confirm=reboot"));
}

void test_no_network_stack_means_no_server_whatever_was_asked(void) {
    const web::Desired every[] = {web::Desired::kOff, web::Desired::kOn, web::Desired::kAuto};
    for (web::Desired desired : every) {
        TEST_ASSERT_FALSE(web::ShouldRun(desired, true, false, false));
        TEST_ASSERT_FALSE(web::ShouldRun(desired, true, false, true));
    }
}

void test_off_is_off_even_with_a_network(void) {
    TEST_ASSERT_FALSE(web::ShouldRun(web::Desired::kOff, true, true, false));
    TEST_ASSERT_FALSE(web::ShouldRun(web::Desired::kOff, true, true, true));
}

void test_on_is_up_on_any_network(void) {
    TEST_ASSERT_TRUE(web::ShouldRun(web::Desired::kOn, true, true, false));
    TEST_ASSERT_TRUE(web::ShouldRun(web::Desired::kOn, true, true, true));
}

void test_auto_is_up_only_while_this_device_is_an_access_point(void) {
    TEST_ASSERT_TRUE(web::ShouldRun(web::Desired::kAuto, true, true, true));
    TEST_ASSERT_FALSE(web::ShouldRun(web::Desired::kAuto, true, true, false));
}

void test_a_radio_on_its_way_out_takes_the_server_with_it(void) {
    TEST_ASSERT_FALSE(web::ShouldRun(web::Desired::kOn, false, true, false));
    TEST_ASSERT_FALSE(web::ShouldRun(web::Desired::kOn, false, true, true));
    TEST_ASSERT_FALSE(web::ShouldRun(web::Desired::kAuto, false, true, true));
}

void test_every_wish_has_a_word(void) {
    const web::Desired every[] = {web::Desired::kOff, web::Desired::kOn, web::Desired::kAuto};
    for (web::Desired desired : every) {
        TEST_ASSERT_NOT_NULL(web::Name(desired));
        TEST_ASSERT_TRUE(web::Name(desired)[0] != 0);
    }
}

void test_the_reconciler_is_quiet_when_the_world_already_agrees(void) {
    TEST_ASSERT_EQUAL(web::Reconcile::kNothing,
                      web::Next(web::Desired::kOn, true, true, true, true));
    TEST_ASSERT_EQUAL(web::Reconcile::kNothing,
                      web::Next(web::Desired::kOff, true, true, true, false));
}

void test_it_starts_what_is_wanted_and_is_not_up(void) {
    TEST_ASSERT_EQUAL(web::Reconcile::kStart,
                      web::Next(web::Desired::kOn, true, true, false, false));
    TEST_ASSERT_EQUAL(web::Reconcile::kStart,
                      web::Next(web::Desired::kAuto, true, true, true, false));
}

void test_it_stops_what_is_up_and_no_longer_wanted(void) {
    TEST_ASSERT_EQUAL(web::Reconcile::kStop,
                      web::Next(web::Desired::kOff, true, true, true, true));
    TEST_ASSERT_EQUAL(web::Reconcile::kStop,
                      web::Next(web::Desired::kOn, false, true, true, true));
    TEST_ASSERT_EQUAL(web::Reconcile::kStop,
                      web::Next(web::Desired::kAuto, true, true, false, true));
}

void test_the_stop_still_happens_while_the_stack_is_up(void) {
    TEST_ASSERT_EQUAL(web::Reconcile::kStop,
                      web::Next(web::Desired::kOn, false, true, false, true));
}

void RegisterWebPathTests(void) {
    RUN_TEST(test_the_root_is_the_index);
    RUN_TEST(test_a_page_and_the_things_a_page_is_made_of);
    RUN_TEST(test_a_query_string_is_the_same_page);

    RUN_TEST(test_the_config_file_is_not_a_page);
    RUN_TEST(test_an_extension_nobody_listed_is_refused);
    RUN_TEST(test_traversal_is_not_defended_against_it_is_unrepresentable);
    RUN_TEST(test_a_url_that_is_not_a_path_is_refused);
    RUN_TEST(test_a_name_longer_than_the_filesystem_holds_is_refused);
    RUN_TEST(test_a_buffer_too_small_is_refused_rather_than_filled);

    RUN_TEST(test_every_served_extension_has_a_type);
    RUN_TEST(test_a_type_is_never_null);

    RUN_TEST(test_the_pages_this_site_is_made_of_are_all_reachable);
    RUN_TEST(test_the_not_found_page_is_a_page_like_any_other);
    RUN_TEST(test_the_unauthorised_page_is_a_page_like_any_other);

    RUN_TEST(test_a_reboot_needs_the_word);
    RUN_TEST(test_the_word_is_found_wherever_in_the_query_it_sits);
    RUN_TEST(test_something_that_merely_contains_the_word_is_not_the_word);
    RUN_TEST(test_the_confirmation_is_not_a_path);

    RUN_TEST(test_no_network_stack_means_no_server_whatever_was_asked);
    RUN_TEST(test_off_is_off_even_with_a_network);
    RUN_TEST(test_on_is_up_on_any_network);
    RUN_TEST(test_auto_is_up_only_while_this_device_is_an_access_point);
    RUN_TEST(test_a_radio_on_its_way_out_takes_the_server_with_it);
    RUN_TEST(test_every_wish_has_a_word);

    RUN_TEST(test_the_reconciler_is_quiet_when_the_world_already_agrees);
    RUN_TEST(test_it_starts_what_is_wanted_and_is_not_up);
    RUN_TEST(test_it_stops_what_is_up_and_no_longer_wanted);
    RUN_TEST(test_the_stop_still_happens_while_the_stack_is_up);
}
