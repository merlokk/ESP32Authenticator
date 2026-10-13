#include "wifi_cmd.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "config.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "subcommand.h"
#include "web_server.h"
#include "wifi_manager.h"

namespace console {

namespace {

int Result(esp_err_t err, const char *what) {
    if (err == ESP_OK) {
        return 0;
    }
    printf("%s: %s\n", what, esp_err_to_name(err));
    return 1;
}

// Saves config.json, then hands the Wi-Fi settings to the manager.
int SaveAndApply() {
    wifimgr::Apply();
    const esp_err_t err = config::Save();
    if (err != ESP_OK) {
        printf("applied, but config.json not saved: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("saved %s\n", config::Path());
    return 0;
}

void PrintIp(const char *label, uint32_t ip) {
    printf("%-10s %u.%u.%u.%u\n", label, static_cast<unsigned>(ip & 0xFF),
           static_cast<unsigned>((ip >> 8) & 0xFF), static_cast<unsigned>((ip >> 16) & 0xFF),
           static_cast<unsigned>((ip >> 24) & 0xFF));
}

void PrintMac(const char *label, esp_mac_type_t type) {
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, type) == ESP_OK) {
        printf("%-10s %02x:%02x:%02x:%02x:%02x:%02x\n", label, mac[0], mac[1], mac[2], mac[3],
               mac[4], mac[5]);
    }
}

void PrintSeconds(uint32_t ms) { printf("%" PRIu32 "m %02" PRIu32 "s", ms / 60000, ms / 1000 % 60); }

int CmdInfo(int, char **) {
    const config::Wifi &w = config::Get().wifi;
    const wifimgr::Snapshot s = wifimgr::Get();
    printf("config     %s, mode %s\n", w.active ? "on" : "off", config::Name(w.mode));
    printf("state      %s (asked: %s)", wifimgr::Name(s.state), wifimgr::Name(s.desired));
    if (s.state == wifimgr::State::kWaiting) {
        printf(", %s ", s.resting ? "resting, next cycle in" : "next attempt in");
        PrintSeconds(s.wait_remaining_ms);
    } else if (s.state == wifimgr::State::kApWindow) {
        printf(", back to the networks ");
        if (s.ap_window_remaining_ms == wifimgr::kHeldOpen) {
            printf("when the AP is empty");
        } else {
            printf("in ");
            PrintSeconds(s.ap_window_remaining_ms);
        }
    }
    printf("\n");
    const bool walking = s.state == wifimgr::State::kConnecting ||
                         s.state == wifimgr::State::kOnline ||
                         (s.state == wifimgr::State::kWaiting && !s.resting);
    if (walking && s.network < w.network_count) {
        printf("network    '%s' (%u of %u), round %u\n", w.networks[s.network].ssid,
               static_cast<unsigned>(s.network + 1), static_cast<unsigned>(w.network_count),
               static_cast<unsigned>(s.round + 1));
    }
    if (s.failure != wifimgr::Failure::kNone) {
        printf("last error %s", wifimgr::Name(s.failure));
        if (s.radio.reason != 0) {
            printf(" (reason %u)", static_cast<unsigned>(s.radio.reason));
        }
        printf("\n");
    }
    if (s.radio_error != ESP_OK) {
        printf("radio      init failed: %s\n", esp_err_to_name(s.radio_error));
    }
    printf("radio      %s", wifi::Radio::Name(s.radio.mode));
    if (s.radio.channel != 0) {
        printf(", channel %u", static_cast<unsigned>(s.radio.channel));
    }
    printf("\n");
    PrintMac("mac sta", ESP_MAC_WIFI_STA);
    PrintMac("mac ap", ESP_MAC_WIFI_SOFTAP);
    printf("hostname   %s\n", w.hostname);
    if (s.radio.link == wifi::Link::kConnected) {
        printf("joined     '%s', %d dBm\n", s.radio.ssid, s.radio.rssi);
        PrintIp(s.radio.ip_is_static ? "ip static" : "ip dhcp", s.radio.ip);
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip = {};
        if (netif != nullptr && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
            PrintIp("netmask", ip.netmask.addr);
            PrintIp("gateway", ip.gw.addr);
            esp_netif_dns_info_t dns = {};
            if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
                PrintIp("dns", dns.ip.u_addr.ip4.addr);
            }
        }
    }
    if (s.radio.mode == wifi::Mode::kAp || s.radio.mode == wifi::Mode::kApSta) {
        printf("ap         '%s', %u client(s)\n", s.radio.ap_ssid,
               static_cast<unsigned>(s.radio.clients));
        PrintIp("ap ip", s.radio.ap_ip);
    }
    return 0;
}

int CmdScan(int, char **) {
    static wifi::ScanResult found[wifi::kMaxScanResults];
    size_t count = 0;
    const int rc = Result(wifimgr::Scan(found, wifi::kMaxScanResults, &count), "scan");
    if (rc != 0) {
        return rc;
    }
    printf("rssi  ch  auth       ssid\n");
    for (size_t i = 0; i < count; ++i) {
        printf("%4d  %2u  %-9s  %s\n", found[i].rssi, found[i].channel,
               wifi::Radio::AuthName(found[i].auth),
               found[i].ssid[0] != '\0' ? found[i].ssid : "(hidden)");
    }
    printf("%u network(s), 2.4 GHz only\n", static_cast<unsigned>(count));
    return 0;
}

int CmdOn(int, char **) {
    config::Get().wifi.active = true;
    return SaveAndApply();
}

int CmdOff(int, char **) {
    config::Get().wifi.active = false;
    return SaveAndApply();
}

int CmdMode(int argc, char **argv) {
    config::WifiMode mode;
    if (argc != 2 || !config::ParseWifiMode(argv[1], &mode)) {
        printf("usage: wifi mode client | ap | apsta   ('wifi off' switches the radio off)\n");
        return 1;
    }
    config::Wifi &w = config::Get().wifi;
    w.mode = mode;
    w.active = true;
    return SaveAndApply();
}

int CmdAp(int argc, char **argv) {
    config::Wifi &w = config::Get().wifi;
    if (argc == 1) {
        printf("ap         '%s', %s, channel %u\n", w.ap_ssid,
               w.ap_password[0] != '\0' ? "wpa2" : "open", static_cast<unsigned>(w.ap_channel));
        return 0;
    }
    if (argc > 4) {
        printf("usage: wifi ap [<ssid> [password|\"\"] [channel]]\n");
        return 1;
    }
    const char *password = argc >= 3 ? argv[2] : "";
    const long channel = argc == 4 ? strtol(argv[3], nullptr, 10) : w.ap_channel;
    if (!config::ValidSsid(argv[1])) {
        printf("ssid: 1..32 bytes\n");
        return 1;
    }
    if (!config::ValidApPassword(password)) {
        printf("password: empty (open) or 8..63 characters\n");
        return 1;
    }
    if (channel < 1 || channel > 13) {
        printf("channel: 1..13\n");
        return 1;
    }
    snprintf(w.ap_ssid, sizeof w.ap_ssid, "%s", argv[1]);
    snprintf(w.ap_password, sizeof w.ap_password, "%s", password);
    w.ap_channel = static_cast<uint8_t>(channel);
    return SaveAndApply();
}

int CmdRetry(int, char **) {
    wifimgr::Apply();
    printf("restarted from the top ('wifi info' for state)\n");
    return 0;
}

int CmdNetworks(int, char **) {
    const config::Wifi &w = config::Get().wifi;
    if (w.network_count == 0) {
        printf("no networks configured ('wifi set <ssid> <pass>')\n");
        return 0;
    }
    const wifimgr::Snapshot s = wifimgr::Get();
    printf(" #  password  ssid\n");
    for (uint8_t i = 0; i < w.network_count; ++i) {
        const config::StaticIp &ip = w.networks[i].ip;
        printf("%2u  %-8s  %s%s\n", i + 1, w.networks[i].password[0] != '\0' ? "set" : "open",
               w.networks[i].ssid, s.auth_failed[i] ? "  (password refused)" : "");
        if (ip.enabled) {
            printf("    static    %s/%s gw %s", ip.address, ip.netmask, ip.gateway);
            if (ip.dns1[0] != '\0') {
                printf(" dns %s%s%s", ip.dns1, ip.dns2[0] != '\0' ? " " : "", ip.dns2);
            }
            printf("\n");
        } else if (ip.address[0] != '\0') {
            printf("    dhcp      (static %s kept, off)\n", ip.address);
        }
    }
    printf("%u of %u, tried in this order\n", w.network_count,
           static_cast<unsigned>(config::kMaxNetworks));
    return 0;
}

// wifi static <n> <address> <netmask> <gateway> [dns1] [dns2] | wifi static <n> off
int CmdStatic(int argc, char **argv) {
    config::Wifi &w = config::Get().wifi;
    const long n = argc >= 3 ? strtol(argv[1], nullptr, 10) : 0;
    const bool off = argc == 3 && strcmp(argv[2], "off") == 0;
    if (n < 1 || n > w.network_count || (!off && (argc < 5 || argc > 7))) {
        printf("usage: wifi static <n> <address> <netmask> <gateway> [dns1] [dns2]\n");
        printf("       wifi static <n> off      (n as listed by 'wifi networks')\n");
        return 1;
    }
    config::StaticIp &target = w.networks[n - 1].ip;
    if (off) {
        target.enabled = false;  // the typed address is kept for next time
        return SaveAndApply();
    }
    // Every field checked before any of it is stored.
    config::StaticIp ip = {};
    ip.enabled = true;
    char *const fields[] = {ip.address, ip.netmask, ip.gateway, ip.dns1, ip.dns2};
    for (int i = 2; i < argc; ++i) {
        if (strlen(argv[i]) >= config::kIpTextSize) {
            printf("'%s' is not an IPv4 address\n", argv[i]);
            return 1;
        }
        snprintf(fields[i - 2], config::kIpTextSize, "%s", argv[i]);
    }
    char error[config::kErrorSize] = {};
    if (!config::CheckStaticIp(ip, error, sizeof error)) {
        printf("%s\n", error);
        return 1;
    }
    target = ip;
    return SaveAndApply();
}

int CmdSet(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        printf("usage: wifi set <ssid> [password]   (quote an SSID with spaces)\n");
        return 1;
    }
    char error[config::kErrorSize] = {};
    if (!config::SetNetwork(&config::Get().wifi, argv[1], argc == 3 ? argv[2] : "", error,
                            sizeof error)) {
        printf("%s\n", error);
        return 1;
    }
    return SaveAndApply();
}

