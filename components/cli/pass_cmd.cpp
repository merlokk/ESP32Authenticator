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
            printf("'pass reset confirm%s' writes an empty store over it (records lost)\n",
                   info.has_key ? "" : " <pin>");
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

void PrintKeyId(const vault::Info &info) {
    for (uint8_t b : info.key_id) {
        printf("%02x", b);
    }
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
    if (info.has_key) {
        printf("key        HKDF(seed, PBKDF2-HMAC-SHA256(PIN)), %" PRIu32 " iterations (%" PRIu32
               " ms), id ",
               info.kdf_iterations, info.unlock_ms);
        PrintKeyId(info);
        printf("\n");
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
        printf("usage: pass add <site> [user] [password] [otp]   (otp: Base32, or "
               "base64:<Base64>)\n");
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

// The list order (the screen's): `pass move <#> <to #>`, `pass sort` (by name).
int CmdMove(int argc, char **argv) {
    const long to = argc == 3 ? strtol(argv[2], nullptr, 10) : 0;
    if (argc != 3 || to < 1) {
        printf("usage: pass move <#> <to #>\n");
        return 1;
    }
    uint32_t id = 0;
    if (!Pick(argv[1], &id)) {
        return 1;
    }
    const vault::Error e = vault::Move(id, static_cast<size_t>(to - 1));
    if (e != vault::Error::kOk) {
        return Failed(e);
    }
    printf("#%s %s is #%ld now\n", argv[1], g_label, vault::Position(id) + 1);
    return 0;
}

int CmdSort(int, char **) {
    const vault::Error e = vault::SortByName();
    if (e != vault::Error::kOk) {
        return Failed(e);
    }
    printf("sorted by site, then user\n");
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
    const int want = info.has_key ? 2 : 3;  // without the key in: the PIN too
    if (argc != want || strcmp(argv[1], "confirm") != 0) {
        if (info.state == vault::State::kOpen) {
            printf("this deletes all %u record(s)\n", static_cast<unsigned>(info.count));
        } else {
            printf("this deletes the store file and every record in it (%s now)\n",
                   vault::StateName(info.state));
        }
        if (info.has_key) {
            printf("type 'pass reset confirm' to do it\n");
        } else {
            printf("type 'pass reset confirm <pin>' to do it: the PIN of a new key "
                   "(fido.bin then needs 'fido2 reset')\n");
        }
        return 1;
    }
    const vault::Error e = vault::Reset(argc == 3 ? argv[2] : nullptr);
    if (e != vault::Error::kOk) {
        return Failed(e);
    }
    printf("all records deleted\n");
    return 0;
}

constexpr Subcommand kPassSubcommands[] = {
    {"info", &CmdInfo}, {"list", &CmdList},     {"add", &CmdAdd},     {"set", &CmdSet},
    {"delete", &CmdDelete}, {"code", &CmdCode}, {"reset", &CmdReset},
    {"move", &CmdMove},     {"sort", &CmdSort},
};

// --- pin

int CmdPinInfo() {
    const vault::Info info = vault::GetInfo();
    printf("vault      %s%s%s\n", vault::StateName(info.state), info.why ? ": " : "",
           info.why ? info.why : "");
    if (info.has_key) {
        printf("key        in (HKDF(seed, PBKDF2), %" PRIu32 " iterations, %" PRIu32 " ms), id ",
               info.kdf_iterations, info.unlock_ms);
        PrintKeyId(info);
        printf("\n");
    } else {
        printf("key        not in; enter the PIN: pin <6 digits>%s\n",
               info.file_size == 0 ? " (no store yet: the first PIN makes it)" : "");
    }
    printf("tries      %u wrong in a row of %u; ", static_cast<unsigned>(info.pin_failures),
           static_cast<unsigned>(vault::kMaxPinTries));
    if (info.lock_left_s > 0) {
        printf("locked for %u s more\n", static_cast<unsigned>(info.lock_left_s));
    } else {
        printf("then a %u-minute lock\n", static_cast<unsigned>(config::Get().pin.lock_minutes));
    }
    const uint8_t keep = config::Get().power.keep_pin_hours;
    if (keep != 0) {
        printf("sleep      the key kept for %u h of sleep (config power keep; off lock: not)\n",
               static_cast<unsigned>(keep));
    } else {
        printf("sleep      the PIN after every wake (config power keep <hours>)\n");
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
    vault::SetPolicy({static_cast<uint32_t>(cfg.lock_minutes) * 60, cfg.wipe, cfg.complexity});
    const esp_err_t err = config::Save();
    printf("wipe PIN %s%s\n", cfg.wipe[0] ? "set" : "off",
           err == ESP_OK ? ", saved" : ": config.json NOT saved");
    return err == ESP_OK ? 0 : 1;
}

// --- seed

void PrintId(const uint8_t *id, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        printf("%02x", id[i]);
    }
}

int CmdSeedInfo() {
    const vault::Info info = vault::GetInfo();
    if (info.seed_set) {
        printf("seed       set, id ");
        PrintId(info.seed_id, sizeof info.seed_id);
        printf("\n");
    } else {
        printf("seed       none ('seed set <words>' from another device, or 'seed new'; the "
               "first store makes one)\n");
    }
    static const uint8_t kNone[vault::seed::kIdSize] = {};
    if (memcmp(info.file_seed_id, kNone, sizeof kNone) != 0) {
        printf("vault.bin  seed id ");
        PrintId(info.file_seed_id, sizeof info.file_seed_id);
        printf("%s, store id ", memcmp(info.file_seed_id, info.seed_id, sizeof info.seed_id) == 0
                                    ? " (this one)"
                                    : " (ANOTHER phrase)");
        PrintId(info.store_id, sizeof info.store_id);
        printf("\n");
    }
    return 0;
}

// Static: the words do not belong on the console's stack; wiped after use.
char g_words[2 * vault::seed::kTextSize];

struct WordsCopy {
    ~WordsCopy() { vault::Wipe(g_words, sizeof g_words); }
};

void PrintWords() {
    printf("seed words (write them down; the same on every device of this store):\n  %s\n",
           g_words);
}

}  // namespace

int CmdSeed(int argc, char **argv) {
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "info") == 0)) {
        return CmdSeedInfo();
    }
    WordsCopy wipe;
    if (argc == 3 && strcmp(argv[1], "show") == 0) {
        const vault::Error e = vault::CopySeed(argv[2], g_words);
        if (e != vault::Error::kOk) {
            return Failed(e);
        }
        PrintWords();
        return 0;
    }
    if (argc >= 2 && (strcmp(argv[1], "new") == 0 || strcmp(argv[1], "set") == 0)) {
        const bool fresh = argv[1][0] == 'n';
        int at = 2;
        const char *pin = nullptr;
        if (at < argc && vault::ValidPin(argv[at])) {
            pin = argv[at++];
        }
        // The words, as separate arguments or one quoted.
        size_t n = 0;
        for (; at < argc && !fresh; ++at) {
            n += static_cast<size_t>(snprintf(g_words + n, sizeof g_words - n, "%s%s",
                                              n != 0 ? " " : "", argv[at]));
            if (n >= sizeof g_words) {
                return Failed(vault::Error::kBadSeed);
            }
        }
        if (at != argc || (!fresh && n == 0)) {
            printf("usage: seed set [<pin>] <12 words> | seed new [<pin>]\n");
            return 1;
        }
        const vault::Error e = vault::SetSeed(pin, fresh ? nullptr : g_words, g_words);
        if (e != vault::Error::kOk) {
            if (e == vault::Error::kBadPin && pin == nullptr) {
                printf("the store is open: its PIN first (seed %s <pin> ...)\n", argv[1]);
                return 1;
            }
            if (e == vault::Error::kLocked && fresh && !vault::GetInfo().has_key) {
                printf("there is a store: 'seed set <its words>', or 'pin <pin>' and then "
                       "'seed new <pin>'\n");
                return 1;
            }
            return Failed(e);
        }
        const bool open = vault::GetInfo().state == vault::State::kOpen;
        printf("seed phrase %s%s\n", fresh ? "made (hardware RNG)" : "set",
               open ? ": a new key, vault.bin and fido.bin rewritten with it" : "");
        if (fresh) {
            PrintWords();
        }
        return CmdSeedInfo();
    }
    printf("usage: seed [info] | seed show <pin> | seed set [<pin>] <words> | seed new [<pin>]\n");
    return 1;
}

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
        printf("PIN changed: a new key; vault.bin and fido.bin rewritten with it\n");
        return 0;
    }
    if (argc == 2) {
        const bool had_store = vault::GetInfo().file_size != 0;
        const vault::Error e = vault::Unlock(argv[1]);
        if (e != vault::Error::kOk) {
            return Failed(e);
        }
        const vault::Info info = vault::GetInfo();
        printf("%s: %u record(s)\n", had_store ? "unlocked" : "new store, its key from this PIN",
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
                    "move <#> <to #> | sort | "
                    "reset [confirm [pin]]");  // pin: only without the key in
}

}  // namespace console
