// GT911 status, Home key latch, info and point decoding
// (devices/touch/gt911_logic.h).

#include "gt911_logic.h"
#include "unity.h"

namespace {

namespace gt = touch::gt911;

void test_touch_home_changes_only_on_a_fresh_frame(void) {
    // No new frame: whatever it was stays (a motionless key makes no frames).
    gt::Frame f = gt::Decode(0x00, true);
    TEST_ASSERT_FALSE(f.fresh);
    TEST_ASSERT_TRUE(f.home);
    f = gt::Decode(0x10, false);  // key bit without ready: not believed
    TEST_ASSERT_FALSE(f.fresh);
    TEST_ASSERT_FALSE(f.home);
    // A fresh frame says it.
    f = gt::Decode(0x90, false);
    TEST_ASSERT_TRUE(f.fresh);
    TEST_ASSERT_TRUE(f.home);
    TEST_ASSERT_EQUAL_UINT8(0, f.touches);
    f = gt::Decode(0x80, true);
    TEST_ASSERT_TRUE(f.fresh);
    TEST_ASSERT_FALSE(f.home);
}

void test_touch_contacts_count(void) {
    const gt::Frame f = gt::Decode(0x82, false);
    TEST_ASSERT_TRUE(f.fresh);
    TEST_ASSERT_EQUAL_UINT8(2, f.touches);
    TEST_ASSERT_EQUAL_UINT8(0, gt::Decode(0x02, false).touches);  // not fresh: none
}

void test_touch_info(void) {
    const uint8_t raw[gt::kInfoSize] = {'9', '1', '1', 0, 0x60, 0x10, 0xE0, 0x01, 0x20, 0x03, 0x05};
    const gt::Info i = gt::DecodeInfo(raw);
    TEST_ASSERT_EQUAL_STRING("911", i.product);
    TEST_ASSERT_EQUAL_HEX16(0x1060, i.firmware);
    TEST_ASSERT_EQUAL_UINT16(480, i.x_max);
    TEST_ASSERT_EQUAL_UINT16(800, i.y_max);
    TEST_ASSERT_EQUAL_HEX8(0x05, i.vendor);
}

void test_touch_config_head(void) {
    const uint8_t raw[gt::kConfigHeadSize] = {0x41, 0xE0, 0x01, 0x20, 0x03, 0xF5, 0x0D};
    const gt::ConfigHead c = gt::DecodeConfigHead(raw);
    TEST_ASSERT_EQUAL_HEX8(0x41, c.version);
    TEST_ASSERT_EQUAL_UINT16(480, c.x_max);
    TEST_ASSERT_EQUAL_UINT16(800, c.y_max);
    TEST_ASSERT_EQUAL_UINT8(5, c.touches);
    TEST_ASSERT_EQUAL_HEX8(0x0D, c.switch1);
}

void test_touch_point(void) {
    // Measured on the X4 Pro, the first of two fingers.
    const uint8_t r[gt::kPointSize] = {0x53, 0x01, 0x52, 0x01, 0x2d, 0x00, 0x00, 0x01};
    const gt::Point p = gt::DecodePoint(r);
    TEST_ASSERT_EQUAL_UINT16(339, p.x);
    TEST_ASSERT_EQUAL_UINT16(338, p.y);
    TEST_ASSERT_EQUAL_UINT16(45, p.size);
    TEST_ASSERT_EQUAL_UINT8(1, p.id);
}

void test_touch_stale_contacts(void) {
    TEST_ASSERT_TRUE(gt::Touching(1, 0));
    TEST_ASSERT_TRUE(gt::Touching(2, gt::kStaleMs));
    TEST_ASSERT_FALSE(gt::Touching(0, 0));                // the lift frame
    TEST_ASSERT_FALSE(gt::Touching(1, gt::kStaleMs + 1));  // lift frame lost
}

void test_touch_edges_keep_every_tap(void) {
    gt::Edges e;
    gt::Edge out = {};
    TEST_ASSERT_FALSE(e.Pop(&out));
    // Two quick taps between two reads: four edges, in order.
    e.Update(true, 10, 20);
    e.Update(true, 11, 21);  // still down: nothing new
    e.Update(false, 0, 0);
    e.Update(true, 30, 40);
    e.Update(false, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(4, e.Pending());
    TEST_ASSERT_TRUE(e.Pop(&out));
    TEST_ASSERT_TRUE(out.down);
    TEST_ASSERT_EQUAL_UINT16(10, out.x);
    TEST_ASSERT_EQUAL_UINT16(20, out.y);
    TEST_ASSERT_TRUE(e.Pop(&out));
    TEST_ASSERT_FALSE(out.down);
    TEST_ASSERT_EQUAL_UINT16(10, out.x);  // the lift where the finger went down
    TEST_ASSERT_TRUE(e.Pop(&out));
    TEST_ASSERT_EQUAL_UINT16(30, out.x);
    TEST_ASSERT_TRUE(e.Pop(&out));
    TEST_ASSERT_FALSE(out.down);
    TEST_ASSERT_FALSE(e.Pop(&out));
    // Nobody reading: full, the rest counted and dropped.
    for (int i = 0; i < gt::Edges::kSize + 4; ++i) {
        e.Update(i % 2 == 0, 1, 1);
    }
    TEST_ASSERT_EQUAL_UINT8(gt::Edges::kSize, e.Pending());
    TEST_ASSERT_EQUAL_UINT32(4, e.Dropped());
}

}  // namespace

void RegisterTouchTests(void) {
    RUN_TEST(test_touch_home_changes_only_on_a_fresh_frame);
    RUN_TEST(test_touch_contacts_count);
    RUN_TEST(test_touch_info);
    RUN_TEST(test_touch_config_head);
    RUN_TEST(test_touch_point);
    RUN_TEST(test_touch_stale_contacts);
    RUN_TEST(test_touch_edges_keep_every_tap);
}
