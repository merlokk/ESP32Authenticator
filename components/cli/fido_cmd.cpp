#include "fido_cmd.h"

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "fido_ctap.h"
#include "subcommand.h"

namespace console {

namespace {

constexpr size_t kMaxListed = 16;  // more than the store holds (10)
constexpr size_t kShortId = 4;     // bytes shown: 8 hex digits
constexpr size_t kMinPrefix = 4;   // hex digits accepted by delete

// Static: 16 records of ~240 bytes do not belong on the console's stack.
fido_ctap_credential_t g_list[kMaxListed];

int Failed(esp_err_t err, const char *what) {
    if (err == ESP_ERR_TIMEOUT) {
        printf("%s: busy, a FIDO request is running (waiting for the button?)\n", what);
    } else if (err == ESP_ERR_INVALID_STATE) {
        const char *why = fido_ctap_stopped_reason();
        if (why != nullptr) {
            printf("%s: FIDO2 is off: %s\n", what, why);
            printf("'fido2 reset confirm' replaces it with an empty store (credentials lost)\n");
        } else {
            printf("%s: the FIDO core is not running\n", what);
        }
    } else {
        printf("%s: %s\n", what, esp_err_to_name(err));
    }
    return 1;
}

// Local time, or "?" when not recorded.
void PrintTime(int64_t utc) {
    if (utc <= 0) {
        printf("?");
        return;
    }
    const time_t when = static_cast<time_t>(utc);
    struct tm local = {};
    localtime_r(&when, &local);
    char text[20];
    strftime(text, sizeof text, "%Y-%m-%d %H:%M", &local);
    printf("%s", text);
}

void PrintId(const uint8_t *id, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) {
        printf("%02x", id[i]);
    }
}

int CmdInfo(int, char **) {
    fido_ctap_info_t info;
    const esp_err_t err = fido_ctap_info(&info);
    if (err != ESP_OK) {
        return Failed(err, "info");
    }
    printf("stored     %u of %u credentials (%u resident)\n", static_cast<unsigned>(info.credentials),
           static_cast<unsigned>(info.max_credentials), static_cast<unsigned>(info.discoverable));
    printf("store      %u of %u bytes (%u%%), deleted items freed at boot\n",
           static_cast<unsigned>(info.store_used), static_cast<unsigned>(info.store_size),
           static_cast<unsigned>(info.store_size ? info.store_used * 100 / info.store_size : 0));
    if (info.persistent) {
        printf("file       /spiffs/fido.bin, %u bytes\n", static_cast<unsigned>(info.file_size));
        printf("version    %" PRIu32 " (highest seen here %" PRIu32 "), changed ", info.file_version,
               info.kept_version);
        PrintTime(info.modified_utc);
        printf("\n");
    } else {
        printf("file       none: RAM only, lost on reboot\n");
    }
    printf("pin        %s, %u attempt(s) left\n", info.pin_set ? "set" : "not set",
           static_cast<unsigned>(info.pin_retries));
    printf("counter    %" PRIu32 " (signatures, kept by reset)\n", info.sign_count);
    return 0;
}

int Load(size_t *count) {
    const esp_err_t err = fido_ctap_list(g_list, kMaxListed, count);
    return err == ESP_OK ? 0 : Failed(err, "list");
}

void PrintMac(const uint8_t *mac) {
    printf("%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// "made <local time> on <authenticator> by <host>"; parts not recorded: "?".
void PrintOrigin(const fido_ctap_origin_t &o) {
    printf("                   made ");
    PrintTime(o.created_utc);
    printf(" on ");
    if (o.has_device) {
        PrintMac(o.device);
    } else {
        printf("?");
    }
    printf(" by ");
    if (o.client.transport == FIDO_CTAP_TRANSPORT_BLE) {
        PrintMac(o.client.addr);
        printf(" (BLE, %s)", o.client.addr_type == 0 ? "public" : "random");
    } else if (o.client.transport == FIDO_CTAP_TRANSPORT_USB) {
        printf("USB");
    } else {
        printf("?");
    }
    printf("\n");
}

int CmdList(int, char **) {
    size_t count = 0;
    if (Load(&count) != 0) {
        return 1;
    }
    if (count == 0) {
        printf("no credentials\n");
        return 0;
    }
    printf(" #  id        rk   site / user\n");
    for (size_t i = 0; i < count; ++i) {
        const fido_ctap_credential_t &c = g_list[i];
        printf("%2u  ", static_cast<unsigned>(i + 1));
        PrintId(c.id, kShortId);
        printf("  %-3s  %s%s\n", c.discoverable ? "yes" : "no", c.rp_id,
               c.rp_id_truncated ? "..." : "");
        if (c.user_name[0] != '\0' || c.display_name[0] != '\0') {
            printf("                   %s", c.user_name);
            if (c.display_name[0] != '\0' && strcmp(c.display_name, c.user_name) != 0) {
                printf("%s(%s)", c.user_name[0] != '\0' ? " " : "", c.display_name);
            }
            printf("\n");
        }
        PrintOrigin(c.origin);
    }
    printf("%u credential(s); 'fido2 delete <#|id>'\n", static_cast<unsigned>(count));
    return 0;
}

bool IsHex(const char *text) {
    for (; *text != '\0'; ++text) {
        if (!isxdigit(static_cast<unsigned char>(*text))) {
            return false;
        }
    }
    return true;
}

// Does the credential id start with the hex digits in `prefix`?
bool IdStartsWith(const uint8_t *id, const char *prefix) {
    char hex[2 * FIDO_CTAP_CRED_ID_SIZE + 1];
    for (size_t i = 0; i < FIDO_CTAP_CRED_ID_SIZE; ++i) {
        snprintf(hex + 2 * i, 3, "%02x", id[i]);
    }
    for (size_t i = 0; prefix[i] != '\0'; ++i) {
        if (tolower(static_cast<unsigned char>(prefix[i])) != hex[i]) {
            return false;
        }
    }
    return true;
}

int CmdDelete(int argc, char **argv) {
    if (argc != 2) {
        printf("usage: fido2 delete <#|id>   (# or id from 'fido2 list')\n");
        return 1;
    }
    size_t count = 0;
    if (Load(&count) != 0) {
        return 1;
    }
    const char *which = argv[1];
    const size_t length = strlen(which);
    const fido_ctap_credential_t *target = nullptr;
    // Up to 2 digits is a list number; anything longer is an id prefix.
    if (length <= 2 && strspn(which, "0123456789") == length) {
        const long n = strtol(which, nullptr, 10);
        if (n < 1 || static_cast<size_t>(n) > count) {
            printf("no credential #%s ('fido2 list')\n", which);
            return 1;
        }
        target = &g_list[n - 1];
    } else {
        if (length < kMinPrefix || length > 2 * FIDO_CTAP_CRED_ID_SIZE || !IsHex(which)) {
            printf("id: %u to %u hex digits from 'fido2 list'\n",
                   static_cast<unsigned>(kMinPrefix),
                   static_cast<unsigned>(2 * FIDO_CTAP_CRED_ID_SIZE));
            return 1;
        }
        for (size_t i = 0; i < count; ++i) {
            if (IdStartsWith(g_list[i].id, which)) {
                if (target != nullptr) {
                    printf("'%s' matches more than one credential: give more digits\n", which);
                    return 1;
                }
                target = &g_list[i];
            }
        }
        if (target == nullptr) {
            printf("no credential with id %s... ('fido2 list')\n", which);
            return 1;
        }
    }
    const esp_err_t err = fido_ctap_delete(target->id);
    if (err != ESP_OK) {
        return Failed(err, "delete");
    }
    printf("deleted ");
    PrintId(target->id, kShortId);
    printf(" %s%s%s\n", target->rp_id, target->user_name[0] != '\0' ? " " : "",
           target->user_name);
    return 0;
}

int CmdReset(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "confirm") != 0) {
        fido_ctap_info_t info;
        if (fido_ctap_info(&info) == ESP_OK) {
            printf("this deletes all %u credential(s)%s; sites will no longer accept this key\n",
                   static_cast<unsigned>(info.credentials), info.pin_set ? " and the PIN" : "");
        }
        printf("type 'fido2 reset confirm' to do it\n");
        return 1;
    }
    const esp_err_t err = fido_ctap_reset();
    if (err != ESP_OK) {
        return Failed(err, "reset");
    }
    printf("all credentials and the PIN deleted\n");
    return 0;
}

constexpr Subcommand kFidoSubcommands[] = {
    {"info", &CmdInfo},
    {"list", &CmdList},
    {"delete", &CmdDelete},
    {"reset", &CmdReset},
};

}  // namespace

void PrintFidoInfo() { CmdInfo(0, nullptr); }

int CmdFido(int argc, char **argv) {
    return Dispatch(kFidoSubcommands, argc, argv,
                    "fido2 info | list | delete <#|id> | reset [confirm]");
}

}  // namespace console
