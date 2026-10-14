#include "pass_cmd.h"

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "clock_math.h"
#include "codes.h"
#include "config.h"
#include "subcommand.h"
#include "vault.h"

namespace console {

namespace {

// Static: a record (~400 B) does not belong on the console's stack. Wiped
// after every use (it holds the password).
vault::Record g_record;

struct RecordCopy {
    ~RecordCopy() { vault::Wipe(&g_record, sizeof g_record); }
};

int Failed(vault::Error e) {
    if (e == vault::Error::kLocked) {
        const vault::Info info = vault::GetInfo();
        if (info.state == vault::State::kRefused) {
            printf("the password store is off: %s\n", info.why);
            printf("'pass reset confirm <pin>' writes an empty store over it (records lost)\n");
            return 1;
        }
    }
    const vault::Info info = vault::GetInfo();
    if (e == vault::Error::kWrongPin) {
        printf("wrong PIN: %u of %u tries left before a %u-minute lock\n",
               static_cast<unsigned>(vault::kMaxPinTries - info.pin_failures),
               static_cast<unsigned>(vault::kMaxPinTries),
               static_cast<unsigned>(config::Get().pin.lock_minutes));
        return 1;
    }
    if (e == vault::Error::kPinLocked) {
        printf("too many wrong PINs: PIN entry locked for %u s more\n",
               static_cast<unsigned>(info.lock_left_s));
        return 1;
    }
    printf("%s%s\n", vault::Describe(e),
           e == vault::Error::kLocked && info.state == vault::State::kLocked
               ? ": enter the PIN ('pin <6 digits>')"
               : "");
    return 1;
}

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

int CmdInfo(int, char **) {
    const vault::Info info = vault::GetInfo();
    printf("state      %s%s%s\n", vault::StateName(info.state), info.why ? ": " : "",
           info.why ? info.why : "");
    if (info.state == vault::State::kOff) {
        return 0;
    }
    printf("records    %u of %u\n", static_cast<unsigned>(info.count),
           static_cast<unsigned>(info.capacity));
    printf("file       %s, %u bytes, AES-256-GCM\n", info.path,
           static_cast<unsigned>(info.file_size));
    if (info.state == vault::State::kOpen) {
        printf("key        PBKDF2-HMAC-SHA256 of the PIN, %" PRIu32 " iterations (%" PRIu32
               " ms)\n",
               info.kdf_iterations, info.unlock_ms);
    }
    printf("version    %" PRIu32 " (highest seen here %" PRIu32 "), changed ", info.version,
           info.kept_version);
    PrintTime(info.modified_utc);
    printf("\n");
    return 0;
}

bool ContainsNoCase(const char *text, const char *part) {
    const size_t n = strlen(part);
    for (; *text != '\0'; ++text) {
        if (strncasecmp(text, part, n) == 0) {
            return true;
        }
    }
    return n == 0;
}

int CmdList(int argc, char **argv) {
    if (argc > 2) {
        printf("usage: pass list [filter]\n");
        return 1;
    }
    const char *filter = argc == 2 ? argv[1] : "";
    const vault::Info info = vault::GetInfo();
    if (info.state != vault::State::kOpen) {
        return Failed(vault::Error::kLocked);
    }
    RecordCopy wipe;
    size_t shown = 0;
    for (size_t i = 0; vault::Get(i, &g_record) == vault::Error::kOk; ++i) {
        if (!ContainsNoCase(g_record.site, filter) && !ContainsNoCase(g_record.user, filter)) {
            continue;
        }
        if (shown++ == 0) {
            printf("  #  site / user                                password  totp\n");
        }
        char who[vault::kSiteMax + vault::kUserMax + 4];
        snprintf(who, sizeof who, "%s%s%s", g_record.site, g_record.user[0] ? " / " : "",
                 g_record.user);
        printf("%3u  %-42s %-8s  %s\n", static_cast<unsigned>(i + 1), who,
               g_record.password[0] ? "set" : "-", g_record.otp[0] ? "yes" : "-");
    }
    if (shown == 0) {
        printf(info.count == 0 ? "no records; 'pass add <site> [user] [password] [otp]'\n"
                               : "nothing matches\n");
    } else {
        printf("%u of %u record(s)\n", static_cast<unsigned>(shown),
               static_cast<unsigned>(info.count));
    }
    return 0;
}

// "site / user" of the last record picked (no secrets).
char g_label[vault::kSiteMax + vault::kUserMax + 4];

void Label(const vault::Record &r) {
    snprintf(g_label, sizeof g_label, "%s%s%s", r.site, r.user[0] ? " / " : "", r.user);
}

// A record by its list number; false (and a message) if none. Sets g_label.
bool Pick(const char *number, uint32_t *id) {
    char *end = nullptr;
    const long n = strtol(number, &end, 10);
    if (end == number || *end != '\0' || n < 1) {
        printf("'%s' is not a record number ('pass list')\n", number);
        return false;
    }
    RecordCopy wipe;
    const vault::Error e = vault::Get(static_cast<size_t>(n - 1), &g_record);
    if (e != vault::Error::kOk) {
        if (e == vault::Error::kNotFound) {
            printf("no record #%ld ('pass list')\n", n);
        } else {
            Failed(e);
        }
        return false;
    }
    *id = g_record.id;
    Label(g_record);
    return true;
}

int CmdAdd(int argc, char **argv) {
    if (argc < 2 || argc > 5) {
        printf("usage: pass add <site> [user] [password] [otp]\n");
        return 1;
    }
    uint32_t id = 0;
    const vault::Error e = vault::Add(argv[1], argc > 2 ? argv[2] : "", argc > 3 ? argv[3] : "",
                                      argc > 4 ? argv[4] : "", &id);
    if (e != vault::Error::kOk) {
        return Failed(e);
    }
    printf("#%ld %s%s%s added\n", vault::Position(id) + 1, argv[1],
           argc > 2 && argv[2][0] ? " / " : "", argc > 2 ? argv[2] : "");
    return 0;
}

int CmdSet(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        printf("usage: pass set <#> <site|user|password|otp> [value]   (no value: clear)\n");
        return 1;
    }
    static const struct {
        const char *name;
        vault::Field field;
    } kFields[] = {{"site", vault::Field::kSite},
                   {"user", vault::Field::kUser},
                   {"password", vault::Field::kPassword},
                   {"otp", vault::Field::kOtp}};
    const vault::Field *field = nullptr;
    for (const auto &f : kFields) {
        if (strcmp(argv[2], f.name) == 0) {
            field = &f.field;
        }
    }
    if (field == nullptr) {
        printf("field: site, user, password or otp\n");
        return 1;
    }
    uint32_t id = 0;
    if (!Pick(argv[1], &id)) {
        return 1;
    }
    const vault::Error e = vault::Set(id, *field, argc == 4 ? argv[3] : "");
    if (e != vault::Error::kOk) {
        return Failed(e);
    }
    // Site and user changes re-sort: say where the record went.
    const long at = vault::Position(id);
    RecordCopy wipe;
    if (at >= 0 && vault::Get(static_cast<size_t>(at), &g_record) == vault::Error::kOk) {
        Label(g_record);
    }
    printf("#%ld %s: %s %s\n", at + 1, g_label, argv[2], argc == 4 ? "changed" : "cleared");
    return 0;
}

int CmdDelete(int argc, char **argv) {
    if (argc != 2) {
        printf("usage: pass delete <#>\n");
        return 1;
    }
    uint32_t id = 0;
    if (!Pick(argv[1], &id)) {
        return 1;
    }
    const vault::Error e = vault::Remove(id);
    if (e != vault::Error::kOk) {
        return Failed(e);
    }
    printf("deleted #%s %s\n", argv[1], g_label);
    return 0;
}

int CmdCode(int argc, char **argv) {
    if (argc != 2) {
        printf("usage: pass code <#>\n");
        return 1;
    }
    const long n = strtol(argv[1], nullptr, 10);
    RecordCopy wipe;
    const vault::Error e = n >= 1 ? vault::Get(static_cast<size_t>(n - 1), &g_record)
                                  : vault::Error::kNotFound;
    if (e != vault::Error::kOk) {
        return Failed(e);
    }
    if (g_record.otp[0] == '\0') {
        printf("#%ld has no TOTP secret\n", n);
        return 1;
    }
    const time_t now = time(nullptr);
    if (!rtc::Believable(now)) {
        printf("no time: 'date set' or a sync first\n");
        return 1;
    }
    totp::Codes codes = {};
    if (totp::Generate(g_record.otp, now, &codes) != ESP_OK) {
        printf("totp failed\n");
        return 1;
    }
    printf("%s%s%s: %06lu (%u s left; previous %06lu, next %06lu)\n", g_record.site,
           g_record.user[0] ? " / " : "", g_record.user, static_cast<unsigned long>(codes.code[1]),
           static_cast<unsigned>(codes.windows.remaining),
           static_cast<unsigned long>(codes.code[0]), static_cast<unsigned long>(codes.code[2]));
    return 0;
}

int CmdReset(int argc, char **argv) {
    const vault::Info info = vault::GetInfo();
    const bool open = info.state == vault::State::kOpen;
    if (argc < 2 || argc > 3 || strcmp(argv[1], "confirm") != 0 || (!open && argc != 3)) {
        if (open) {
            printf("this deletes all %u record(s)\n", static_cast<unsigned>(info.count));
            printf("type 'pass reset confirm [new pin]' to do it\n");
        } else {
            printf("this deletes the store file and every record in it%s\n",
                   info.state == vault::State::kRefused ? " (refused now)" : " (locked now)");
            printf("type 'pass reset confirm <pin>' to do it: the PIN of the new, empty store\n");
        }
        return 1;
    }
    const vault::Error e = vault::Reset(argc == 3 ? argv[2] : nullptr);
    if (e != vault::Error::kOk) {
        return Failed(e);
    }
    printf("all records deleted%s\n", argc == 3 ? "; the store has the new PIN" : "");
    return 0;
}

constexpr Subcommand kPassSubcommands[] = {
    {"info", &CmdInfo}, {"list", &CmdList},     {"add", &CmdAdd},     {"set", &CmdSet},
    {"delete", &CmdDelete}, {"code", &CmdCode}, {"reset", &CmdReset},
};

// --- pin

int CmdPinInfo() {
    const vault::Info info = vault::GetInfo();
    printf("vault      %s%s%s\n", vault::StateName(info.state), info.why ? ": " : "",
           info.why ? info.why : "");
    if (info.state == vault::State::kOpen) {
        printf("key        PBKDF2-HMAC-SHA256, %" PRIu32 " iterations, %" PRIu32 " ms\n",
               info.kdf_iterations, info.unlock_ms);
    } else if (info.state == vault::State::kLocked) {
        printf("enter it: pin <6 digits>\n");
    }
    printf("tries      %u wrong in a row of %u; ", static_cast<unsigned>(info.pin_failures),
           static_cast<unsigned>(vault::kMaxPinTries));
    if (info.lock_left_s > 0) {
        printf("locked for %u s more\n", static_cast<unsigned>(info.lock_left_s));
    } else {
        printf("then a %u-minute lock\n", static_cast<unsigned>(config::Get().pin.lock_minutes));
    }
    printf("wipe PIN   %s\n", info.wipe_set ? "set (erases the store and FIDO2 when entered)"
                                             : "not set ('pin wipe set <pin>')");
    return 0;
}

// pin wipe set <pin> | pin wipe off: config.json `pin.wipe`, saved at once.
int CmdPinWipe(int argc, char **argv) {
    config::Pin &cfg = config::Get().pin;
    if (argc == 3 && strcmp(argv[2], "off") == 0) {
        cfg.wipe[0] = '\0';
    } else if (argc == 4 && strcmp(argv[2], "set") == 0) {
        char text[vault::kWipeTextSize];
        const vault::Error e = vault::MakeWipe(argv[3], text);
        if (e != vault::Error::kOk) {
            return Failed(e);
        }
        snprintf(cfg.wipe, sizeof cfg.wipe, "%s", text);
    } else {
        printf("usage: pin wipe set <pin> | pin wipe off\n");
        return 1;
    }
    vault::SetPolicy({static_cast<uint32_t>(cfg.lock_minutes) * 60, cfg.wipe});
    const esp_err_t err = config::Save();
    printf("wipe PIN %s%s\n", cfg.wipe[0] ? "set" : "off",
           err == ESP_OK ? ", saved" : ": config.json NOT saved");
    return err == ESP_OK ? 0 : 1;
}

}  // namespace

