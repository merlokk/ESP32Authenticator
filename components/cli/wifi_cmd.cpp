#include "wifi_cmd.h"

#include <cstdio>
#include <cstring>

#include "wifi.h"

namespace console {

namespace {

int Result(esp_err_t err, const char *what) {
    if (err == ESP_OK) {
        return 0;
    }
    printf("%s: %s\n", what, esp_err_to_name(err));
    return 1;
}

int CmdOn(int, char **) {
    const int rc = Result(wifi::On(), "wifi on");
    if (rc == 0) {
        printf("wifi on%s\n", !wifi::HasNetwork() ? ", no network saved ('wifi set <ssid> <pass>')"
                               : wifi::Connected()  ? ""
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

int CmdSet(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        printf("usage: wifi set <ssid> [password]   (quote an SSID with spaces)\n");
        return 1;
    }
    const esp_err_t err = wifi::SetCredentials(argv[1], argc == 3 ? argv[2] : "");
    if (err == ESP_ERR_INVALID_ARG) {
        printf("bad ssid (1..32 bytes) or password (8..64 chars, or none for open)\n");
        return 1;
    }
    const int rc = Result(err, "set");
    if (rc == 0) {
        printf("saved '%s'%s\n", argv[1], wifi::IsOn() ? ", connecting" : ", 'wifi on' to connect");
    }
    return rc;
}

int CmdForget(int, char **) {
    const int rc = Result(wifi::ForgetCredentials(), "forget");
    if (rc == 0) {
        printf("saved network deleted\n");
    }
    return rc;
}

struct Subcommand {
    const char *name;
    int (*func)(int, char **);
};

constexpr Subcommand kSubcommands[] = {
    {"on", &CmdOn},   {"off", &CmdOff}, {"info", &CmdInfo},
    {"scan", &CmdScan}, {"set", &CmdSet}, {"forget", &CmdForget},
};

}  // namespace

int CmdWifi(int argc, char **argv) {
    if (argc >= 2) {
        for (const Subcommand &sub : kSubcommands) {
            if (strcmp(argv[1], sub.name) == 0) {
                return sub.func(argc - 1, argv + 1);
            }
        }
    }
    printf("usage: wifi on | off | info | scan | set <ssid> [password] | forget\n");
    return 1;
}

}  // namespace console
