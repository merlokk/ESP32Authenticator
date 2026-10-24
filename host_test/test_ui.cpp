// The portrait screen on the landscape panel, the full-refresh plan
// (components/ui/ui_logic.h); the PIN pad (pin_pad_logic.h); the status bar
// (status_logic.h), the navigation (nav_logic.h) and the list (list_logic.h).

#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "dialog_logic.h"
#include "fido_list_logic.h"
#include "fido_prompt_logic.h"
#include "settings_logic.h"
#include "wifi_view_logic.h"
#include "touch_cal_logic.h"
#include "ble_view_logic.h"
#include "display_view_logic.h"
#include "time_view_logic.h"
#include "security_view_logic.h"
#include "web_view_logic.h"
#include "list_logic.h"
#include "nav_logic.h"
#include "pin_pad_logic.h"
#include "status_logic.h"
#include "ui_logic.h"
#include "unity.h"

namespace {

bool White(const std::vector<uint8_t> &buf, size_t row_bytes, int x, int y) {
    return (buf[y * row_bytes + x / 8] & (0x80 >> (x % 8))) != 0;
}

void SetBlack(std::vector<uint8_t> &buf, size_t row_bytes, int x, int y) {
    buf[y * row_bytes + x / 8] = static_cast<uint8_t>(buf[y * row_bytes + x / 8] & ~(0x80 >> (x % 8)));
}

void test_ui_sizes(void) {
    TEST_ASSERT_EQUAL_UINT16(480, ui::kWidth);
    TEST_ASSERT_EQUAL_UINT16(800, ui::kHeight);
    TEST_ASSERT_EQUAL_size_t(epd::kFrameBytes, ui::kScreenBytes);
    // The portrait top is the frame's left (held as a book).
    TEST_ASSERT_EQUAL_UINT8(epd::kInsetLeft, ui::kInsetTop);
}

void test_ui_corners(void) {
    // Portrait top left is frame bottom left; portrait top right, frame top left.
    TEST_ASSERT_EQUAL_UINT16(0, ui::PanelX(0, 0));
    TEST_ASSERT_EQUAL_UINT16(479, ui::PanelY(0, 0));
    TEST_ASSERT_EQUAL_UINT16(0, ui::PanelX(479, 0));
    TEST_ASSERT_EQUAL_UINT16(0, ui::PanelY(479, 0));
    TEST_ASSERT_EQUAL_UINT16(799, ui::PanelX(0, 799));
    TEST_ASSERT_EQUAL_UINT16(799, ui::PanelX(479, 799));
    TEST_ASSERT_EQUAL_UINT16(0, ui::PanelY(479, 799));
}

void test_ui_to_panel_every_pixel_once(void) {
    std::vector<uint8_t> portrait(ui::kScreenBytes, 0xFF);
    std::vector<uint8_t> frame(epd::kFrameBytes);
    const int dots[][2] = {{0, 0}, {479, 0}, {0, 799}, {479, 799}, {7, 8}, {8, 7}, {123, 456}};
    for (const auto &d : dots) {
        SetBlack(portrait, ui::kRowBytes, d[0], d[1]);
    }
    ui::ToPanel(portrait.data(), frame.data());
    int black = 0;
    for (int y = 0; y < epd::kHeight; ++y) {
        for (int x = 0; x < epd::kWidth; ++x) {
            black += White(frame, epd::kRowBytes, x, y) ? 0 : 1;
        }
    }
    TEST_ASSERT_EQUAL_INT(7, black);
    for (const auto &d : dots) {
        const auto x = static_cast<uint16_t>(d[0]);
        const auto y = static_cast<uint16_t>(d[1]);
        TEST_ASSERT_FALSE(White(frame, epd::kRowBytes, ui::PanelX(x, y), ui::PanelY(x, y)));
    }
}

void test_ui_refresh_plan(void) {
    ui::RefreshPlan plan;
    TEST_ASSERT_TRUE(plan.NextIsFull());
    for (int i = 1; i < ui::RefreshPlan::kFullEvery; ++i) {
        TEST_ASSERT_FALSE(plan.NextIsFull());
    }
    TEST_ASSERT_TRUE(plan.NextIsFull());
    TEST_ASSERT_FALSE(plan.NextIsFull());
    // A new screen is a full refresh, and the count starts over from it.
    TEST_ASSERT_FALSE(plan.NextIsFull(ui::RefreshPlan::kBigChange - 1));
    TEST_ASSERT_TRUE(plan.NextIsFull(ui::RefreshPlan::kBigChange));
    for (int i = 1; i < ui::RefreshPlan::kFullEvery; ++i) {
        TEST_ASSERT_FALSE(plan.NextIsFull(10));
    }
    TEST_ASSERT_TRUE(plan.NextIsFull(10));
}

void test_ui_changed_pixels(void) {
    const uint8_t a[3] = {0xFF, 0x00, 0xA5};
    const uint8_t b[3] = {0xFF, 0x81, 0x5A};
    TEST_ASSERT_EQUAL_UINT32(10, ui::ChangedPixels(a, b, 3));
    TEST_ASSERT_EQUAL_UINT32(0, ui::ChangedPixels(a, a, 3));
}

void test_ui_touch_to_screen(void) {
    // The sensor's corners land on the screen's corners (measured on the X4 Pro).
    ui::ScreenPoint p = ui::TouchToScreen(0, 0);
    TEST_ASSERT_EQUAL_UINT16(0, p.x);
    TEST_ASSERT_EQUAL_UINT16(0, p.y);
    p = ui::TouchToScreen(479, 799);
    TEST_ASSERT_EQUAL_UINT16(479, p.x);
    TEST_ASSERT_EQUAL_UINT16(799, p.y);
    p = ui::TouchToScreen(100, 700);
    TEST_ASSERT_EQUAL_UINT16(100, p.x);
    TEST_ASSERT_EQUAL_UINT16(700, p.y);
    // Past the edge: clamped onto it.
    p = ui::TouchToScreen(480, 800);
    TEST_ASSERT_EQUAL_UINT16(479, p.x);
    TEST_ASSERT_EQUAL_UINT16(799, p.y);
}

namespace pp = ui::pinpad;

pp::Step Type(pp::Pad *pad, const char *digits) {
    pp::Step last = pp::Step::kNone;
    for (const char *c = digits; *c != '\0'; ++c) {
        last = pad->Apply({pp::Key::kDigit, *c});
    }
    return last;
}

void test_pin_pad_keys(void) {
    TEST_ASSERT_EQUAL_CHAR('1', pp::KeyAt(0).digit);
    TEST_ASSERT_EQUAL_CHAR('9', pp::KeyAt(8).digit);
    TEST_ASSERT_TRUE(pp::KeyAt(9).key == pp::Key::kClear);
    TEST_ASSERT_EQUAL_CHAR('0', pp::KeyAt(10).digit);
    TEST_ASSERT_TRUE(pp::KeyAt(11).key == pp::Key::kBack);
    TEST_ASSERT_TRUE(pp::KeyAt(12).key == pp::Key::kNone);
    TEST_ASSERT_TRUE(pp::KeyAt(0xFFFF).key == pp::Key::kNone);  // LV_BUTTONMATRIX_BUTTON_NONE
}

void test_pin_pad_enter(void) {
    pp::Pad pad;
    pad.Start(false);
    TEST_ASSERT_TRUE(pad.stage() == pp::Stage::kEnter);
    TEST_ASSERT_TRUE(pad.Apply({pp::Key::kBack, 0}) == pp::Step::kNone);  // nothing to erase
    TEST_ASSERT_TRUE(Type(&pad, "12") == pp::Step::kTyped);
    TEST_ASSERT_TRUE(pad.Apply({pp::Key::kBack, 0}) == pp::Step::kTyped);
    TEST_ASSERT_EQUAL_UINT8(1, pad.count());
    TEST_ASSERT_TRUE(pad.Apply({pp::Key::kClear, 0}) == pp::Step::kTyped);
    TEST_ASSERT_EQUAL_UINT8(0, pad.count());
    TEST_ASSERT_TRUE(Type(&pad, "12345") == pp::Step::kTyped);
    TEST_ASSERT_TRUE(Type(&pad, "6") == pp::Step::kCheck);
    TEST_ASSERT_TRUE(pad.stage() == pp::Stage::kChecking);
    TEST_ASSERT_EQUAL_STRING("123456", pad.Pin());
    // While it is checked, keys do nothing.
    TEST_ASSERT_TRUE(Type(&pad, "7") == pp::Step::kNone);
    TEST_ASSERT_TRUE(pad.Apply({pp::Key::kClear, 0}) == pp::Step::kNone);
    TEST_ASSERT_EQUAL_STRING("123456", pad.Pin());
    // Wrong: asked again, the digits gone.
    pad.Done(false);
    TEST_ASSERT_TRUE(pad.stage() == pp::Stage::kEnter);
    TEST_ASSERT_EQUAL_UINT8(0, pad.count());
    TEST_ASSERT_EQUAL_STRING("", pad.Pin());
}

void test_pin_pad_new_pin_twice(void) {
    pp::Pad pad;
    pad.Start(true);
    TEST_ASSERT_TRUE(pad.stage() == pp::Stage::kNew);
    TEST_ASSERT_TRUE(Type(&pad, "246801") == pp::Step::kRepeat);
    TEST_ASSERT_TRUE(pad.stage() == pp::Stage::kRepeat);
    TEST_ASSERT_EQUAL_UINT8(0, pad.count());
    TEST_ASSERT_EQUAL_STRING("", pad.Pin());  // the first one is not shown again
    // A different one: a new PIN from the start.
    TEST_ASSERT_TRUE(Type(&pad, "246802") == pp::Step::kMismatch);
    TEST_ASSERT_TRUE(pad.stage() == pp::Stage::kNew);
    TEST_ASSERT_EQUAL_UINT8(0, pad.count());
    TEST_ASSERT_TRUE(Type(&pad, "246801") == pp::Step::kRepeat);
    TEST_ASSERT_TRUE(Type(&pad, "246801") == pp::Step::kCheck);
    TEST_ASSERT_EQUAL_STRING("246801", pad.Pin());
    // Refused (too simple, say): a new one again.
    pad.Done(true);
    TEST_ASSERT_TRUE(pad.stage() == pp::Stage::kNew);
}

void test_pin_pad_texts(void) {
    TEST_ASSERT_EQUAL_UINT8(9, pp::TriesLeft(1, 10));
    TEST_ASSERT_EQUAL_UINT8(0, pp::TriesLeft(10, 10));
    TEST_ASSERT_EQUAL_UINT8(0, pp::TriesLeft(12, 10));
    TEST_ASSERT_EQUAL_UINT32(1, pp::MinutesUp(1));
    TEST_ASSERT_EQUAL_UINT32(1, pp::MinutesUp(60));
    TEST_ASSERT_EQUAL_UINT32(10, pp::MinutesUp(599));
}

void test_status_texts(void) {
    namespace st = ui::status;
    char t[st::kTextSize];
    st::Status s;
    st::Left(s, t, sizeof t);
    TEST_ASSERT_EQUAL_STRING("", t);
    st::Right(s, t, sizeof t);
    TEST_ASSERT_EQUAL_STRING("", t);

    s.wifi = st::Wifi::kOnline;
    s.ble = st::Ble::kWaiting;
    s.sd = true;
    st::Left(s, t, sizeof t);
    std::string want = std::string(st::kWifi) + "   " + st::kBluetooth + " ...   " + st::kSdCard;
    TEST_ASSERT_EQUAL_STRING(want.c_str(), t);
    s.wifi = st::Wifi::kAp;
    s.ble = st::Ble::kOff;
    s.sd = false;
    st::Left(s, t, sizeof t);
    want = std::string(st::kWifi) + " AP";
    TEST_ASSERT_EQUAL_STRING(want.c_str(), t);

    s.time_set = true;
    s.hour = 9;
    s.minute = 5;
    st::Right(s, t, sizeof t);
    TEST_ASSERT_EQUAL_STRING("09:05", t);
    s.battery = 75;
    s.charging = true;
    st::Right(s, t, sizeof t);
    want = std::string("09:05   ") + st::kCharge + st::kBattery[3] + " 75%";
    TEST_ASSERT_EQUAL_STRING(want.c_str(), t);
    s.time_set = false;
    s.charging = false;
    s.battery = 100;
    st::Right(s, t, sizeof t);
    want = std::string(st::kBattery[4]) + " 100%";
    TEST_ASSERT_EQUAL_STRING(want.c_str(), t);
    // A small buffer: cut, terminated.
    char small[4];
    st::Right(s, small, sizeof small);
    TEST_ASSERT_EQUAL_size_t(3, strlen(small));
}

void test_status_battery(void) {
    namespace st = ui::status;
    TEST_ASSERT_EQUAL_UINT8(0, st::BatteryBars(5));
    TEST_ASSERT_EQUAL_UINT8(1, st::BatteryBars(25));
    TEST_ASSERT_EQUAL_UINT8(2, st::BatteryBars(50));
    TEST_ASSERT_EQUAL_UINT8(3, st::BatteryBars(75));
    TEST_ASSERT_EQUAL_UINT8(4, st::BatteryBars(95));
    // Noise of a percent is not shown; two are; the ends always.
    TEST_ASSERT_EQUAL_INT8(60, st::ShownLevel(60, 61));
    TEST_ASSERT_EQUAL_INT8(60, st::ShownLevel(60, 59));
    TEST_ASSERT_EQUAL_INT8(62, st::ShownLevel(60, 62));
    TEST_ASSERT_EQUAL_INT8(100, st::ShownLevel(99, 100));
    TEST_ASSERT_EQUAL_INT8(40, st::ShownLevel(-1, 40));
    TEST_ASSERT_EQUAL_INT8(-1, st::ShownLevel(40, -1));
}

void test_nav_pages_and_tabs(void) {
    using ui::nav::Input;
    ui::nav::Nav n;
    TEST_ASSERT_EQUAL_UINT8(1, n.pages());
    TEST_ASSERT_FALSE(n.Apply(Input::kRight));  // one page: nothing to flip
    n.SetPages(6);
    TEST_ASSERT_EQUAL_UINT8(4, n.tabs());
    TEST_ASSERT_EQUAL_UINT8(0, n.first());
    // Right: pages in turn; the tab row scrolls to keep the page in it.
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_TRUE(n.Apply(Input::kRight));
    }
    TEST_ASSERT_EQUAL_UINT8(4, n.page());
    TEST_ASSERT_EQUAL_UINT8(1, n.first());
    n.Apply(Input::kRight);
    TEST_ASSERT_EQUAL_UINT8(5, n.page());
    TEST_ASSERT_EQUAL_UINT8(2, n.first());
    // Round: after the last, the first.
    n.Apply(Input::kRight);
    TEST_ASSERT_EQUAL_UINT8(0, n.page());
    TEST_ASSERT_EQUAL_UINT8(0, n.first());
    n.Apply(Input::kLeft);
    TEST_ASSERT_EQUAL_UINT8(5, n.page());
    TEST_ASSERT_EQUAL_UINT8(2, n.first());
    // A tab: the page under it; the row stays.
    TEST_ASSERT_TRUE(n.Tab(1));
    TEST_ASSERT_EQUAL_UINT8(3, n.page());
    TEST_ASSERT_EQUAL_UINT8(2, n.first());
    TEST_ASSERT_FALSE(n.Tab(1));
    TEST_ASSERT_FALSE(n.Tab(4));
    // Records gone: the page and the row stay inside.
    TEST_ASSERT_TRUE(n.SetPages(3));
    TEST_ASSERT_EQUAL_UINT8(2, n.page());
    TEST_ASSERT_EQUAL_UINT8(0, n.first());
    TEST_ASSERT_EQUAL_UINT8(3, n.tabs());
    // Home: the first page.
    TEST_ASSERT_TRUE(n.Apply(Input::kHome));
    TEST_ASSERT_EQUAL_UINT8(0, n.page());
    TEST_ASSERT_FALSE(n.Apply(Input::kHome));
    TEST_ASSERT_EQUAL_UINT8(1, ui::nav::PagesFor(0, 6));
    TEST_ASSERT_EQUAL_UINT8(2, ui::nav::PagesFor(7, 6));
}