int CmdForget(int argc, char **argv) {
    if (argc != 2) {
        printf("usage: wifi forget <ssid> | all\n");
        return 1;
    }
    config::Wifi &w = config::Get().wifi;
    if (strcmp(argv[1], "all") == 0) {
        memset(w.networks, 0, sizeof w.networks);
        w.network_count = 0;
    } else if (!config::RemoveNetwork(&w, argv[1])) {
        printf("no network '%s' ('wifi networks' to list)\n", argv[1]);
        return 1;
    }
    return SaveAndApply();
}

constexpr Subcommand kWifiSubcommands[] = {
    {"on", &CmdOn},       {"off", &CmdOff},           {"mode", &CmdMode},
    {"info", &CmdInfo},   {"scan", &CmdScan},         {"ap", &CmdAp},
    {"retry", &CmdRetry}, {"networks", &CmdNetworks}, {"set", &CmdSet},
    {"static", &CmdStatic},
    {"forget", &CmdForget},
};

// --- web ------------------------------------------------------------------------

int SaveWeb(const char *what) {
    web::Apply();
    const esp_err_t err = config::Save();
    if (err != ESP_OK) {
        printf("%s, but config.json not saved: %s\n", what, esp_err_to_name(err));
        return 1;
    }
    printf("%s, saved %s\n", what, config::Path());
    return 0;
}

