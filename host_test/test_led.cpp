// led_frames: the UART encoding of a pixel (a wrong bit is a wrong colour) and
// the animator (re-asserting a state must not restart its phase). Ported from
// approver-esp32-yubikey.

#include <cmath>
#include <cstring>

#include "led_frames.h"
#include "unity.h"

namespace {

// --- Scale ---------------------------------------------------------------

void test_led_scale_at_a_hundred_is_the_colour_itself(void) {
    const led::Rgb out = led::Scale(led::colour::kGreen, 100);
    TEST_ASSERT_EQUAL_UINT8(0, out.r);
    TEST_ASSERT_EQUAL_UINT8(255, out.g);
    TEST_ASSERT_EQUAL_UINT8(0, out.b);
}

void test_led_scale_at_zero_is_dark(void) {
    TEST_ASSERT_TRUE(led::Scale(led::colour::kWhite, 0).Dark());
}

void test_led_scale_is_proportional_and_does_not_round_to_nothing(void) {
    // 1 % of 255 must be 2, not 0. A scale that rounded a dim colour away would
    // make the idle ceiling an off switch.
    const led::Rgb dim = led::Scale(led::colour::kWhite, 1);
    TEST_ASSERT_EQUAL_UINT8(2, dim.r);
    TEST_ASSERT_EQUAL_UINT8(2, dim.g);
    TEST_ASSERT_EQUAL_UINT8(2, dim.b);

    const led::Rgb half = led::Scale(led::colour::kWhite, 50);
    TEST_ASSERT_EQUAL_UINT8(127, half.r);
}

void test_led_scale_above_a_hundred_does_not_overflow(void) {
    const led::Rgb out = led::Scale(led::colour::kWhite, 200);
    TEST_ASSERT_EQUAL_UINT8(255, out.r);
}

// --- The wire ------------------------------------------------------------

void test_led_encode_writes_twelve_bytes_and_refuses_less_room(void) {
    uint8_t frame[led::kBytesPerPixel];
    TEST_ASSERT_EQUAL_UINT32(led::kBytesPerPixel,
                             led::EncodePixel(led::colour::kWhite, frame, sizeof(frame)));
    TEST_ASSERT_EQUAL_UINT32(0, led::EncodePixel(led::colour::kWhite, frame, sizeof(frame) - 1));
    TEST_ASSERT_EQUAL_UINT32(0, led::EncodePixel(led::colour::kWhite, nullptr, sizeof(frame)));
}

void test_led_encode_puts_green_first(void) {
    // **GRB, which is what the part reads.** Getting this backwards swaps red
    // and green — which on this device swaps `allow` and `deny` on the verdict
    // flash, and is the single worst way this file could be wrong.
    uint8_t frame[led::kBytesPerPixel];
    led::EncodePixel(led::Rgb{0xFF, 0x00, 0x00}, frame, sizeof(frame));

    // 0x00 is four copies of the `00` character; 0xFF is four of the `11` one.
    const uint8_t zero = 0x37;
    const uint8_t ones = 0x04;
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(zero, frame[i], "green must come first and be dark");
    }
    for (int i = 4; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(ones, frame[i], "red is the second group");
    }
    for (int i = 8; i < 12; i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(zero, frame[i], "blue is the third group");
    }
}

void test_led_encode_uses_only_the_four_characters(void) {
    // Every byte on the wire must be one of the four the inverted-UART table
    // defines. Anything else is a bit cell of the wrong width.
    uint8_t frame[led::kBytesPerPixel];
    const led::Rgb awkward{0x1B, 0xA5, 0x4E};
    led::EncodePixel(awkward, frame, sizeof(frame));
    for (size_t i = 0; i < sizeof(frame); i++) {
        const bool known = frame[i] == 0x37 || frame[i] == 0x07 || frame[i] == 0x34 ||
                           frame[i] == 0x04;
        TEST_ASSERT_TRUE_MESSAGE(known, "an encoded byte is not one of the four");
    }
}