void test_nav_swipes(void) {
    using ui::nav::Gesture;
    using ui::nav::Mode;
    ui::nav::Nav n;
    n.SetPages(3);
    TEST_ASSERT_FALSE(n.Swipe(Gesture::kDown));  // not from the top
    TEST_ASSERT_TRUE(n.Swipe(Gesture::kDownFromTop));
    TEST_ASSERT_TRUE(n.mode() == Mode::kSettings);
    // In Settings the buttons and tabs do nothing; a swipe up closes it.
    TEST_ASSERT_FALSE(n.Apply(ui::nav::Input::kRight));
    TEST_ASSERT_FALSE(n.Tab(1));
    TEST_ASSERT_FALSE(n.Swipe(Gesture::kDownFromTop));
    TEST_ASSERT_TRUE(n.Swipe(Gesture::kUp));
    TEST_ASSERT_TRUE(n.mode() == Mode::kList);
    TEST_ASSERT_TRUE(n.Swipe(Gesture::kUpFromBottom));
    TEST_ASSERT_TRUE(n.mode() == Mode::kFido);
    TEST_ASSERT_TRUE(n.Swipe(Gesture::kDown));
    TEST_ASSERT_TRUE(n.mode() == Mode::kList);
    // The back arrow: the list at its page.
    n.Apply(ui::nav::Input::kRight);
    n.Swipe(Gesture::kDownFromTop);
    TEST_ASSERT_TRUE(n.Back());
    TEST_ASSERT_TRUE(n.mode() == Mode::kList);
    TEST_ASSERT_EQUAL_UINT8(1, n.page());
    TEST_ASSERT_FALSE(n.Back());
    // A screen by name (console ui show): the pages stay.
    TEST_ASSERT_TRUE(n.Open(Mode::kFido));
    TEST_ASSERT_FALSE(n.Open(Mode::kFido));
    TEST_ASSERT_TRUE(n.Open(Mode::kList));
    TEST_ASSERT_EQUAL_UINT8(1, n.page());
    // Home closes them too.
    n.Swipe(Gesture::kUpFromBottom);
    TEST_ASSERT_TRUE(n.Apply(ui::nav::Input::kHome));
    TEST_ASSERT_TRUE(n.mode() == Mode::kList);
}

