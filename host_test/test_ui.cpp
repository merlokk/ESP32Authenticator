// The portrait screen on the landscape panel, the full-refresh plan
// (components/ui/ui_logic.h); the PIN pad (pin_pad_logic.h); the status bar
// (status_logic.h), the navigation (nav_logic.h) and the list (list_logic.h).

#include <cstring>
#include <string>
#include <vector>

#include "fido_list_logic.h"
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

    // Yes: held 4 s, filling; let go early: empty.
    r.Swipe(Gesture::kLeft, 0);
    r.Press(100);  // not asked yet: no hold
    TEST_ASSERT_EQUAL_UINT8(0, r.Fill(5000));
    r.Trash();
    r.Press(1000);
    TEST_ASSERT_EQUAL_UINT8(0, r.Fill(1000));
    TEST_ASSERT_EQUAL_UINT8(1, r.Fill(1500));
    TEST_ASSERT_EQUAL_UINT8(7, r.Fill(4999));
    r.Release();
    TEST_ASSERT_EQUAL_UINT8(0, r.Fill(4999));
    TEST_ASSERT_TRUE(r.stage() == fl::Stage::kAsk);
    r.Press(10000);
    TEST_ASSERT_EQUAL_UINT8(fl::kHoldSteps, r.Fill(14000));
    TEST_ASSERT_TRUE(r.stage() == fl::Stage::kDelete);
    r.Release();
    TEST_ASSERT_EQUAL_UINT8(fl::kHoldSteps, r.Fill(15000));
    TEST_ASSERT_TRUE(r.Clear());
    r.Press(0xFFFFF000u);  // the tick wraps
    TEST_ASSERT_EQUAL_UINT8(0, r.Fill(0xFFFFF000u));
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
    const std::string want = std::string("FIDO2   ") + st::kArrowLeft + " yes   " +
                             st::kArrowRight + " no";
    TEST_ASSERT_EQUAL_STRING(want.c_str(), t);
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
    RUN_TEST(test_list_rows);
    RUN_TEST(test_status_fido_prompt);
}