void test_led_encode_round_trips_every_two_bit_group(void) {
    // Decode the frame back by hand and compare. This is the check that would
    // catch a table whose entries were transposed — which the test above would
    // not, because a transposed table still only emits the four characters.
    auto decode = [](const uint8_t *four) {
        uint8_t value = 0;
        for (int i = 0; i < 4; i++) {
            uint8_t bits = 0;
            switch (four[i]) {
                case 0x37: bits = 0; break;
                case 0x07: bits = 1; break;
                case 0x34: bits = 2; break;
                default: bits = 3; break;
            }
            value = static_cast<uint8_t>((value << 2) | bits);
        }
        return value;
    };

    uint8_t frame[led::kBytesPerPixel];
    const led::Rgb colour{0x12, 0x34, 0x56};
    led::EncodePixel(colour, frame, sizeof(frame));
    TEST_ASSERT_EQUAL_UINT8(colour.g, decode(frame + 0));
    TEST_ASSERT_EQUAL_UINT8(colour.r, decode(frame + 4));
    TEST_ASSERT_EQUAL_UINT8(colour.b, decode(frame + 8));
}

// --- The animator --------------------------------------------------------

void test_led_solid_stays_on_and_asks_to_be_asked_again(void) {
    led::Animator a;
    a.Set(led::colour::kGreen, led::Effect::kSolid, 100, 0);

    uint32_t next = 0;
    TEST_ASSERT_TRUE(a.FrameAt(0, &next) == led::colour::kGreen);
    // Never zero: a caller that slept for the answer would spin.
    TEST_ASSERT_TRUE(next > 0);
    TEST_ASSERT_TRUE(a.FrameAt(5000, &next) == led::colour::kGreen);
}

void test_led_a_blink_is_dark_for_its_off_half(void) {
    led::Animator a;
    a.Set(led::colour::kRed, led::Effect::kFastBlink, 100, 1000);

    uint32_t next = 0;
    TEST_ASSERT_FALSE(a.FrameAt(1000, &next).Dark());   // 0 ms in: on
    TEST_ASSERT_FALSE(a.FrameAt(1150, &next).Dark());   // 150 ms in: still on
    TEST_ASSERT_TRUE(a.FrameAt(1250, &next).Dark());    // 250 ms in: off
    TEST_ASSERT_FALSE(a.FrameAt(1450, &next).Dark());   // 450 ms in: on again
}

void test_led_the_next_frame_lands_on_the_change(void) {
    // The whole point of `next_ms`: a caller that sleeps exactly that long wakes
    // when the frame is due, not before and not after.
    led::Animator a;
    a.Set(led::colour::kRed, led::Effect::kFastBlink, 100, 0);

    uint32_t next = 0;
    a.FrameAt(50, &next);
    TEST_ASSERT_EQUAL_UINT32(150, next);  // 200 ms on, 50 ms in
    a.FrameAt(250, &next);
    TEST_ASSERT_EQUAL_UINT32(150, next);  // 400 ms period, 250 ms in
}