int CmdPin(int argc, char **argv) {
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "info") == 0)) {
        return CmdPinInfo();
    }
    if (argc == 2 && strcmp(argv[1], "lock") == 0) {
        vault::Lock();
        printf("locked: key and records wiped from memory\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "wipe") == 0) {
        return CmdPinWipe(argc, argv);
    }
    if (argc == 4 && strcmp(argv[1], "change") == 0) {
        const vault::Error e = vault::ChangePin(argv[2], argv[3]);
        if (e != vault::Error::kOk) {
            return Failed(e);
        }
        printf("PIN changed\n");
        return 0;
    }
    if (argc == 2) {
        const bool had_store = vault::GetInfo().file_size != 0;
        const vault::Error e = vault::Unlock(argv[1]);
        if (e != vault::Error::kOk) {
            return Failed(e);
        }
        const vault::Info info = vault::GetInfo();
        printf("%s: %u record(s)\n", had_store ? "unlocked" : "new store made with this PIN",
               static_cast<unsigned>(info.count));
        return 0;
    }
    printf("usage: pin [info] | pin <6 digits> | pin change <old> <new> | pin lock | "
           "pin wipe set <pin> | pin wipe off\n");
    return 1;
}

void PrintPassInfo() { CmdInfo(0, nullptr); }

int CmdPass(int argc, char **argv) {
    return Dispatch(kPassSubcommands, argc, argv,
                    "pass info | list [filter] | add <site> [user] [password] [otp] | "
                    "set <#> <site|user|password|otp> [value] | delete <#> | code <#> | "
                    "reset [confirm [pin]]");
}

}  // namespace console