int CmdWebInfo(int, char **) {
    const config::Web &cfg = config::Get().web;
    const web::Status s = web::Get();
    const wifimgr::Snapshot w = wifimgr::Get();
    printf("wanted     %s\n", web::Name(s.desired));
    printf("server     %s", s.running ? "running on port 80" : "stopped");
    if (!s.running && s.desired != web::Desired::kOff) {
        printf(" - %s", !w.stack_up ? "the radio is off"
                        : s.desired == web::Desired::kAuto ? "auto: waits for the device's own AP"
                                                           : "starting");
    }
    printf("\n");
    if (s.running) {
        if (w.radio.ip != 0) {
            const uint32_t ip = w.radio.ip;
            printf("reach      http://%u.%u.%u.%u/\n", static_cast<unsigned>(ip & 0xFF),
                   static_cast<unsigned>((ip >> 8) & 0xFF),
                   static_cast<unsigned>((ip >> 16) & 0xFF), static_cast<unsigned>(ip >> 24));
        }
        if (w.radio.ap_ip != 0) {
            const uint32_t ip = w.radio.ap_ip;
            printf("reach      http://%u.%u.%u.%u/ (on '%s')\n", static_cast<unsigned>(ip & 0xFF),
                   static_cast<unsigned>((ip >> 8) & 0xFF),
                   static_cast<unsigned>((ip >> 16) & 0xFF), static_cast<unsigned>(ip >> 24),
                   w.radio.ap_ssid);
        }
        printf("cost       %" PRIu32 " byte(s) of heap\n", s.cost);
    }
    if (s.auth) {
        printf("auth       basic, user '%s'\n", cfg.user);
    } else {
        printf("auth       none: open to anyone who can reach it ('web login <user> <pw>')\n");
    }
    printf("write      %s\n", cfg.write ? "allowed" : "refused (read-only pages)");
    printf("served     %" PRIu32 " page(s), %" PRIu32 " byte(s), %" PRIu32 " api, %" PRIu32
           " not found, %" PRIu32 " unauthorised\n",
           s.files, s.bytes, s.api, s.not_found, s.unauthorised);
    return 0;
}

int SetWebMode(config::WebMode mode) {
    config::Get().web.mode = mode;
    char what[32];
    snprintf(what, sizeof what, "web %s", config::Name(mode));
    return SaveWeb(what);
}

