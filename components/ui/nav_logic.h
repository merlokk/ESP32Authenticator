#pragma once

// Navigation (x4-design.md): the password list in pages, a tab per page at
// the bottom (at most kTabs shown; with more pages the row scrolls so the
// current one is in it). Left / Right flip the pages (round). A swipe down
// from the top quarter opens Settings, a swipe up from the bottom quarter the
// FIDO2 list; the opposite swipe or Home closes them. Home on the list: its
// first page. Pure (host-tested); the PIN pad stands over all of it while
// the vault is locked.

#include <cstddef>
#include <cstdint>

namespace ui::nav {

enum class Mode : uint8_t {
    kList,      // the password records
    kSettings,  // pulled down from the top
    kFido,      // pulled up from the bottom
};

enum class Input : uint8_t {
    kLeft,
    kRight,
    kHome,
};

// A finger's path on the screen, from where it went down to where it lifted.
enum class Gesture : uint8_t {
    kNone,          // moved, but no swipe (a tap that slid)
    kTap,
    kDownFromTop,   // a swipe down that started in the top quarter
    kUpFromBottom,  // a swipe up that started in the bottom quarter
    kDown,          // a swipe down from elsewhere
    kUp,
};

inline constexpr int32_t kTapSlop = 24;    // px a tap may slide
inline constexpr int32_t kSwipeMin = 100;  // px a swipe goes, mostly vertically

constexpr Gesture Classify(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t height) {
    const int32_t dx = x1 - x0;
    const int32_t dy = y1 - y0;
    const int32_t ax = dx < 0 ? -dx : dx;
    const int32_t ay = dy < 0 ? -dy : dy;
    if (ax <= kTapSlop && ay <= kTapSlop) {
        return Gesture::kTap;
    }
    if (ay < kSwipeMin || ay < 2 * ax) {
        return Gesture::kNone;
    }
    if (dy > 0) {
        return y0 < height / 4 ? Gesture::kDownFromTop : Gesture::kDown;
    }
    return y0 >= height - height / 4 ? Gesture::kUpFromBottom : Gesture::kUp;
}

inline constexpr uint8_t kTabs = 4;

// Pages for `items` in rows of `per_page`: at least one.
constexpr uint8_t PagesFor(size_t items, size_t per_page) {
    const size_t n = per_page == 0 ? 1 : (items + per_page - 1) / per_page;
    return static_cast<uint8_t>(n == 0 ? 1 : n > 255 ? 255 : n);
}

class Nav {
   public:
    constexpr Mode mode() const { return mode_; }
    constexpr uint8_t page() const { return page_; }
    constexpr uint8_t pages() const { return pages_; }
    // The first page with a tab: tabs show pages first() .. first() + tabs() - 1.
    constexpr uint8_t first() const { return first_; }
    constexpr uint8_t tabs() const { return pages_ < kTabs ? pages_ : kTabs; }

    // The list's page count (records added or gone); the page stays inside.
    // True when the page moved.
    constexpr bool SetPages(uint8_t n) {
        pages_ = n < 1 ? 1 : n;
        const bool moved = page_ >= pages_;
        if (moved) {
            page_ = static_cast<uint8_t>(pages_ - 1);
        }
        Follow();
        return moved;
    }

    // A tab: its page. True when it changed.
    constexpr bool Tab(uint8_t index) {
        const uint8_t p = static_cast<uint8_t>(first_ + index);
        if (mode_ != Mode::kList || index >= tabs() || p == page_) {
            return false;
        }
        page_ = p;
        Follow();
        return true;
    }

    // A button. True when anything shown changed.
    constexpr bool Apply(Input in) {
        if (in == Input::kHome) {
            const bool changed = mode_ != Mode::kList || page_ != 0;
            mode_ = Mode::kList;
            page_ = 0;
            Follow();
            return changed;
        }
        if (mode_ != Mode::kList || pages_ < 2) {
            return false;
        }
        page_ = in == Input::kRight ? static_cast<uint8_t>((page_ + 1) % pages_)
                                    : static_cast<uint8_t>((page_ + pages_ - 1) % pages_);
        Follow();
        return true;
    }

    // A swipe. True when the mode changed.
    constexpr bool Swipe(Gesture g) {
        Mode next = mode_;
        switch (mode_) {
            case Mode::kList:
                next = g == Gesture::kDownFromTop    ? Mode::kSettings
                       : g == Gesture::kUpFromBottom ? Mode::kFido
                                                     : mode_;
                break;
            case Mode::kSettings:
                next = g == Gesture::kUp || g == Gesture::kUpFromBottom ? Mode::kList : mode_;
                break;
            case Mode::kFido:
                next = g == Gesture::kDown || g == Gesture::kDownFromTop ? Mode::kList : mode_;
                break;
        }
        const bool changed = next != mode_;
        mode_ = next;
        return changed;
    }

   private:
    // The tab row scrolls only as far as it takes to show the page.
    constexpr void Follow() {
        if (page_ < first_) {
            first_ = page_;
        } else if (page_ >= first_ + kTabs) {
            first_ = static_cast<uint8_t>(page_ - kTabs + 1);
        }
        const uint8_t last_first = pages_ > kTabs ? static_cast<uint8_t>(pages_ - kTabs) : 0;
        if (first_ > last_first) {
            first_ = last_first;
        }
    }

    Mode mode_ = Mode::kList;
    uint8_t page_ = 0;
    uint8_t pages_ = 1;
    uint8_t first_ = 0;
};

}  // namespace ui::nav
