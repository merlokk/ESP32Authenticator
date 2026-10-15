// PIN complexity policy (components/pin_policy/pin_policy.h).

#include <cstring>
#include <initializer_list>

#include "pin_policy.h"
#include "unity.h"

namespace {

pin_policy_result_t Check(const char *pin) {
    return pin_policy_check(reinterpret_cast<const uint8_t *>(pin), std::strlen(pin));
}

void AssertResult(pin_policy_result_t expected, const char *pin) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, Check(pin), pin);
}

void test_pin_policy_accepts_ordinary_pins(void) {
    for (const char *pin : {"482913", "905731", "2468", "1357", "13579", "k7#Lp2", "Correct horse",
                            "120345", "121314", "abcx", "11223344x", "1q2w3e4r5t", "\xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c" "7"}) {
        AssertResult(PIN_POLICY_OK, pin);
    }
}

void test_pin_policy_refuses_repeats(void) {
    for (const char *pin : {"0000", "111111", "aaaa", "AaAa", "1212", "121212", "123123", "abcabc",
                            "ab12ab12", "\xd0\xbf\xd0\xbf"}) {
        AssertResult(PIN_POLICY_REPEATED, pin);
    }
}

void test_pin_policy_refuses_runs(void) {
    for (const char *pin : {"1234", "123456", "654321", "9876", "7890", "890123", "0987", "abcd",
                            "ZYXW", "abcdef", "112233", "998877", "aabbcc"}) {
        AssertResult(PIN_POLICY_RUN, pin);
    }
}

void test_pin_policy_a_run_needs_three(void) {
    // two in a row are no run (and 12 repeats nothing)
    AssertResult(PIN_POLICY_OK, "12");
    // mixed digits and letters are no run
    AssertResult(PIN_POLICY_OK, "89ab");
    // a step that changes direction is no run
    AssertResult(PIN_POLICY_OK, "1232");
}

void test_pin_policy_refuses_common_pins(void) {
    for (const char *pin : {"2580", "1004", "147258", "159753", "123321", "password", "PassWord",
                            "qwerty", "1qaz2wsx"}) {
        AssertResult(PIN_POLICY_COMMON, pin);
    }
}

void test_pin_policy_common_is_a_whole_match(void) {
    AssertResult(PIN_POLICY_OK, "password7");
    AssertResult(PIN_POLICY_OK, "25801");
}

void test_pin_policy_describes_every_result(void) {
    TEST_ASSERT_EQUAL_STRING("ok", pin_policy_describe(PIN_POLICY_OK));
    TEST_ASSERT_NOT_EQUAL(0, std::strcmp("?", pin_policy_describe(PIN_POLICY_REPEATED)));
    TEST_ASSERT_NOT_EQUAL(0, std::strcmp("?", pin_policy_describe(PIN_POLICY_RUN)));
    TEST_ASSERT_NOT_EQUAL(0, std::strcmp("?", pin_policy_describe(PIN_POLICY_COMMON)));
}

}  // namespace

void RegisterPinPolicyTests(void) {
    RUN_TEST(test_pin_policy_accepts_ordinary_pins);
    RUN_TEST(test_pin_policy_refuses_repeats);
    RUN_TEST(test_pin_policy_refuses_runs);
    RUN_TEST(test_pin_policy_a_run_needs_three);
    RUN_TEST(test_pin_policy_refuses_common_pins);
    RUN_TEST(test_pin_policy_common_is_a_whole_match);
    RUN_TEST(test_pin_policy_describes_every_result);
}
