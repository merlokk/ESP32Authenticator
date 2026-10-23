#pragma once

// The configuration web server (ported from approver-esp32): esp_http_server
// on port 80, pages embedded in the firmware (`www/`; SPIFFS is small and
// holds the reader's stock data).
//
// Routes: GET pages, /api/status, /api/devstatus and /api/hwinfo (console dumps),
// /api/vault and /api/fido (web_admin_routes.h),
// /api/settings, /api/wifi/scan; POST /api/settings, /api/action?do=,
// /api/reboot?confirm=reboot. Every handler goes through basic auth first
// (web_auth.h); web.write false refuses the POSTs except reboot.
//
// Desired state (off / on / auto) is reconciled on the Wi-Fi manager's tick:
// the server comes up only when there is a network stack, and goes down
// before the radio does.

#include <cstdint>

#include "esp_err.h"
#include "web_policy.h"

namespace web {

constexpr uint16_t kPort = 80;

struct Status {
    Desired desired = Desired::kAuto;
    bool running = false;
    bool auth = false;  // a credential is required
    uint32_t files = 0;
    uint32_t api = 0;
    uint32_t not_found = 0;  // refused or missing: the same answer to the client
    uint32_t unauthorised = 0;
    uint32_t bytes = 0;
    uint32_t cost = 0;  // heap the running server took at start
};

// Registers the reconciler on the Wi-Fi manager's tick and reads web.mode.
esp_err_t Init();

// In memory; config::Save() persists web.mode.
void SetDesired(Desired desired);
Desired DesiredFromConfig();
// Re-reads web.mode (after config reload).
void Apply();

// Prints the device dump for /api/devstatus (the console's, through a stdout
// swap); 503 until set. A hook: the console depends on this component.
using DiagnosticsPrinter = void (*)();
void SetDiagnostics(DiagnosticsPrinter printer);
// The same for /api/hwinfo: the console's `hwinfo`.
void SetHardwareInfo(DiagnosticsPrinter printer);
// Called on every request (power::NoteActivity: the site in use keeps it on).
void SetRequestHook(void (*hook)());

bool Running();
Status Get();

}  // namespace web
