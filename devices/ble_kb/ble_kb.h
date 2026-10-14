#pragma once

#include "esp_err.h"

// BLE HID keyboard (HID over GATT): types text on the connected host.
// Characters map to a US keyboard layout; the host must use the US layout.

namespace ble_kb {

// Adds the HID service and the event listener to the ble driver. Call once,
// before ble::On().
void Register();

// True when the host has subscribed to keyboard input reports.
bool Ready();

// Types `text`: printable ASCII, '\n' (Enter) and '\t' (Tab).
// Returns ESP_ERR_INVALID_STATE without a secure, subscribed link and
// ESP_ERR_INVALID_ARG on a character that cannot be typed (nothing is sent).
esp_err_t Type(const char *text);

}  // namespace ble_kb