void test_nav_fido_pages(void) {
    using ui::nav::Gesture;
    using ui::nav::Input;
    ui::nav::Nav n;
    n.SetPages(3);
    n.Swipe(Gesture::kUpFromBottom);
    TEST_ASSERT_FALSE(n.Apply(Input::kRight));  // one page
    n.SetFidoPages(3);
    // Left / Right flip the FIDO2 pages (round), not the list's.
    TEST_ASSERT_TRUE(n.Apply(Input::kLeft));
    TEST_ASSERT_EQUAL_UINT8(2, n.fido_page());
    TEST_ASSERT_EQUAL_UINT8(0, n.page());
    TEST_ASSERT_TRUE(n.Apply(Input::kRight));
    TEST_ASSERT_EQUAL_UINT8(0, n.fido_page());
    // The bar under the list: back, the page (nothing), forward.
    TEST_ASSERT_TRUE(n.Tab(2));
    TEST_ASSERT_EQUAL_UINT8(1, n.fido_page());
    TEST_ASSERT_TRUE(n.Tab(0));
    TEST_ASSERT_EQUAL_UINT8(0, n.fido_page());
    TEST_ASSERT_FALSE(n.Tab(1));
    TEST_ASSERT_FALSE(n.Tab(3));
    TEST_ASSERT_EQUAL_UINT8(0, n.page());  // the list's page untouched
    n.Apply(Input::kLeft);
    TEST_ASSERT_TRUE(n.SetFidoPages(2));  // keys gone: the page stays inside
    TEST_ASSERT_EQUAL_UINT8(1, n.fido_page());
    // Back keeps it, Home starts over.
    n.Back();
    n.Swipe(Gesture::kUpFromBottom);
    TEST_ASSERT_EQUAL_UINT8(1, n.fido_page());
    n.Apply(Input::kHome);
    TEST_ASSERT_EQUAL_UINT8(0, n.fido_page());
}

