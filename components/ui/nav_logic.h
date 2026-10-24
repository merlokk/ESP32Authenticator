#pragma once

// Navigation between the screens (x4-design.md): a tab bar at the bottom
// picks a screen; Left / Right flip its pages and past the first / last page
// go to the screen before / after (round), so the buttons alone reach all of
// it; Home goes to the first page of Passwords. Each screen keeps its page.
// Pure (host-tested); the PIN pad stands over all of it while the vault is
// locked.

#include <cstddef>
#include <cstdint>

namespace ui::nav {

enum class Screen : uint8_t {
    kPasswords,  // the records: a tap types the login / password / TOTP
    kTotp,       // the TOTP codes
    kFido,       // the FIDO2 keys sites made
    kSettings,   // frontlight, Wi-Fi, Bluetooth, the device
};
inline constexpr uint8_t kScreens = 4;

inline constexpr const char *kTitles[kScreens] = {"Passwords", "TOTP", "FIDO2", "Settings"};

constexpr const char *Title(Screen s) { return kTitles[static_cast<uint8_t>(s)]; }

enum class Input : uint8_t {
    kLeft,
    kRight,
    kHome,
};

// Pages for `items` in rows of `per_page`: at least one.
constexpr uint8_t PagesFor(size_t items, size_t per_page) {
    const size_t n = per_page == 0 ? 1 : (items + per_page - 1) / per_page;
    return static_cast<uint8_t>(n == 0 ? 1 : n > 255 ? 255 : n);
}

class Nav {
   public:
    constexpr Screen screen() const { return screen_; }
    constexpr uint8_t page() const { return page_[Index(screen_)]; }
    constexpr uint8_t pages() const { return pages_[Index(screen_)]; }
    constexpr uint8_t page(Screen s) const { return page_[Index(s)]; }
    constexpr uint8_t pages(Screen s) const { return pages_[Index(s)]; }

    // A screen's page count (its list changed); its page stays inside. True
    // when the page moved.
    constexpr bool SetPages(Screen s, uint8_t n) {
        const uint8_t i = Index(s);
        pages_[i] = n < 1 ? 1 : n;
        if (page_[i] >= pages_[i]) {
            page_[i] = static_cast<uint8_t>(pages_[i] - 1);
            return true;
        }
        return false;
    }

    // A tab: that screen, at the page it was left on. True when it changed.
    constexpr bool Go(Screen s) {
        if (s == screen_) {
            return false;
        }
        screen_ = s;
        return true;
    }

    // A button. True when the screen or the page changed.
    constexpr bool Apply(Input in) {
        const uint8_t i = Index(screen_);
        switch (in) {
            case Input::kHome: {
                const bool changed = screen_ != Screen::kPasswords || page_[0] != 0;
                screen_ = Screen::kPasswords;
                page_[0] = 0;
                return changed;
            }
            case Input::kRight:
                if (page_[i] + 1 < pages_[i]) {
                    ++page_[i];
                } else {
                    screen_ = static_cast<Screen>((i + 1) % kScreens);
                    page_[Index(screen_)] = 0;
                }
                return true;
            case Input::kLeft:
                if (page_[i] > 0) {
                    --page_[i];
                } else {
                    screen_ = static_cast<Screen>((i + kScreens - 1) % kScreens);
                    page_[Index(screen_)] = static_cast<uint8_t>(pages_[Index(screen_)] - 1);
                }
                return true;
        }
        return false;
    }

   private:
    static constexpr uint8_t Index(Screen s) { return static_cast<uint8_t>(s); }

    Screen screen_ = Screen::kPasswords;
    uint8_t page_[kScreens] = {};
    uint8_t pages_[kScreens] = {1, 1, 1, 1};
};

}  // namespace ui::nav