int CmdWebOn(int, char **) { return SetWebMode(config::WebMode::kOn); }
int CmdWebOff(int, char **) { return SetWebMode(config::WebMode::kOff); }
int CmdWebAuto(int, char **) { return SetWebMode(config::WebMode::kAuto); }

int CmdWebLogin(int argc, char **argv) {
    config::Web &cfg = config::Get().web;
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        cfg.user[0] = '\0';
        cfg.password[0] = '\0';
        return SaveWeb("login off: the site is open");
    }
    // Both or neither: half a credential is an open site.
    if (argc != 3 || argv[1][0] == '\0' || argv[2][0] == '\0') {
        printf("usage: web login <user> <password> | web login off\n");
        return 1;
    }
    if (!config::ValidWebUser(argv[1]) || strlen(argv[2]) >= sizeof cfg.password) {
        printf("user: 1..32 characters, no ':'; password: 1..64 characters\n");
        return 1;
    }
    snprintf(cfg.user, sizeof cfg.user, "%s", argv[1]);
    snprintf(cfg.password, sizeof cfg.password, "%s", argv[2]);
    printf("basic auth is not TLS: the password crosses the network readable\n");
    return SaveWeb("login set");
}

int CmdWebWrite(int argc, char **argv) {
    const bool on = argc == 2 && strcmp(argv[1], "on") == 0;
    if (argc != 2 || (!on && strcmp(argv[1], "off") != 0)) {
        printf("usage: web write on | off\n");
        return 1;
    }
    config::Get().web.write = on;
    return SaveWeb(on ? "settings writable from the site" : "site read-only");
}

constexpr Subcommand kWebSubcommands[] = {
    {"info", &CmdWebInfo},   {"on", &CmdWebOn},       {"off", &CmdWebOff},
    {"auto", &CmdWebAuto},   {"login", &CmdWebLogin}, {"write", &CmdWebWrite},
};

// --- config -----------------------------------------------------------------------

int CmdShow(int, char **) {
    config::Data masked = config::Get();
    for (uint8_t i = 0; i < masked.wifi.network_count; ++i) {
        if (masked.wifi.networks[i].password[0] != '\0') {
            snprintf(masked.wifi.networks[i].password, config::kPasswordSize, "***");
        }
    }
    if (masked.wifi.ap_password[0] != '\0') {
        snprintf(masked.wifi.ap_password, config::kPasswordSize, "***");
    }
    if (masked.web.password[0] != '\0') {
        snprintf(masked.web.password, config::kWebPasswordSize, "***");
    }
    static char json[config::kMaxFileSize];
    if (config::Serialize(masked, json, sizeof json) == 0) {
        printf("does not fit in %u bytes\n", static_cast<unsigned>(sizeof json));
        return 1;
    }
    printf("%s\n", json);
    return 0;
}

int CmdConfigInfo(int, char **) {
    printf("file       %s\n", config::Path());
    printf("source     %s\n",
           config::LoadedFrom() == config::Source::kFile ? "file" : "built-in defaults");
    if (config::LastError()[0] != '\0') {
        printf("error      %s\n", config::LastError());
    }
    return 0;
}

int CmdReload(int, char **) {
    const esp_err_t err = config::Reload();
    if (err != ESP_OK) {
        printf("not reloaded, values unchanged: %s\n", config::LastError());
        return 1;
    }
    wifimgr::Apply();
    web::Apply();
    printf("reloaded %s, Wi-Fi and web settings applied\n", config::Path());
    return 0;
}

int CmdSave(int, char **) {
    const int rc = Result(config::Save(), "save");
    if (rc == 0) {
        printf("saved %s\n", config::Path());
    }
    return rc;
}

constexpr Subcommand kConfigSubcommands[] = {
    {"show", &CmdShow},
    {"info", &CmdConfigInfo},
    {"reload", &CmdReload},
    {"save", &CmdSave},
};

}  // namespace

int CmdWifi(int argc, char **argv) {
    return Dispatch(kWifiSubcommands, argc, argv,
                    "wifi on | off | mode <client|ap|apsta> | info | scan | "
                    "ap [<ssid> [password] [channel]] | retry | networks | "
                    "set <ssid> [password] | static <n> <ip> <mask> <gw> [dns1] [dns2] | "
                    "static <n> off | forget <ssid|all>");
}

int CmdWeb(int argc, char **argv) {
    return Dispatch(kWebSubcommands, argc, argv,
                    "web info | on | off | auto | login <user> <password> | login off | "
                    "write on|off");
}

int CmdConfig(int argc, char **argv) {
    return Dispatch(kConfigSubcommands, argc, argv, "config show | info | reload | save");
}

void PrintWifiInfo() { CmdInfo(0, nullptr); }

void PrintWebInfo() { CmdWebInfo(0, nullptr); }

}  // namespace console
