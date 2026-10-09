#include "wifimgr.h"

#include <cstring>

#include "config.h"
#include "wifi.h"

namespace wifimgr {

static_assert(config::kMaxNetworks == wifi::kMaxNetworks);
static_assert(config::kSsidSize == wifi::kSsidSize);
static_assert(config::kPasswordSize == wifi::kPasswordSize);

void ApplyConfig() {
    const config::Wifi &cfg = config::Get().wifi;
    wifi::Network list[wifi::kMaxNetworks] = {};
    for (uint8_t i = 0; i < cfg.network_count; ++i) {
        memcpy(list[i].ssid, cfg.networks[i].ssid, sizeof list[i].ssid);
        memcpy(list[i].password, cfg.networks[i].password, sizeof list[i].password);
    }
    wifi::SetHostname(cfg.hostname);
    wifi::SetNetworks(list, cfg.network_count);
}

void Start() {
    ApplyConfig();
    if (config::Get().wifi.active) {
        wifi::On();
    }
}

}  // namespace wifimgr