void test_fido_list_rows(void) {
    namespace fl = ui::fido_list;
    TEST_ASSERT_EQUAL_INT(0, fl::Compare("GitHub.com", "github.com"));
    TEST_ASSERT_EQUAL_INT(-1, fl::Compare("github.com", "google.com"));
    TEST_ASSERT_EQUAL_INT(-1, fl::Compare("a", "ab"));
    TEST_ASSERT_EQUAL_INT(1, fl::Compare("b", "A"));
    const char *sites[5] = {"webauthn.io", "Github.com", "amazon.com", "github.com", "U2F 1a2b3c4d"};
    uint16_t order[5] = {};
    fl::Sort(order, 5, [&sites](uint16_t a, uint16_t b) { return fl::Compare(sites[a], sites[b]) < 0; });
    const uint16_t want[5] = {2, 1, 3, 4, 0};  // equal sites keep the store's order
    TEST_ASSERT_EQUAL_UINT16_ARRAY(want, order, 5);
    fl::Sort(order, 0, [](uint16_t, uint16_t) { return false; });

    char text[64];
    fl::Details(2026, 10, 4, -7, false, true, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("made 2026-10-04, ES256, resident", text);
    fl::Details(0, 0, 0, -8, false, false, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("EdDSA, non-resident", text);
    fl::Details(2025, 1, 31, -7, true, false, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("made 2025-01-31, ES256, U2F", text);
    fl::User("alice", "Alice Smith", text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("alice (Alice Smith)", text);
    fl::User("alice", "Alice", text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("alice", text);
    fl::User("", "Alice", text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("Alice", text);
    fl::User("", "", text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("-", text);
}

void test_nav_classify(void) {
    using ui::nav::Classify;
    using ui::nav::Gesture;
    TEST_ASSERT_TRUE(Classify(100, 400, 110, 390, 800) == Gesture::kTap);
    TEST_ASSERT_TRUE(Classify(100, 400, 160, 400, 800) == Gesture::kNone);  // sideways
    TEST_ASSERT_TRUE(Classify(200, 60, 210, 400, 800) == Gesture::kDownFromTop);
    TEST_ASSERT_TRUE(Classify(200, 199, 200, 400, 800) == Gesture::kDownFromTop);
    TEST_ASSERT_TRUE(Classify(200, 200, 200, 400, 800) == Gesture::kDown);
    TEST_ASSERT_TRUE(Classify(200, 760, 200, 300, 800) == Gesture::kUpFromBottom);
    TEST_ASSERT_TRUE(Classify(200, 500, 200, 300, 800) == Gesture::kUp);
    TEST_ASSERT_TRUE(Classify(200, 60, 200, 140, 800) == Gesture::kNone);  // too short
    TEST_ASSERT_TRUE(Classify(100, 60, 300, 200, 800) == Gesture::kNone);  // too slanted
    TEST_ASSERT_TRUE(Classify(400, 300, 150, 340, 800) == Gesture::kLeft);
    TEST_ASSERT_TRUE(Classify(100, 300, 220, 300, 800) == Gesture::kRight);
    TEST_ASSERT_TRUE(Classify(400, 300, 250, 380, 800) == Gesture::kNone);  // too slanted
}

void test_fido_remove(void) {
    namespace fl = ui::fido_list;
    using ui::nav::Gesture;
    TEST_ASSERT_EQUAL_UINT8(fl::kNoRow, fl::RowAt(50, 56, 103, 6));
    TEST_ASSERT_EQUAL_UINT8(0, fl::RowAt(56, 56, 103, 6));
    TEST_ASSERT_EQUAL_UINT8(2, fl::RowAt(56 + 2 * 103 + 5, 56, 103, 6));
    TEST_ASSERT_EQUAL_UINT8(fl::kNoRow, fl::RowAt(56 + 4 * 103, 56, 103, 4));  // under the last

    fl::Remove r;
    TEST_ASSERT_FALSE(r.Trash());  // no can
    TEST_ASSERT_FALSE(r.Swipe(Gesture::kLeft, fl::kNoRow));
    TEST_ASSERT_FALSE(r.Swipe(Gesture::kUp, 1));
    TEST_ASSERT_TRUE(r.Swipe(Gesture::kLeft, 1));
    TEST_ASSERT_TRUE(r.stage() == fl::Stage::kTrash);
    TEST_ASSERT_EQUAL_UINT8(1, r.row());
    TEST_ASSERT_FALSE(r.Swipe(Gesture::kLeft, 1));  // already there
    TEST_ASSERT_TRUE(r.Swipe(Gesture::kLeft, 3));   // one can: moved
    TEST_ASSERT_TRUE(r.Swipe(Gesture::kRight, fl::kNoRow));
    TEST_ASSERT_TRUE(r.stage() == fl::Stage::kNone);
    TEST_ASSERT_FALSE(r.Swipe(Gesture::kRight, 2));
    r.Swipe(Gesture::kLeft, 2);
    TEST_ASSERT_TRUE(r.Drop());
    TEST_ASSERT_FALSE(r.Drop());

    // Asked: swipes do nothing, No at once.
    r.Swipe(Gesture::kLeft, 2);
    TEST_ASSERT_TRUE(r.Trash());
    TEST_ASSERT_TRUE(r.asking());
    TEST_ASSERT_FALSE(r.Swipe(Gesture::kRight, 2));
    TEST_ASSERT_FALSE(r.Drop());
    TEST_ASSERT_EQUAL_UINT8(2, r.row());
    TEST_ASSERT_TRUE(r.Clear());
    TEST_ASSERT_FALSE(r.asking());
    TEST_ASSERT_EQUAL_UINT8(fl::kNoRow, r.row());
    TEST_ASSERT_FALSE(r.Clear());  // nothing left
}

void test_dialog_hold(void) {
    namespace dl = ui::dialog;
    dl::Hold h;
    TEST_ASSERT_EQUAL_UINT8(0, h.Fill(5000));  // not pressed
    h.Press(1000);
    TEST_ASSERT_EQUAL_UINT8(0, h.Fill(1000));
    TEST_ASSERT_EQUAL_UINT8(1, h.Fill(1500));
    TEST_ASSERT_EQUAL_UINT8(7, h.Fill(4999));
    h.Release();  // let go early: empty
    TEST_ASSERT_EQUAL_UINT8(0, h.Fill(4999));
    TEST_ASSERT_FALSE(h.done());
    h.Press(10000);
    TEST_ASSERT_EQUAL_UINT8(dl::kHoldSteps, h.Fill(10000 + dl::kHoldMs));
    TEST_ASSERT_TRUE(h.done());
    h.Release();
    TEST_ASSERT_EQUAL_UINT8(dl::kHoldSteps, h.Fill(20000));  // stays done
    h.Reset();
    TEST_ASSERT_FALSE(h.done());
    h.Press(0xFFFFFE00u);  // the tick wraps: 768 ms held
    TEST_ASSERT_EQUAL_UINT8(1, h.Fill(0x100u));
}

void test_settings_pages(void) {
    namespace st = ui::settings;
    using ui::nav::Gesture;
    using ui::nav::Mode;
    ui::nav::Nav n;
    TEST_ASSERT_FALSE(n.OpenSettings(2));  // not in Settings
    n.Swipe(Gesture::kDownFromTop);
    TEST_ASSERT_TRUE(n.OpenSettings(static_cast<uint8_t>(st::Page::kWifi)));
    TEST_ASSERT_FALSE(n.OpenSettings(static_cast<uint8_t>(st::Page::kWifi)));
    TEST_ASSERT_TRUE(n.Back());  // the page: back to Settings
    TEST_ASSERT_TRUE(n.mode() == Mode::kSettings);
    TEST_ASSERT_EQUAL_UINT8(0, n.settings_page());
    TEST_ASSERT_TRUE(n.Back());
    TEST_ASSERT_TRUE(n.mode() == Mode::kList);
    n.Swipe(Gesture::kDownFromTop);
    n.OpenSettings(3);
    TEST_ASSERT_TRUE(n.Swipe(Gesture::kUp));  // closes it all
    n.Swipe(Gesture::kDownFromTop);
    TEST_ASSERT_EQUAL_UINT8(0, n.settings_page());  // opens at the main page
    n.OpenSettings(3);
    TEST_ASSERT_TRUE(n.Apply(ui::nav::Input::kHome));
    TEST_ASSERT_EQUAL_UINT8(0, n.settings_page());
    n.Open(Mode::kSettings);
    n.OpenSettings(4);
    TEST_ASSERT_TRUE(n.Open(Mode::kSettings));  // the console: the main page
    TEST_ASSERT_EQUAL_UINT8(0, n.settings_page());

    TEST_ASSERT_EQUAL_STRING("Wi-Fi", st::Title(st::kRows[0]));
    TEST_ASSERT_EQUAL_UINT8(8, st::kRowCount);
    TEST_ASSERT_TRUE(st::PageAction(st::Page::kSecurity) == st::Action::kLockNow);
    TEST_ASSERT_TRUE(st::PageAction(st::Page::kWifi) == st::Action::kNone);
    TEST_ASSERT_TRUE(st::PageAction(st::Page::kAbout) == st::Action::kRestart);
    TEST_ASSERT_EQUAL_STRING("Restart", st::ActionText(st::Action::kRestart));
    TEST_ASSERT_TRUE(st::PageAction(st::Page::kFido) == st::Action::kFidoReset);
    TEST_ASSERT_TRUE(st::Parent(st::Page::kFido) == st::Page::kSecurity);
    TEST_ASSERT_EQUAL_STRING("Lock now", st::ActionText(st::Action::kLockNow));

    char label[8];
    char value[8];
    const char *text = "State\tonline\nIP\t192.168.1.20\nplain line\n";
    TEST_ASSERT_TRUE(st::Line(text, 0, label, sizeof label, value, sizeof value));
    TEST_ASSERT_EQUAL_STRING("State", label);
    TEST_ASSERT_EQUAL_STRING("online", value);
    TEST_ASSERT_TRUE(st::Line(text, 1, label, sizeof label, value, sizeof value));
    TEST_ASSERT_EQUAL_STRING("IP", label);
    TEST_ASSERT_EQUAL_STRING("192.168", value);  // cut
    TEST_ASSERT_TRUE(st::Line(text, 2, label, sizeof label, value, sizeof value));
    TEST_ASSERT_EQUAL_STRING("plain l", label);
    TEST_ASSERT_EQUAL_STRING("", value);
    TEST_ASSERT_FALSE(st::Line(text, 3, label, sizeof label, value, sizeof value));
    TEST_ASSERT_FALSE(st::Line("", 0, label, sizeof label, value, sizeof value));
    TEST_ASSERT_TRUE(st::Line("a\tb", 0, label, sizeof label, value, sizeof value));
    TEST_ASSERT_EQUAL_STRING("b", value);

    TEST_ASSERT_EQUAL_UINT8(1, st::Brightness(0));
    TEST_ASSERT_EQUAL_UINT8(100, st::Brightness(130));
    TEST_ASSERT_EQUAL_UINT8(0, st::Warmth(-5));
    TEST_ASSERT_EQUAL_UINT8(55, st::Warmth(55));
}

void test_wifi_view(void) {
    namespace wv = ui::wifi_view;
    namespace st = ui::settings;
    ui::nav::Nav n;
    n.Swipe(ui::nav::Gesture::kDownFromTop);
    n.OpenSettings(static_cast<uint8_t>(st::Page::kWifi));
    n.OpenSettings(static_cast<uint8_t>(st::Page::kWifiScan));
    n.OpenSettings(static_cast<uint8_t>(st::Page::kWifiPassword));
    TEST_ASSERT_TRUE(n.Back());  // the password: the scan
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(st::Page::kWifiScan), n.settings_page());
    n.Back();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(st::Page::kWifi), n.settings_page());
    n.Back();
    TEST_ASSERT_EQUAL_UINT8(0, n.settings_page());

    wv::Nearby list[6] = {{"cafe", -80, true}, {"", -40, true},    {"home", -60, true},
                          {"cafe", -50, true}, {"open", -70, false}, {"far", -90, true}};
    TEST_ASSERT_EQUAL_UINT(4, wv::Tidy(list, 6, 9));
    TEST_ASSERT_EQUAL_STRING("cafe", list[0].ssid);  // its strongest
    TEST_ASSERT_EQUAL_INT8(-50, list[0].rssi);
    TEST_ASSERT_EQUAL_STRING("home", list[1].ssid);
    TEST_ASSERT_EQUAL_STRING("open", list[2].ssid);
    TEST_ASSERT_EQUAL_STRING("far", list[3].ssid);
    TEST_ASSERT_EQUAL_UINT(2, wv::Tidy(list, 4, 2));

    TEST_ASSERT_EQUAL_UINT8(4, wv::Bars(-40));
    TEST_ASSERT_EQUAL_UINT8(2, wv::Bars(-70));
    TEST_ASSERT_EQUAL_UINT8(0, wv::Bars(-95));

    wv::View v = {};
    v.saved_count = 1;
    TEST_ASSERT_TRUE(v.saved[0].ssid[0] == 0);
    v.saved[0] = {"home", false};
    v.joined = wv::kNone;
    const wv::Nearby home = {"home", -60, true};
    const wv::Nearby cafe = {"cafe", -60, true};
    const wv::Nearby open = {"open", -60, false};
    TEST_ASSERT_TRUE(wv::OnTap(v, home) == wv::Join::kSaved);
    TEST_ASSERT_TRUE(wv::OnTap(v, cafe) == wv::Join::kPassword);
    TEST_ASSERT_TRUE(wv::OnTap(v, open) == wv::Join::kOpen);
    TEST_ASSERT_EQUAL_INT8(0, wv::SavedIndex(v, "home"));
    TEST_ASSERT_EQUAL_INT8(wv::kNone, wv::SavedIndex(v, "hom"));

    wv::View w = v;
    TEST_ASSERT_TRUE(wv::Equal(v, w));
    w.saved[0].refused = true;
    TEST_ASSERT_FALSE(wv::Equal(v, w));
    w = v;
    w.joined = 0;
    TEST_ASSERT_FALSE(wv::Equal(v, w));

    TEST_ASSERT_FALSE(wv::PasswordOk("1234567"));
    TEST_ASSERT_TRUE(wv::PasswordOk("12345678"));
    TEST_ASSERT_TRUE(wv::PasswordOk("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    TEST_ASSERT_FALSE(wv::PasswordOk("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg"));
    TEST_ASSERT_FALSE(wv::PasswordOk(""));
}

void test_touch_calibration(void) {
    namespace tc = ui::touch_cal;
    tc::Calibration c;
    TEST_ASSERT_TRUE(tc::IsDefault(c));
    ui::ScreenPoint p = tc::Apply(c, {100, 700});
    TEST_ASSERT_EQUAL_UINT16(100, p.x);
    TEST_ASSERT_EQUAL_UINT16(700, p.y);
    c = {1100, -20, 1000, 15};
    p = tc::Apply(c, {100, 700});
    TEST_ASSERT_EQUAL_UINT16(90, p.x);
    TEST_ASSERT_EQUAL_UINT16(715, p.y);
    p = tc::Apply(c, {479, 799});  // clamped
    TEST_ASSERT_EQUAL_UINT16(479, p.x);
    TEST_ASSERT_EQUAL_UINT16(799, p.y);
    p = tc::Apply(c, {5, 0});
    TEST_ASSERT_EQUAL_UINT16(0, p.x);

    // Taps right on the crosses: the identity.
    ui::ScreenPoint taps[tc::kTargets] = {};
    for (uint8_t i = 0; i < tc::kTargets; ++i) {
        taps[i] = tc::kTarget[i];
    }
    tc::Fit f = tc::Solve(taps);
    TEST_ASSERT_TRUE(f.ok);
    TEST_ASSERT_TRUE(tc::IsDefault(f.calibration));
    TEST_ASSERT_EQUAL_INT32(0, f.worst);

    // A glass read 12 px right and 8 px high: the offsets take it back.
    for (uint8_t i = 0; i < tc::kTargets; ++i) {
        taps[i] = {static_cast<uint16_t>(tc::kTarget[i].x + 12),
                   static_cast<uint16_t>(tc::kTarget[i].y - 8)};
    }
    f = tc::Solve(taps);
    TEST_ASSERT_TRUE(f.ok);
    TEST_ASSERT_EQUAL_INT16(1000, f.calibration.x_scale);
    TEST_ASSERT_EQUAL_INT16(-12, f.calibration.x_offset);
    TEST_ASSERT_EQUAL_INT16(8, f.calibration.y_offset);
    for (uint8_t i = 0; i < tc::kTargets; ++i) {
        const ui::ScreenPoint s = tc::Apply(f.calibration, taps[i]);
        TEST_ASSERT_INT_WITHIN(1, tc::kTarget[i].x, s.x);
        TEST_ASSERT_INT_WITHIN(1, tc::kTarget[i].y, s.y);
    }

    // Stretched by 5 %: the scale.
    for (uint8_t i = 0; i < tc::kTargets; ++i) {
        taps[i] = {static_cast<uint16_t>(tc::kTarget[i].x * 105 / 100), tc::kTarget[i].y};
    }
    f = tc::Solve(taps);
    TEST_ASSERT_TRUE(f.ok);
    TEST_ASSERT_INT_WITHIN(3, 952, f.calibration.x_scale);

    // One tap beside its cross: refused.
    for (uint8_t i = 0; i < tc::kTargets; ++i) {
        taps[i] = tc::kTarget[i];
    }
    taps[2] = {300, 600};
    TEST_ASSERT_FALSE(tc::Solve(taps).ok);
    // All in one place: no line.
    for (uint8_t i = 0; i < tc::kTargets; ++i) {
        taps[i] = {200, 200};
    }
    TEST_ASSERT_FALSE(tc::Solve(taps).ok);
}

void test_ble_view(void) {
    namespace bv = ui::ble_view;
    namespace st = ui::settings;
    ui::nav::Nav n;
    n.Swipe(ui::nav::Gesture::kDownFromTop);
    n.OpenSettings(static_cast<uint8_t>(st::Page::kBluetooth));
    n.OpenSettings(static_cast<uint8_t>(st::Page::kBlePair));
    n.Back();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(st::Page::kBluetooth), n.settings_page());
    n.OpenSettings(static_cast<uint8_t>(st::Page::kTouch));
    n.Back();
    TEST_ASSERT_EQUAL_UINT8(0, n.settings_page());

    char text[16];
    bv::Passkey(42, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("000 042", text);
    bv::Passkey(987654, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("987 654", text);

    bv::PairPage page;
    page.Start();
    TEST_ASSERT_TRUE(page.state() == bv::Pairing::kWaiting);
    TEST_ASSERT_FALSE(page.Left(30));
    TEST_ASSERT_TRUE(page.Passkey(123456));
    TEST_ASSERT_FALSE(page.Passkey(123456));
    TEST_ASSERT_EQUAL_UINT32(123456, page.passkey());
    TEST_ASSERT_TRUE(page.Failed());
    TEST_ASSERT_TRUE(page.Passkey(222222));  // tried again
    TEST_ASSERT_TRUE(page.Paired());
    TEST_ASSERT_FALSE(page.Left(0));  // paired: not "closed"
    page.Start();
    TEST_ASSERT_TRUE(page.Left(0));
    TEST_ASSERT_TRUE(page.state() == bv::Pairing::kClosed);

    TEST_ASSERT_TRUE(st::Parent(st::Page::kBleName) == st::Page::kBluetooth);
    TEST_ASSERT_TRUE(bv::NameOk("ESP32 Auth X4Pro"));
    TEST_ASSERT_TRUE(bv::NameOk("12345678901234567890123456789"));    // 29
    TEST_ASSERT_FALSE(bv::NameOk("123456789012345678901234567890"));  // 30
    TEST_ASSERT_FALSE(bv::NameOk(""));
    TEST_ASSERT_FALSE(bv::NameOk("tab\there"));
    bv::View a = {};
    a.host_count = 1;
    a.hosts[0] = {"aa:bb:cc:dd:ee:ff", true, false};
    bv::View b = a;
    TEST_ASSERT_TRUE(bv::Equal(a, b));
    b.hosts[0].target = true;
    TEST_ASSERT_FALSE(bv::Equal(a, b));
}

void test_display_view(void) {
    namespace dv = ui::display_view;
    namespace st = ui::settings;
    TEST_ASSERT_TRUE(st::Parent(st::Page::kDisplayChoice) == st::Page::kDisplay);
    TEST_ASSERT_EQUAL_INT(3, dv::Index(dv::Setting::kIdle, 10));
    TEST_ASSERT_EQUAL_INT(0, dv::Index(dv::Setting::kKeepPin, 0));
    TEST_ASSERT_EQUAL_INT(-1, dv::Index(dv::Setting::kIdle, 7));
    TEST_ASSERT_EQUAL_INT(1, dv::Index(dv::Setting::kWallpaper, 1));
    char text[24];
    dv::Label(dv::Setting::kIdle, 0, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("Never", text);
    dv::Label(dv::Setting::kIdle, 7, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("7 min", text);  // set on the site
    dv::Label(dv::Setting::kKeepPin, 8, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("After 8 hours", text);
    dv::Label(dv::Setting::kKeepPin, 12, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("After 12 hours", text);
    dv::Label(dv::Setting::kWallpaper, 0, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("Grey", text);
    size_t n = 0;
    TEST_ASSERT_NOT_NULL(dv::Options(dv::Setting::kKeepPin, &n));
    TEST_ASSERT_EQUAL_UINT(7, n);
    TEST_ASSERT_EQUAL_STRING("PIN after sleep", dv::Title(dv::Setting::kKeepPin));
    // Every choice fits config.json's range.
    for (const dv::Option &o : dv::kIdle) {
        TEST_ASSERT_TRUE(o.value <= 1440);
    }
    for (const dv::Option &o : dv::kKeepPin) {
        TEST_ASSERT_TRUE(o.value <= 168);
    }
    dv::View a = {50, false, {2, 0, 10, 0, 8, 0, 1}};
    dv::View b = a;
    TEST_ASSERT_TRUE(dv::Equal(a, b));
    b.value[1] = 4;
    TEST_ASSERT_FALSE(dv::Equal(a, b));
}

void test_time_view(void) {
    namespace tv = ui::time_view;
    namespace st = ui::settings;
    TEST_ASSERT_TRUE(st::Parent(st::Page::kTimeZone) == st::Page::kTimeRegion);
    TEST_ASSERT_TRUE(st::Parent(st::Page::kTimeRegion) == st::Page::kTime);
    TEST_ASSERT_TRUE(st::Parent(st::Page::kTimeSet) == st::Page::kTime);

    TEST_ASSERT_EQUAL_UINT8(0, tv::RegionOf("Europe/Kyiv"));
    TEST_ASSERT_EQUAL_UINT8(1, tv::RegionOf("Asia/Nicosia"));
    TEST_ASSERT_EQUAL_UINT8(2, tv::RegionOf("America/Argentina/Buenos_Aires"));
    TEST_ASSERT_EQUAL_UINT8(tv::kOther, tv::RegionOf("UTC"));
    TEST_ASSERT_EQUAL_UINT8(tv::kOther, tv::RegionOf("Europ"));
    TEST_ASSERT_EQUAL_UINT8(tv::kOther, tv::RegionOf("Etc/UTC"));
    char city[24];
    tv::City("America/Argentina/Buenos_Aires", city, sizeof city);
    TEST_ASSERT_EQUAL_STRING("Buenos Aires", city);
    tv::City("CET", city, sizeof city);
    TEST_ASSERT_EQUAL_STRING("CET", city);

    char text[24];
    tv::SyncLabel(6, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("Every 6 hours", text);
    tv::SyncLabel(3, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("Every 3 hours", text);
    TEST_ASSERT_EQUAL_INT(0, tv::SyncIndex(0));
    TEST_ASSERT_EQUAL_INT(-1, tv::SyncIndex(3));

    TEST_ASSERT_EQUAL_UINT8(29, tv::DaysIn(2028, 2));
    TEST_ASSERT_EQUAL_UINT8(28, tv::DaysIn(2026, 2));
    TEST_ASSERT_EQUAL_UINT8(31, tv::DaysIn(2026, 12));
    tv::Manual m = {{2026, 1, 31, 23, 59}};
    tv::Step(m, tv::kMinute, 1);
    TEST_ASSERT_EQUAL_UINT16(0, m.value[tv::kMinute]);  // round, the hour stays
    TEST_ASSERT_EQUAL_UINT16(23, m.value[tv::kHour]);
    tv::Step(m, tv::kHour, 1);
    TEST_ASSERT_EQUAL_UINT16(0, m.value[tv::kHour]);
    tv::Step(m, tv::kMonth, 1);  // 31 January -> February: the 28th
    TEST_ASSERT_EQUAL_UINT16(2, m.value[tv::kMonth]);
    TEST_ASSERT_EQUAL_UINT16(28, m.value[tv::kDay]);
    tv::Step(m, tv::kDay, 1);
    TEST_ASSERT_EQUAL_UINT16(1, m.value[tv::kDay]);
    tv::Step(m, tv::kDay, -1);
    TEST_ASSERT_EQUAL_UINT16(28, m.value[tv::kDay]);
    tv::Step(m, tv::kMonth, -2);
    TEST_ASSERT_EQUAL_UINT16(12, m.value[tv::kMonth]);
    tv::Step(m, tv::kYear, -3);
    TEST_ASSERT_EQUAL_UINT16(2099, m.value[tv::kYear]);  // 2026 - 3 past 2024: round
    tv::Step(m, tv::kYear, 1);
    TEST_ASSERT_EQUAL_UINT16(2024, m.value[tv::kYear]);
    m = {{2028, 2, 29, 0, 0}};
    tv::Step(m, tv::kYear, 1);  // no 29 February in 2029
    TEST_ASSERT_EQUAL_UINT16(28, m.value[tv::kDay]);

    tv::View a = {};
    tv::View b = a;
    b.now[tv::kMinute] = 5;
    TEST_ASSERT_TRUE(tv::Equal(a, b));
    b.sync_hours = 6;
    TEST_ASSERT_FALSE(tv::Equal(a, b));
}

void test_security_view(void) {
    namespace sv = ui::security_view;
    namespace st = ui::settings;
    using ui::pinpad::Key;
    TEST_ASSERT_TRUE(st::Parent(st::Page::kSecurityPin) == st::Page::kSecurity);
    TEST_ASSERT_EQUAL_INT(2, sv::LockIndex(10));
    TEST_ASSERT_EQUAL_INT(-1, sv::LockIndex(7));
    for (const sv::Option &o : sv::kLock) {
        TEST_ASSERT_TRUE(o.value >= 1 && o.value <= 1440);  // config.json's range
    }

    auto type = [](sv::PinFlow &f, const char *digits) {
        sv::Result r = sv::Result::kNone;
        for (const char *d = digits; *d != 0; ++d) {
            r = f.Apply({Key::kDigit, *d});
        }
        return r;
    };
    sv::PinFlow f;
    f.Start(sv::Flow::kChangePin);
    TEST_ASSERT_EQUAL_STRING("The current PIN", f.Title());
    TEST_ASSERT_TRUE(type(f, "12345") == sv::Result::kTyped);
    TEST_ASSERT_TRUE(f.Apply({Key::kDigit, '6'}) == sv::Result::kNext);
    TEST_ASSERT_EQUAL_STRING("A new PIN", f.Title());
    TEST_ASSERT_EQUAL_UINT8(0, f.count());
    TEST_ASSERT_TRUE(type(f, "246813") == sv::Result::kNext);
    TEST_ASSERT_EQUAL_STRING("Repeat the new PIN", f.Title());
    TEST_ASSERT_TRUE(type(f, "246810") == sv::Result::kMismatch);
    TEST_ASSERT_EQUAL_STRING("A new PIN", f.Title());  // the current one is kept
    type(f, "246813");
    TEST_ASSERT_TRUE(type(f, "246813") == sv::Result::kSubmit);
    TEST_ASSERT_EQUAL_STRING("123456", f.old());
    TEST_ASSERT_EQUAL_STRING("246813", f.fresh());
    f.Wipe();
    TEST_ASSERT_EQUAL_STRING("", f.old());
    TEST_ASSERT_EQUAL_STRING("", f.fresh());

    f.Start(sv::Flow::kSetWipe);
    TEST_ASSERT_EQUAL_STRING("The wipe PIN", f.Title());
    TEST_ASSERT_TRUE(type(f, "975310") == sv::Result::kNext);
    TEST_ASSERT_EQUAL_STRING("Repeat the wipe PIN", f.Title());
    TEST_ASSERT_TRUE(f.Apply({Key::kBack, 0}) == sv::Result::kNone);  // nothing to take back
    TEST_ASSERT_TRUE(type(f, "975310") == sv::Result::kSubmit);
    TEST_ASSERT_EQUAL_STRING("975310", f.fresh());

    sv::View a = {};
    sv::View b = a;
    TEST_ASSERT_TRUE(sv::Equal(a, b));
    b.wipe = true;
    TEST_ASSERT_FALSE(sv::Equal(a, b));

    namespace tv = ui::time_view;
    TEST_ASSERT_TRUE(tv::CityLess("Europe/Amsterdam", "Europe/Berlin"));
    TEST_ASSERT_FALSE(tv::CityLess("Europe/London", "Europe/Dublin"));
    TEST_ASSERT_TRUE(tv::CityLess("America/Argentina/Buenos_Aires", "America/Chicago"));
    TEST_ASSERT_FALSE(tv::CityLess("Asia/Baku", "Asia/Baku"));
}

void test_web_view(void) {
    namespace wv = ui::web_view;
    namespace st = ui::settings;
    TEST_ASSERT_TRUE(st::Parent(st::Page::kWebLogin) == st::Page::kWeb);
    TEST_ASSERT_TRUE(st::Parent(st::Page::kWeb) == st::Page::kMain);
    TEST_ASSERT_EQUAL_STRING("Web site", st::Title(st::kRows[2]));
    TEST_ASSERT_TRUE(wv::UserOk("admin"));
    TEST_ASSERT_FALSE(wv::UserOk(""));
    TEST_ASSERT_FALSE(wv::UserOk("a:b"));
    TEST_ASSERT_FALSE(wv::UserOk("123456789012345678901234567890123"));  // 33
    TEST_ASSERT_TRUE(wv::UserOk("12345678901234567890123456789012"));    // 32
    TEST_ASSERT_TRUE(wv::PasswordOk("x"));
    TEST_ASSERT_FALSE(wv::PasswordOk(""));
    TEST_ASSERT_EQUAL_STRING("Its access point", wv::ModeLabel(wv::kModeAuto));
    TEST_ASSERT_EQUAL_UINT8(3, wv::kModeCount);
    wv::View a = {};
    wv::View b = a;
    TEST_ASSERT_TRUE(wv::Equal(a, b));
    b.write = true;
    TEST_ASSERT_FALSE(wv::Equal(a, b));
}

// CBOR bits for the prompt's tests.
void Put(std::vector<uint8_t> &v, std::initializer_list<uint8_t> bytes) { v.insert(v.end(), bytes); }
void PutText(std::vector<uint8_t> &v, const char *s) {
    const size_t n = strlen(s);
    v.push_back(static_cast<uint8_t>(0x60 | n));  // short texts only
    v.insert(v.end(), s, s + n);
}

void test_fido_prompt(void) {
    namespace fp = ui::fido_prompt;
    // makeCredential: hash, rp, user, rk.
    std::vector<uint8_t> m = {0x01, 0xa4, 0x01, 0x58, 0x20};
    m.insert(m.end(), 32, 0xab);
    Put(m, {0x02, 0xa2});
    PutText(m, "id");
    PutText(m, "example.com");
    PutText(m, "name");
    PutText(m, "Example");
    Put(m, {0x03, 0xa3});
    PutText(m, "id");
    Put(m, {0x41, 0x01});
    PutText(m, "name");
    PutText(m, "alice");
    PutText(m, "displayName");
    PutText(m, "Alice A");
    Put(m, {0x07, 0xa1});
    PutText(m, "rk");
    Put(m, {0xf5});
    fp::Request r = fp::Parse(m.data(), m.size());
    TEST_ASSERT_TRUE(r.kind == fp::Kind::kRegister);
    TEST_ASSERT_EQUAL_STRING("example.com", r.site);
    TEST_ASSERT_EQUAL_STRING("alice", r.user);
    TEST_ASSERT_EQUAL_STRING("Alice A", r.display);
    TEST_ASSERT_TRUE(r.resident);
    TEST_ASSERT_FALSE(r.verify);
    TEST_ASSERT_EQUAL_STRING("Create a key", fp::Title(r.kind));

    // getAssertion: rpId, hash, allowList of two, pinUvAuthParam.
    std::vector<uint8_t> g = {0x02, 0xa4, 0x01};
    PutText(g, "webauthn.io");
    Put(g, {0x02, 0x58, 0x20});
    g.insert(g.end(), 32, 0x11);
    Put(g, {0x03, 0x82});
    for (int i = 0; i < 2; ++i) {
        Put(g, {0xa2});
        PutText(g, "type");
        PutText(g, "public-key");
        PutText(g, "id");
        Put(g, {0x42, 0x00, static_cast<uint8_t>(i)});
    }
    Put(g, {0x06, 0x50});
    g.insert(g.end(), 16, 0x22);
    r = fp::Parse(g.data(), g.size());
    TEST_ASSERT_TRUE(r.kind == fp::Kind::kSignIn);
    TEST_ASSERT_EQUAL_STRING("webauthn.io", r.site);
    TEST_ASSERT_EQUAL_UINT8(2, r.allow);
    TEST_ASSERT_EQUAL_UINT8(0, r.ids);  // not of the store's size
    TEST_ASSERT_TRUE(r.verify);

    // An allowList of the store's IDs: only the listed keys match.
    std::vector<uint8_t> a = {0x02, 0xa2, 0x01};
    PutText(a, "webauthn.io");
    Put(a, {0x03, 0x82});
    for (uint8_t i = 1; i <= 2; ++i) {
        Put(a, {0xa2});
        PutText(a, "id");
        Put(a, {0x58, 0x20});
        a.insert(a.end(), 32, i);
        PutText(a, "type");
        PutText(a, "public-key");
    }
    r = fp::Parse(a.data(), a.size());
    TEST_ASSERT_EQUAL_UINT8(2, r.allow);
    TEST_ASSERT_EQUAL_UINT8(2, r.ids);
    uint8_t id[32];
    memset(id, 2, sizeof id);
    TEST_ASSERT_TRUE(fp::Matches(r, "webauthn.io", id));
    memset(id, 3, sizeof id);
    TEST_ASSERT_FALSE(fp::Matches(r, "webauthn.io", id));
    r.allow = 0;  // no allowList: the site's keys
    TEST_ASSERT_TRUE(fp::Matches(r, "webauthn.io", id));
    TEST_ASSERT_FALSE(fp::Matches(r, "webauthn.iox", id));
    TEST_ASSERT_FALSE(fp::Matches(r, "webauthn", id));
    r.allow = fp::kIds + 1;  // a list not read whole: the site's keys
    TEST_ASSERT_TRUE(fp::Matches(r, "webauthn.io", id));

    // The accounts: one named in full, several listed, "+N more", unnamed counted.
    char text[fp::kNames * fp::kTextSize + 32];
    fp::Request s = {};
    TEST_ASSERT_EQUAL_INT(0, fp::AccountText(s, text, sizeof text));
    TEST_ASSERT_EQUAL_STRING("", text);
    fp::AddAccount(&s, "alice", "Alice A");
    TEST_ASSERT_EQUAL_INT(2, fp::AccountText(s, text, sizeof text));
    TEST_ASSERT_EQUAL_STRING("alice\nAlice A", text);
    fp::AddAccount(&s, "", "Bob B");
    TEST_ASSERT_EQUAL_INT(2, fp::AccountText(s, text, sizeof text));
    TEST_ASSERT_EQUAL_STRING("alice\nBob B", text);
    fp::AddAccount(&s, "", "");  // unnamed: counted only
    fp::AddAccount(&s, "carol", "");
    fp::AddAccount(&s, "dave", "");
    fp::AddAccount(&s, "erin", "");
    TEST_ASSERT_EQUAL_UINT8(6, s.accounts);
    TEST_ASSERT_EQUAL_INT(5, fp::AccountText(s, text, sizeof text));
    TEST_ASSERT_EQUAL_STRING("alice\nBob B\ncarol\ndave\n+2 more", text);
    TEST_ASSERT_EQUAL_INT(5, fp::AccountText(s, text, 12));  // cut at the size
    TEST_ASSERT_EQUAL_STRING("alice\nBob B", text);
    fp::Request none = {};
    fp::AddAccount(&none, "", "");
    fp::AddAccount(&none, "", "");
    TEST_ASSERT_EQUAL_INT(1, fp::AccountText(none, text, sizeof text));
    TEST_ASSERT_EQUAL_STRING("one of 2 accounts", text);
    for (int i = 0; i < 10; ++i) {
        fp::AddAccount(&s, "x", "");
    }
    fp::AccountText(s, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("alice\nBob B\ncarol\ndave\n+12 more", text);

    // U2F_AUTHENTICATE, a short APDU: the application's first bytes.
    std::vector<uint8_t> u = {0x00, 0x02, 0x03, 0x00, 65 + 4};
    u.insert(u.end(), 32, 0x00);  // challenge
    Put(u, {0x3d, 0xec, 0x69, 0xac});
    u.insert(u.end(), 28, 0x55);
    Put(u, {0x04, 1, 2, 3, 4});
    r = fp::Parse(u.data(), u.size());
    TEST_ASSERT_TRUE(r.kind == fp::Kind::kU2fSignIn);
    TEST_ASSERT_EQUAL_STRING("U2F 3dec69ac", r.site);
    TEST_ASSERT_EQUAL_UINT8(0, r.ids);  // a short key handle: not the store's
    std::vector<uint8_t> h = {0x00, 0x02, 0x03, 0x00, 65 + 32};
    h.insert(h.end(), 64, 0x00);
    h.push_back(32);
    h.insert(h.end(), 32, 0x77);
    r = fp::Parse(h.data(), h.size());
    TEST_ASSERT_EQUAL_UINT8(1, r.ids);
    TEST_ASSERT_EQUAL_UINT8(0x77, r.id[0][31]);
    u[4] = 0;  // extended: 0, Lc Lc
    u.insert(u.begin() + 5, {0x00, 69});
    r = fp::Parse(u.data(), u.size());
    TEST_ASSERT_EQUAL_STRING("U2F 3dec69ac", r.site);

    // The rest: reset, selection, unknown, cut short.
    const uint8_t reset[] = {0x07};
    TEST_ASSERT_TRUE(fp::Parse(reset, 1).kind == fp::Kind::kReset);
    const uint8_t select[] = {0x0b};
    TEST_ASSERT_TRUE(fp::Parse(select, 1).kind == fp::Kind::kSelect);
    const uint8_t info[] = {0x04};
    TEST_ASSERT_TRUE(fp::Parse(info, 1).kind == fp::Kind::kOther);
    r = fp::Parse(m.data(), 40);  // cut inside the rp
    TEST_ASSERT_TRUE(r.kind == fp::Kind::kRegister);
    TEST_ASSERT_EQUAL_STRING("", r.user);
    const uint8_t indefinite[] = {0x02, 0xbf, 0x01, 0x61, 'a', 0xff};
    r = fp::Parse(indefinite, sizeof indefinite);  // not read, no fault
    TEST_ASSERT_EQUAL_STRING("", r.site);
    TEST_ASSERT_EQUAL_STRING("A FIDO2 request", fp::Title(fp::Kind::kOther));
}

void test_wifi_settings(void) {
    namespace wv = ui::wifi_view;
    namespace st = ui::settings;
    TEST_ASSERT_TRUE(st::Parent(st::Page::kWifiText) == st::Page::kWifiMore);
    TEST_ASSERT_TRUE(st::Parent(st::Page::kWifiStaticText) == st::Page::kWifiStatic);
    TEST_ASSERT_TRUE(st::Parent(st::Page::kWifiStatic) == st::Page::kWifi);
    TEST_ASSERT_TRUE(st::Parent(st::Page::kWifiHidden) == st::Page::kWifiScan);
    TEST_ASSERT_EQUAL_INT(2, wv::ChoiceIndex(wv::Choice::kWindow, 120));
    TEST_ASSERT_EQUAL_INT(5, wv::ChoiceIndex(wv::Choice::kChannel, 6));
    TEST_ASSERT_EQUAL_INT(-1, wv::ChoiceIndex(wv::Choice::kRounds, 4));
    TEST_ASSERT_EQUAL_STRING("Both", wv::ModeLabel(wv::kModeBoth));
    // Every choice fits config.json's ranges.
    size_t n = 0;
    const wv::Option *o = wv::Options(wv::Choice::kRounds, &n);
    for (size_t i = 0; i < n; ++i) {
        TEST_ASSERT_TRUE(o[i].value >= 1 && o[i].value <= 10);
    }
    o = wv::Options(wv::Choice::kWindow, &n);
    for (size_t i = 0; i < n; ++i) {
        TEST_ASSERT_TRUE(o[i].value >= 30 && o[i].value <= 3600);
    }

    TEST_ASSERT_TRUE(wv::Ipv4Ok("192.168.1.50"));
    TEST_ASSERT_TRUE(wv::Ipv4Ok("0.0.0.0"));
    TEST_ASSERT_FALSE(wv::Ipv4Ok("192.168.1"));
    TEST_ASSERT_FALSE(wv::Ipv4Ok("192.168.1.256"));
    TEST_ASSERT_FALSE(wv::Ipv4Ok("192.168.01.1"));  // a leading zero
    TEST_ASSERT_FALSE(wv::Ipv4Ok("1.2.3.4.5"));
    TEST_ASSERT_FALSE(wv::Ipv4Ok("1.2.3.4 "));
    TEST_ASSERT_FALSE(wv::Ipv4Ok(""));
    TEST_ASSERT_TRUE(wv::HostnameOk("esp32-auth"));
    TEST_ASSERT_FALSE(wv::HostnameOk("-esp"));
    TEST_ASSERT_FALSE(wv::HostnameOk("esp_32"));
    TEST_ASSERT_NULL(wv::Check(wv::Field::kApPassword, ""));  // open
    TEST_ASSERT_NOT_NULL(wv::Check(wv::Field::kApPassword, "short"));
    TEST_ASSERT_NULL(wv::Check(wv::Field::kHiddenPassword, ""));
    TEST_ASSERT_NULL(wv::Check(wv::Field::kDns, ""));
    TEST_ASSERT_NOT_NULL(wv::Check(wv::Field::kGateway, ""));
    TEST_ASSERT_NOT_NULL(wv::Check(wv::Field::kHiddenSsid, ""));
    TEST_ASSERT_TRUE(wv::Spec(wv::Field::kAddress).numeric);
    TEST_ASSERT_TRUE(wv::Spec(wv::Field::kApPassword).masked);

    wv::StaticIp ip = {};
    TEST_ASSERT_NULL(wv::CheckStatic(ip));  // DHCP
    ip.enabled = true;
    TEST_ASSERT_EQUAL_STRING("the IP address is missing", wv::CheckStatic(ip));
    strcpy(wv::StaticField(ip, wv::Field::kAddress), "192.168.1.50");
    strcpy(wv::StaticField(ip, wv::Field::kNetmask), "255.255.255.0");
    TEST_ASSERT_EQUAL_STRING("the gateway is missing", wv::CheckStatic(ip));
    strcpy(wv::StaticField(ip, wv::Field::kGateway), "192.168.1.1");
    TEST_ASSERT_NULL(wv::CheckStatic(ip));
    TEST_ASSERT_NULL(wv::StaticField(ip, wv::Field::kHostname));

    namespace dv = ui::display_view;
    char text[24];
    TEST_ASSERT_EQUAL_UINT8(7, dv::kSettingCount);
    dv::Label(dv::Setting::kLightOff, 2, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("2 min", text);
    dv::Label(dv::Setting::kIdleUsb, 0, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("Never", text);
    dv::Label(dv::Setting::kHoldRail, 1, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("Kept", text);
    TEST_ASSERT_EQUAL_STRING("Power in sleep", dv::Title(dv::Setting::kHoldRail));
}

void test_list_rows(void) {
    using ui::list::Zone;
    TEST_ASSERT_TRUE(ui::list::ZoneAt(0, 472) == Zone::kTypeUser);
    TEST_ASSERT_TRUE(ui::list::ZoneAt(117, 472) == Zone::kTypeUser);
    TEST_ASSERT_TRUE(ui::list::ZoneAt(118, 472) == Zone::kTypePassword);
    TEST_ASSERT_TRUE(ui::list::ZoneAt(236, 472) == Zone::kShowPassword);
    TEST_ASSERT_TRUE(ui::list::ZoneAt(400, 472) == Zone::kTypeTotp);
    TEST_ASSERT_TRUE(ui::list::ZoneAt(500, 472) == Zone::kTypeTotp);
    TEST_ASSERT_TRUE(ui::list::ZoneAt(-3, 472) == Zone::kTypeUser);
    using ui::list::Tap;
    TEST_ASSERT_TRUE(ui::list::OnTap(false, Zone::kTypeUser) == Tap::kType);
    TEST_ASSERT_TRUE(ui::list::OnTap(false, Zone::kTypeTotp) == Tap::kType);
    TEST_ASSERT_TRUE(ui::list::OnTap(false, Zone::kShowPassword) == Tap::kShow);
    // Shown: anywhere hides it, nothing typed.
    TEST_ASSERT_TRUE(ui::list::OnTap(true, Zone::kTypeUser) == Tap::kHide);
    TEST_ASSERT_TRUE(ui::list::OnTap(true, Zone::kTypePassword) == Tap::kHide);
    TEST_ASSERT_TRUE(ui::list::OnTap(true, Zone::kShowPassword) == Tap::kHide);
    TEST_ASSERT_TRUE(ui::list::OnTap(true, Zone::kTypeTotp) == Tap::kHide);

    ui::list::Span s = ui::list::PageSpan(14, 0);
    TEST_ASSERT_EQUAL_size_t(0, s.first);
    TEST_ASSERT_EQUAL_UINT8(6, s.count);
    s = ui::list::PageSpan(14, 2);
    TEST_ASSERT_EQUAL_size_t(12, s.first);
    TEST_ASSERT_EQUAL_UINT8(2, s.count);
    TEST_ASSERT_EQUAL_UINT8(0, ui::list::PageSpan(14, 3).count);

    char code[8];
    ui::list::FormatCode(42, code);
    TEST_ASSERT_EQUAL_STRING("000 042", code);
    ui::list::FormatCode(987654, code);
    TEST_ASSERT_EQUAL_STRING("987 654", code);
    TEST_ASSERT_EQUAL_INT64(ui::list::Window(59), ui::list::Window(30));
    TEST_ASSERT_TRUE(ui::list::Window(60) != ui::list::Window(59));
}

void test_status_fido_prompt(void) {
    namespace st = ui::status;
    st::Status s;
    s.wifi = st::Wifi::kOnline;
    s.fido_wait = true;
    char t[st::kTextSize];
    st::Left(s, t, sizeof t);
    // The bar stays as it is: the prompt window says what the buttons do.
    TEST_ASSERT_EQUAL_STRING(st::kWifi, t);
}

}  // namespace

void RegisterUiTests(void) {
    RUN_TEST(test_ui_sizes);
    RUN_TEST(test_ui_corners);
    RUN_TEST(test_ui_to_panel_every_pixel_once);
    RUN_TEST(test_ui_refresh_plan);
    RUN_TEST(test_ui_changed_pixels);
    RUN_TEST(test_ui_touch_to_screen);
    RUN_TEST(test_pin_pad_keys);
    RUN_TEST(test_pin_pad_enter);
    RUN_TEST(test_pin_pad_new_pin_twice);
    RUN_TEST(test_pin_pad_texts);
    RUN_TEST(test_status_texts);
    RUN_TEST(test_status_battery);
    RUN_TEST(test_nav_pages_and_tabs);
    RUN_TEST(test_nav_swipes);
    RUN_TEST(test_nav_fido_pages);
    RUN_TEST(test_fido_list_rows);
    RUN_TEST(test_nav_classify);
    RUN_TEST(test_fido_remove);
    RUN_TEST(test_dialog_hold);
    RUN_TEST(test_settings_pages);
    RUN_TEST(test_wifi_view);
    RUN_TEST(test_wifi_settings);
    RUN_TEST(test_touch_calibration);
    RUN_TEST(test_ble_view);
    RUN_TEST(test_display_view);
    RUN_TEST(test_time_view);
    RUN_TEST(test_security_view);
    RUN_TEST(test_web_view);
    RUN_TEST(test_fido_prompt);
    RUN_TEST(test_list_rows);
    RUN_TEST(test_status_fido_prompt);
}
