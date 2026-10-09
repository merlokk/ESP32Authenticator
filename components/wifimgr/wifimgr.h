#pragma once

// Glue between config and the Wi-Fi driver: the driver stores nothing, this
// hands it the networks and hostname from config.json.

namespace wifimgr {

// Pushes config's wifi.networks and wifi.hostname to the driver. If the radio
// is on it reconnects from the first network.
void ApplyConfig();

// Boot: ApplyConfig(), then start the radio if wifi.active.
void Start();

}  // namespace wifimgr
