// boards: every board class is consistent, and the pin rules catch the
// mistakes they exist for (shared GPIO, USB/flash/PSRAM pins, a device
// without its pins). The selected board is also checked at compile time.

#include "board.h"
#include "unity.h"

using boards::Board;
using boards::ButtonId;

namespace {

constexpr boards::DevBoard kDev;
constexpr boards::X4Pro kX4;

// A test board built from the dev board.
class Edited : public Board {
public:
    constexpr Edited() { *static_cast<Board *>(this) = kDev; }
};

void test_boards_are_consistent(void) {
    TEST_ASSERT_NULL(boards::CheckPins(kDev));
    TEST_ASSERT_NULL(boards::CheckPins(kX4));
    TEST_ASSERT_NULL(boards::CheckPins(boards::kBoard));
}

void test_boards_dev_board(void) {
    TEST_ASSERT_TRUE(kDev.hw_clock == boards::HwClockType::kNone);
    TEST_ASSERT_TRUE(kDev.display == boards::DisplayType::kNone);
    TEST_ASSERT_TRUE(kDev.led == boards::LedType::kWs2812);
    TEST_ASSERT_EQUAL_INT(48, kDev.led_pins.data);
    TEST_ASSERT_EQUAL_INT(0, kDev.Button(ButtonId::kLeft).gpio);
    TEST_ASSERT_FALSE(kDev.HasButton(ButtonId::kRight));
    TEST_ASSERT_FALSE(kDev.has_sd_card);
    TEST_ASSERT_FALSE(kDev.NeedsI2c());
}

void test_boards_x4_pro(void) {
    TEST_ASSERT_TRUE(kX4.hw_clock == boards::HwClockType::kBm8563);
    TEST_ASSERT_TRUE(kX4.touch == boards::TouchType::kGt911);
    TEST_ASSERT_TRUE(kX4.led == boards::LedType::kNone);
    TEST_ASSERT_TRUE(kX4.has_sd_card);
    TEST_ASSERT_EQUAL_INT(0, kX4.Button(ButtonId::kLeft).gpio);
    TEST_ASSERT_EQUAL_INT(7, kX4.Button(ButtonId::kRight).gpio);
    TEST_ASSERT_EQUAL_INT(3, kX4.Button(ButtonId::kPower).gpio);
    TEST_ASSERT_EQUAL_INT(38, kX4.i2c.scl);
    TEST_ASSERT_EQUAL_INT(39, kX4.i2c.sda);
}

void test_boards_usable_gpios(void) {
    TEST_ASSERT_TRUE(boards::UsableGpio(0));
    TEST_ASSERT_TRUE(boards::UsableGpio(21));
    TEST_ASSERT_TRUE(boards::UsableGpio(38));
    TEST_ASSERT_TRUE(boards::UsableGpio(48));
    TEST_ASSERT_FALSE(boards::UsableGpio(-1));
    TEST_ASSERT_FALSE(boards::UsableGpio(19));  // USB D-
    TEST_ASSERT_FALSE(boards::UsableGpio(20));  // USB D+
    TEST_ASSERT_FALSE(boards::UsableGpio(22));  // no such pin
    TEST_ASSERT_FALSE(boards::UsableGpio(27));  // flash
    TEST_ASSERT_FALSE(boards::UsableGpio(35));  // octal PSRAM
    TEST_ASSERT_FALSE(boards::UsableGpio(49));
}

void test_boards_catch_a_shared_pin(void) {
    Edited b;
    b.led_pins.data = 0;  // the BOOT button's
    TEST_ASSERT_NOT_NULL(boards::CheckPins(b));
}

void test_boards_catch_a_reserved_pin(void) {
    Edited b;
    b.led_pins.data = 36;
    TEST_ASSERT_NOT_NULL(boards::CheckPins(b));
}

void test_boards_catch_a_device_without_pins(void) {
    Edited no_led;
    no_led.led_pins.data = boards::kNoPin;
    TEST_ASSERT_NOT_NULL(boards::CheckPins(no_led));

    Edited rtc_without_i2c;
    rtc_without_i2c.hw_clock = boards::HwClockType::kBm8563;
    TEST_ASSERT_NOT_NULL(boards::CheckPins(rtc_without_i2c));

    Edited sd_without_pins;
    sd_without_pins.has_sd_card = true;
    TEST_ASSERT_NOT_NULL(boards::CheckPins(sd_without_pins));

    Edited no_presence_button;
    no_presence_button.buttons[0].gpio = boards::kNoPin;
    TEST_ASSERT_NOT_NULL(boards::CheckPins(no_presence_button));
}

}  // namespace

void RegisterBoardTests(void) {
    RUN_TEST(test_boards_are_consistent);
    RUN_TEST(test_boards_dev_board);
    RUN_TEST(test_boards_x4_pro);
    RUN_TEST(test_boards_usable_gpios);
    RUN_TEST(test_boards_catch_a_shared_pin);
    RUN_TEST(test_boards_catch_a_reserved_pin);
    RUN_TEST(test_boards_catch_a_device_without_pins);
}
