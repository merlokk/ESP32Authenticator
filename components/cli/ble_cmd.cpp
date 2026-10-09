#include "ble_cmd.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ble.h"
#include "ble_fido.h"
#include "ble_kb.h"
#include "host/ble_hs.h"

namespace console {

namespace {

constexpr uint32_t kDefaultPairingSeconds = 60;

int Result(esp_err_t err, const char *what) {
    if (err == ESP_OK) {
        return 0;
    }
    printf("%s: %s\n", what, esp_err_to_name(err));
    return 1;
}

bool RequireOn() {
    if (!ble::IsOn()) {
        printf("ble is off, run 'ble on'\n");
        return false;
    }
    return true;
}

int CmdOn(int, char **) {
    const int rc = Result(ble::On(), "ble on");
    if (rc == 0) {
        printf("ble on, advertising as '%s'\n", ble::kDeviceName);
    }
    return rc;
}

int CmdOff(int, char **) {
    const int rc = Result(ble::Off(), "ble off");
    if (rc == 0) {
        printf("ble off\n");
    }
    return rc;
}

int CmdInfo(int, char **) {
    ble::PrintInfo();
    if (ble::IsOn()) {
        uint32_t requests = 0;
        uint32_t errors = 0;
        ble_fido::Stats(&requests, &errors);
        printf("keyboard   %s\n", ble_kb::Ready() ? "ready" : "not subscribed");
        printf("fido       %s, %" PRIu32 " request(s), %" PRIu32 " error(s)\n",
               ble_fido::Ready() ? "ready" : "not subscribed", requests, errors);
    }
    return 0;
}

int CmdPair(int argc, char **argv) {
    if (!RequireOn()) {
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "stop") == 0) {
        ble::StopPairing();
        printf("pairing closed\n");
        return 0;
    }
    const bool background = argc == 3 && strcmp(argv[2], "bg") == 0;
    const uint32_t seconds = argc >= 2 ? strtoul(argv[1], nullptr, 10) : kDefaultPairingSeconds;
    if (argc > 3 || (argc == 3 && !background) || seconds == 0) {
        printf("usage: ble pair [seconds [bg]] | ble pair stop\n");
        return 1;
    }
    ble::StartPairing(seconds);
    printf("pairing open for %" PRIu32 " s: add '%s' on the host\n", seconds, ble::kDeviceName);
    if (background) {
        return 0;  // the passkey goes to the log only
    }
    fflush(stdout);

    // Blocks the console until a new bond or the end of the window. A failed
    // attempt keeps waiting: the host may retry inside the window.
    ble::PairingEvent event = {};
    while (ble::PairingSecondsLeft() > 0) {
        if (!ble::WaitPairingEvent(&event, 500)) {
            continue;
        }
        switch (event.type) {
            case ble::PairingEvent::kPasskey:
                printf("passkey %06" PRIu32 "\n", event.passkey);
                break;
            case ble::PairingEvent::kPaired:
                printf("paired with %s\n", event.peer);
                return 0;
            case ble::PairingEvent::kFailed:
                printf("attempt failed: %s\n", event.reason);
                break;
        }
        fflush(stdout);
    }
    printf("pairing window closed, no new bond\n");
    return 1;
}

int CmdBonds(int, char **) {
    ble::PrintBonds();
    return 0;
}

int CmdConns(int, char **) {
    ble::PrintConnection();
    if (ble::ConnHandle() != BLE_HS_CONN_HANDLE_NONE) {
        printf("keyboard   %s\n", ble_kb::Ready() ? "ready" : "not subscribed");
        printf("fido       %s\n", ble_fido::Ready() ? "ready" : "not subscribed");
    }
    return 0;
}

int CmdUse(int argc, char **argv) {
    if (argc != 2) {
        printf("usage: ble use <#> | <aa:bb:cc:dd:ee:ff> | any   (# from 'ble bonds')\n");
        return 1;
    }
    if (!RequireOn()) {
        return 1;
    }
    const bool any = strcmp(argv[1], "any") == 0;
    const int rc = Result(ble::UseBond(any ? nullptr : argv[1]), "use");
    if (rc == 0) {
        printf(any ? "any bonded host may connect\n"
                   : "target set: keys released and link dropped if another host was "
                     "connected; waiting for the target to reconnect\n");
    }
    return rc;
}

int CmdDisconnect(int, char **) {
    if (!RequireOn()) {
        return 1;
    }
    const int rc = Result(ble::Disconnect(), "disconnect");
    if (rc == 0) {
        printf("disconnected (keys released first)\n");
    }
    return rc;
}

int CmdUnpair(int argc, char **argv) {
    if (argc != 2) {
        printf("usage: ble unpair <#> | <aa:bb:cc:dd:ee:ff> | all   (# from 'ble bonds')\n");
        return 1;
    }
    if (!RequireOn()) {
        return 1;
    }
    const bool all = strcmp(argv[1], "all") == 0;
    const int rc = Result(ble::Unpair(all ? nullptr : argv[1]), "unpair");
    if (rc == 0) {
        printf("%s\n", all ? "all bonds deleted" : "bond deleted");
    }
    return rc;
}

// Joins argv[1..] with spaces and expands "\n", "\t", "\\".
int CmdKb(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: ble kb <text>\n");
        return 1;
    }
    static char text[256];
    size_t n = 0;
    for (int i = 1; i < argc; ++i) {
        for (const char *s = argv[i]; *s != '\0' && n + 1 < sizeof text; ++s) {
            char c = *s;
            if (c == '\\' && s[1] != '\0') {
                ++s;
                c = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s;
            }
            text[n++] = c;
        }
        if (i + 1 < argc && n + 1 < sizeof text) {
            text[n++] = ' ';
        }
    }
    text[n] = '\0';
    if (!RequireOn()) {
        return 1;
    }
    const esp_err_t err = ble_kb::Type(text);
    if (err == ESP_ERR_INVALID_STATE) {
        printf("keyboard not ready: no secure connection or host not subscribed\n");
        return 1;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        printf("text has a character that cannot be typed (US layout, ASCII only)\n");
        return 1;
    }
    const int rc = Result(err, "kb");
    if (rc == 0) {
        printf("typed %u char(s)\n", static_cast<unsigned>(n));
    }
    return rc;
}

struct Subcommand {
    const char *name;
    int (*func)(int, char **);
};

constexpr Subcommand kSubcommands[] = {
    {"on", &CmdOn},         {"off", &CmdOff},       {"info", &CmdInfo},
    {"pair", &CmdPair},     {"bonds", &CmdBonds},   {"conns", &CmdConns},
    {"use", &CmdUse},       {"disconnect", &CmdDisconnect},
    {"unpair", &CmdUnpair}, {"kb", &CmdKb},
};

}  // namespace

int CmdBle(int argc, char **argv) {
    if (argc >= 2) {
        for (const Subcommand &sub : kSubcommands) {
            if (strcmp(argv[1], sub.name) == 0) {
                return sub.func(argc - 1, argv + 1);
            }
        }
    }
    printf("usage: ble on | off | info | pair [seconds [bg]|stop] | bonds | conns |"
           " use <#|addr|any> | disconnect | unpair <#|addr|all> | kb <text>\n");
    return 1;
}

}  // namespace console
