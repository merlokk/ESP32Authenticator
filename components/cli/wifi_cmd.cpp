#include "wifi_cmd.h"

#include <cstdio>
#include <cstring>

#include "config.h"
#include "wifi.h"
#include "wifimgr.h"

namespace console {

namespace {

int Result(esp_err_t err, const char *what) {
    if (err == ESP_OK) {
        return 0;
    }
    printf("%s: %s\n", what, esp_err_to_name(err));
    return 1;
}

// Saves config.json and pushes the networks to the driver.
int SaveAndApply() {
    wifimgr::ApplyConfig();
    const esp_err_t err = config::Save();
    if (err != ESP_OK) {
        printf("applied, but config.json not saved: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("saved %s\n", config::Path());
    return 0;
}

int CmdOn(int, char **) {
    const int rc = Result(wifi::On(), "wifi on");
    if (rc == 0) {
        printf("wifi on%s\n", config::Get().wifi.network_count == 0
                                  ? ", no networks configured ('wifi set <ssid> <pass>')"
                                  : ", connecting ('wifi info' for state)");
    }
    return rc;
}

int CmdOff(int, char **) {
    const int rc = Result(wifi::Off(), "wifi off");
    if (rc == 0) {
        printf("wifi off\n");
    }
    return rc;
}

int CmdInfo(int, char **) {
    wifi::PrintInfo();
    return 0;
}

int CmdScan(int, char **) {
    if (!wifi::IsOn()) {
        printf("wifi is off, run 'wifi on'\n");
        return 1;
    }
    return Result(wifi::Scan(), "scan");
}

int CmdNetworks(int, char **) {
    const config::Wifi &w = config::Get().wifi;
    if (w.network_count == 0) {
        printf("no networks configured ('wifi set <ssid> <pass>')\n");
        return 0;
    }
    printf(" #  password  ssid\n");
    for (uint8_t i = 0; i < w.network_count; ++i) {
        printf("%2u  %-8s  %s\n", i + 1, w.networks[i].password[0] != '\0' ? "set" : "open",
               w.networks[i].ssid);
    }
    printf("%u of %u, tried in this order\n", w.network_count,
           static_cast<unsigned>(config::kMaxNetworks));
    return 0;
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

struct Subcommand {
    const char *name;
    int (*func)(int, char **);
};

int Dispatch(const Subcommand *subs, size_t count, int argc, char **argv, const char *usage) {
    if (argc >= 2) {
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(argv[1], subs[i].name) == 0) {
                return subs[i].func(argc - 1, argv + 1);
            }
        }
    }
    printf("usage: %s\n", usage);
    return 1;
}

constexpr Subcommand kWifiSubcommands[] = {
    {"on", &CmdOn},           {"off", &CmdOff}, {"info", &CmdInfo},
    {"scan", &CmdScan},       {"networks", &CmdNetworks},
    {"set", &CmdSet},         {"forget", &CmdForget},
};

// --- config -------------------------------------------------------------------

int CmdShow(int, char **) {
    config::Data masked = config::Get();
    for (uint8_t i = 0; i < masked.wifi.network_count; ++i) {
        if (masked.wifi.networks[i].password[0] != '\0') {
            snprintf(masked.wifi.networks[i].password, config::kPasswordSize, "***");
        }
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
    wifimgr::ApplyConfig();
    printf("reloaded %s, Wi-Fi settings applied\n", config::Path());
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
    return Dispatch(kWifiSubcommands, sizeof kWifiSubcommands / sizeof kWifiSubcommands[0], argc,
                    argv,
                    "wifi on | off | info | scan | networks | set <ssid> [password] | "
                    "forget <ssid|all>");
}

int CmdConfig(int argc, char **argv) {
    return Dispatch(kConfigSubcommands, sizeof kConfigSubcommands / sizeof kConfigSubcommands[0],
                    argc, argv, "config show | info | reload | save");
}

}  // namespace console
