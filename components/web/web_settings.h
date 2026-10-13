#pragma once

// The write path of the web settings (ported from approver-esp32). Pure:
// a JSON body applied to a config::Data, host-tested.
//
// - A whitelist, not a merge: `wifi.mode` (off/client/ap/apsta), `wifi.ap`
//   {ssid, password, channel}, `wifi.networks[]` {ssid, password}. Anything
//   else is refused by name (web.* included: the site cannot unlock itself).
// - Passwords are write-only: GET never returns one. In a POST an absent or
//   null password keeps the current one (networks matched by SSID), "" clears.
// - All or nothing: any refusal leaves `into` untouched.
// - Memory only: saving is its own action.

#include <cstddef>
#include <cstdint>

#include "config.h"

namespace web {

constexpr size_t kMaxSettingsBody = 1280;

enum class WriteResult : uint8_t {
    kOk = 0,
    kTooBig,
    kNotJson,
    kNotObject,
    kUnknownField,  // named in detail
    kBadValue,      // wrong type, or a value config refuses
    kTooLong,
    kTooMany,       // more networks than config::kMaxNetworks
};

struct WriteOutcome {
    WriteResult result = WriteResult::kOk;
    char detail[48] = {};
    bool wifi_changed = false;
};

WriteOutcome ApplySettings(const char *body, size_t length, config::Data *into);
const char *WriteResultText(WriteResult result);

// POST /api/action?do=<verb>
enum class Action : uint8_t {
    kNone = 0,
    kSave,       // write config.json
    kReload,     // re-read config.json (drops unsaved edits)
    kWifiRetry,  // apply the Wi-Fi settings and restart the walk
};

Action ActionFromUri(const char *uri);
const char *ActionName(Action action);

}  // namespace web
