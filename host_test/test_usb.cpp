// The USB port's pure half (devices/usb/usb_logic.h): modes, product IDs,
// the console's line endings.

#include <cstring>

#include "unity.h"
#include "usb_logic.h"

namespace {

void test_usb_modes() {
    TEST_ASSERT_EQUAL_STRING("jtag", usb::Name(usb::Mode::kJtag));
    TEST_ASSERT_EQUAL_STRING("serial", usb::Name(usb::Mode::kSerial));
    TEST_ASSERT_EQUAL_STRING("serial+fido", usb::Name(usb::Mode::kSerialFido));
    TEST_ASSERT_FALSE(usb::OnOtg(usb::Mode::kJtag));
    TEST_ASSERT_TRUE(usb::OnOtg(usb::Mode::kSerial));
    TEST_ASSERT_TRUE(usb::HasFido(usb::Mode::kSerialFido));
    TEST_ASSERT_FALSE(usb::HasFido(usb::Mode::kSerial));
    // A product ID per interface set.
    TEST_ASSERT_EQUAL_HEX16(0x4001, usb::ProductId(usb::Mode::kSerial));
    TEST_ASSERT_EQUAL_HEX16(0x4005, usb::ProductId(usb::Mode::kSerialFido));
    // The card reader: the serial port and the drive, never FIDO2.
    TEST_ASSERT_EQUAL_STRING("reader", usb::Name(usb::Mode::kReader));
    TEST_ASSERT_TRUE(usb::OnOtg(usb::Mode::kReader));
    TEST_ASSERT_TRUE(usb::HasCard(usb::Mode::kReader));
    TEST_ASSERT_FALSE(usb::HasFido(usb::Mode::kReader));
    TEST_ASSERT_FALSE(usb::HasCard(usb::Mode::kSerialFido));
    TEST_ASSERT_EQUAL_HEX16(0x4003, usb::ProductId(usb::Mode::kReader));
}

void test_usb_expand_newlines() {
    char out[16] = {};
    size_t used = 0;
    size_t n = usb::ExpandNewlines("ab\ncd\n", 6, out, sizeof out, &used);
    TEST_ASSERT_EQUAL_UINT(8, n);
    TEST_ASSERT_EQUAL_UINT(6, used);
    TEST_ASSERT_EQUAL_MEMORY("ab\r\ncd\r\n", out, 8);

    // A pair is never split: "\n" waits for the next call.
    n = usb::ExpandNewlines("abc\n", 4, out, 4, &used);
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_EQUAL_UINT(3, used);
    n = usb::ExpandNewlines("\n", 1, out, 1, &used);
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_UINT(0, used);
    n = usb::ExpandNewlines("", 0, out, sizeof out, &used);
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_UINT(0, used);
}

void test_usb_cr_to_lf() {
    char text[] = "help\rok\r\n";
    usb::CrToLf(text, std::strlen(text));
    TEST_ASSERT_EQUAL_STRING("help\nok\n\n", text);
}

}  // namespace

void RegisterUsbTests(void) {
    RUN_TEST(test_usb_modes);
    RUN_TEST(test_usb_expand_newlines);
    RUN_TEST(test_usb_cr_to_lf);
}