void test_led_re_asserting_the_same_state_does_not_restart_the_phase(void) {
    // **The rule this class exists for.** `indicator` calls `Set` every tick;
    // an animator that restarted the phase would pin a beacon permanently on.
    led::Animator a;
    a.Set(led::colour::kRed, led::Effect::kFastBlink, 100, 0);

    uint32_t next = 0;
    TEST_ASSERT_FALSE(a.FrameAt(0, &next).Dark());

    // Half a second later, re-asserted every 100 ms the way a tick would.
    for (uint32_t t = 100; t <= 500; t += 100) {
        a.Set(led::colour::kRed, led::Effect::kFastBlink, 100, t);
    }
    // 500 ms into a 200/200 blink is the on half of the second cycle... and the
    // point is that it is *somewhere in the cycle*, not stuck at the start.
    bool saw_dark = false;
    for (uint32_t t = 0; t < 400; t += 50) {
        a.Set(led::colour::kRed, led::Effect::kFastBlink, 100, 500 + t);
        if (a.FrameAt(500 + t, &next).Dark()) {
            saw_dark = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(saw_dark, "a re-asserted blink stopped blinking");
}

void test_led_a_changed_state_does_restart_the_phase(void) {
    led::Animator a;
    a.Set(led::colour::kRed, led::Effect::kFastBlink, 100, 0);
    uint32_t next = 0;
    a.FrameAt(250, &next);  // in the dark half

    a.Set(led::colour::kGreen, led::Effect::kFastBlink, 100, 250);
    // A new state starts lit, so that a change is visible immediately rather
    // than after up to a full off-period of nothing.
    TEST_ASSERT_FALSE(a.FrameAt(250, &next).Dark());
}

void test_led_breathe_walks_the_ramp_and_reaches_both_ends(void) {
    led::Animator a;
    a.Set(led::colour::kGreen, led::Effect::kBreathe, 100, 0);

    uint32_t next = 0;
    uint8_t low = 255;
    uint8_t high = 0;
    for (size_t step = 0; step < led::kBreathSteps; step++) {
        const led::Rgb frame = a.FrameAt(static_cast<uint32_t>(step) * led::kBreathStepMs, &next);
        if (frame.g < low) low = frame.g;
        if (frame.g > high) high = frame.g;
    }
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, low, "the breath never reaches dark");
    // **Exactly the ceiling, not nearly it.** A breath is the operator's
    // brightness setting swept from off to that setting and back, so the top of
    // the sweep has to be the same number a solid frame would show — otherwise
    // `config set led 15` means one thing when the device is idle and another
    // when it is not.
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(255, high, "the breath never reaches the ceiling");
}

// **The regression test for a breath that looked like a switch.** The ramp is a
// duty cycle and the eye reads roughly its cube root, so a table that walks
// *duty* evenly — or worse, walks it along the perceptual curve instead of its
// inverse, which is what this firmware shipped first — spends one step getting to
// most of its apparent brightness and the remaining four seconds going nowhere.
// What it looked like on the desk: a snap to full, eight seconds of sitting there,
// and a gradient visible only at the very bottom.
//
// So the property under test is not the table's shape, which looks wrong on
// purpose. It is that **apparent** brightness moves by about the same
// amount at every step, measured with the same model the table was generated
// from — CIE L\*, including its linear segment near black, which a bare `cbrt`
// gets badly wrong exactly where this table spends half its entries.
double Lstar(uint8_t duty) {
    const double y = duty / 255.0;
    return y <= 0.008856 ? 903.3 * y : 116.0 * std::cbrt(y) - 16.0;
}

void test_led_breathe_is_linear_in_what_the_eye_sees(void) {
    const size_t half = led::kBreathSteps / 2;
    // An even sweep is 100 / 89 = 1.12 L\* a step. The budget is wider than that
    // because the bottom of the travel is quantisation-limited rather than
    // curve-limited: one duty level out of 255 is 3.5 L\* down there, and no
    // 8-bit part can do better. What it still excludes by a factor of twelve is
    // the table this replaced, whose *first* step was 62.8.
    const double budget = 5.0;

    double previous = 0.0;
    for (size_t step = 1; step < half; step++) {
        const double seen = Lstar(led::kBreathRamp[step]);
        TEST_ASSERT_TRUE_MESSAGE(seen >= previous, "the rising half is not monotone");
        TEST_ASSERT_TRUE_MESSAGE(seen - previous <= budget,
                                 "one step of the breath is a visible jump");
        previous = seen;
    }
    TEST_ASSERT_TRUE_MESSAGE(previous > 99.0, "the rising half does not reach the top");

    // And it is symmetric: what goes up comes back down the same way, so the
    // breath has no fast edge on either side.
    for (size_t step = 0; step < half; step++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(led::kBreathRamp[step],
                                        led::kBreathRamp[led::kBreathSteps - 1 - step],
                                        "the breath is not symmetric");
    }
}

// The halfway point of the travel is the shortest statement of the same bug: half
// way through the sweep the emitter should look about half as bright, which on a
// linear-duty part is well under a quarter of full duty. The table this replaced
// was at 216 of 255 here — 94 % of the way there, a quarter of the way in.
void test_led_breathe_at_half_travel_looks_half_bright(void) {
    const uint8_t mid = led::kBreathRamp[led::kBreathSteps / 4];
    TEST_ASSERT_TRUE_MESSAGE(mid > 30 && mid < 70, "half way up the breath is not half bright");
}

// A breath under the idle ceiling is where the two divisions used to compound:
// scaling to 8 % and *then* applying the ramp rounds twice, and the second
// rounding lands on a curve whose bottom half is one duty level wide. The whole
// sweep still has to be a sweep — several distinct levels, not on and off.
void test_led_breathe_at_the_idle_ceiling_is_still_a_sweep(void) {
    led::Animator a;
    a.Set(led::colour::kGreen, led::Effect::kBreathe, 8, 0);

    bool seen[256] = {};
    uint32_t next = 0;
    size_t distinct = 0;
    for (size_t step = 0; step < led::kBreathSteps; step++) {
        const uint8_t g = a.FrameAt(static_cast<uint32_t>(step) * led::kBreathStepMs, &next).g;
        if (!seen[g]) {
            seen[g] = true;
            distinct++;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(distinct >= 15, "the idle breath is not a sweep");
    TEST_ASSERT_TRUE_MESSAGE(seen[0], "the idle breath never reaches dark");
    // 8 % of 255, which is what a solid frame at the same ceiling shows.
    TEST_ASSERT_TRUE_MESSAGE(seen[led::Scale(led::colour::kGreen, 8).g],
                             "the idle breath never reaches its own ceiling");
}

void test_led_an_override_expires_and_the_state_underneath_comes_back(void) {
    led::Animator a;
    a.Set(led::colour::kGreen, led::Effect::kSolid, 100, 0);
    a.SetFor(led::colour::kRed, led::Effect::kSolid, 100, 1000, 0);

    uint32_t next = 0;
    TEST_ASSERT_TRUE(a.FrameAt(0, &next) == led::colour::kRed);
    TEST_ASSERT_TRUE(a.Overriding());
    // And the sleep is shortened so the fall-back lands on time rather than a
    // whole refresh period late.
    TEST_ASSERT_TRUE(next <= 1000);

    TEST_ASSERT_TRUE(a.FrameAt(1000, &next) == led::colour::kGreen);
    TEST_ASSERT_FALSE(a.Overriding());
}

void test_led_a_state_change_under_a_running_override_is_what_it_falls_back_to(void) {
    // A verdict flash has to finish being seen; the device moving on underneath
    // it is the ordinary case rather than a conflict.
    led::Animator a;
    a.Set(led::colour::kGreen, led::Effect::kSolid, 100, 0);
    a.SetFor(led::colour::kRed, led::Effect::kSolid, 100, 1000, 0);
    a.Set(led::colour::kCyan, led::Effect::kSolid, 100, 200);

    uint32_t next = 0;
    TEST_ASSERT_TRUE(a.FrameAt(500, &next) == led::colour::kRed);
    TEST_ASSERT_TRUE(a.FrameAt(1000, &next) == led::colour::kCyan);
}

void test_led_an_override_can_be_ended_early(void) {
    // **The one caller that needs this is a prompt rather than a flash.** A verdict
    // has a duration of its own and must not be cut short; "touch the key now" is
    // true only until the key is touched, and a light still asking for a fingertip
    // after the fingertip arrived is a light that lies. So ending one early is an
    // explicit call and not a `Set`.
    led::Animator a;
    a.Set(led::colour::kMagenta, led::Effect::kNormBlink, 100, 0);
    a.SetFor(led::colour::kBlue, led::Effect::kFastBlink, 100, 30000, 0);

    uint32_t next = 0;
    TEST_ASSERT_TRUE(a.FrameAt(0, &next) == led::colour::kBlue);
    TEST_ASSERT_TRUE(a.Overriding());

    a.EndFor(500);
    TEST_ASSERT_FALSE(a.Overriding());
    // And the state underneath is back at once, not at the end of the 30 s.
    TEST_ASSERT_TRUE(a.FrameAt(500, &next) == led::colour::kMagenta);
}

void test_led_ending_an_override_that_is_not_running_changes_nothing(void) {
    // The console calls this after every wait, including the ones that never put
    // a prompt up — so it has to be a no-op rather than a reset of the phase.
    led::Animator a;
    a.Set(led::colour::kGreen, led::Effect::kSolid, 100, 0);
    a.EndFor(100);
    TEST_ASSERT_FALSE(a.Overriding());

    uint32_t next = 0;
    TEST_ASSERT_TRUE(a.FrameAt(200, &next) == led::colour::kGreen);
}

void test_led_a_prompt_ended_early_does_not_resurrect(void) {
    // Ended means ended: the original duration must not bring it back on a later
    // frame, which is what would happen if `EndFor` only moved a deadline.
    led::Animator a;
    a.Set(led::colour::kGreen, led::Effect::kSolid, 100, 0);
    a.SetFor(led::colour::kBlue, led::Effect::kFastBlink, 100, 30000, 0);
    a.EndFor(10);

    uint32_t next = 0;
    TEST_ASSERT_TRUE(a.FrameAt(20, &next) == led::colour::kGreen);
    TEST_ASSERT_TRUE(a.FrameAt(5000, &next) == led::colour::kGreen);
    TEST_ASSERT_FALSE(a.Overriding());
}

void test_led_times_survive_the_millisecond_wrap(void) {
    // `uint32_t` milliseconds wrap at ~49 days and are only ever subtracted.
    const uint32_t before_wrap = 0xFFFFFF00u;
    led::Animator a;
    a.Set(led::colour::kRed, led::Effect::kFastBlink, 100, before_wrap);

    uint32_t next = 0;
    TEST_ASSERT_FALSE(a.FrameAt(before_wrap, &next).Dark());
    // 256 ms later, which is 0x00000000 — past the wrap, into the off half.
    TEST_ASSERT_TRUE(a.FrameAt(0u, &next).Dark());
}

}  // namespace

void RegisterLedTests(void) {
    RUN_TEST(test_led_scale_at_a_hundred_is_the_colour_itself);
    RUN_TEST(test_led_scale_at_zero_is_dark);
    RUN_TEST(test_led_scale_is_proportional_and_does_not_round_to_nothing);
    RUN_TEST(test_led_scale_above_a_hundred_does_not_overflow);

    RUN_TEST(test_led_encode_writes_twelve_bytes_and_refuses_less_room);
    RUN_TEST(test_led_encode_puts_green_first);
    RUN_TEST(test_led_encode_uses_only_the_four_characters);
    RUN_TEST(test_led_encode_round_trips_every_two_bit_group);

    RUN_TEST(test_led_solid_stays_on_and_asks_to_be_asked_again);
    RUN_TEST(test_led_a_blink_is_dark_for_its_off_half);
    RUN_TEST(test_led_the_next_frame_lands_on_the_change);
    RUN_TEST(test_led_re_asserting_the_same_state_does_not_restart_the_phase);
    RUN_TEST(test_led_a_changed_state_does_restart_the_phase);
    RUN_TEST(test_led_breathe_walks_the_ramp_and_reaches_both_ends);
    RUN_TEST(test_led_breathe_is_linear_in_what_the_eye_sees);
    RUN_TEST(test_led_breathe_at_half_travel_looks_half_bright);
    RUN_TEST(test_led_breathe_at_the_idle_ceiling_is_still_a_sweep);
    RUN_TEST(test_led_an_override_expires_and_the_state_underneath_comes_back);
    RUN_TEST(test_led_a_state_change_under_a_running_override_is_what_it_falls_back_to);
    RUN_TEST(test_led_an_override_can_be_ended_early);
    RUN_TEST(test_led_ending_an_override_that_is_not_running_changes_nothing);
    RUN_TEST(test_led_a_prompt_ended_early_does_not_resurrect);
    RUN_TEST(test_led_times_survive_the_millisecond_wrap);
}
