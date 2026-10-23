#include "date_cmd.h"

#include <sys/time.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "battery.h"
#include "board_devices.h"
#include "clock_math.h"
#include "config.h"
#include "esp_timer.h"
#include "frontlight.h"
#include "keypad.h"
#include "touch.h"
#include "freertos/FreeRTOS.h"
#include "led.h"
#include "freertos/task.h"
#include "subcommand.h"
#include "timesync.h"
#include "timezone.h"

namespace console {

namespace {

void PrintAge(uint32_t ms) {
    const uint32_t s = ms / 1000;
    if (s < 120) {
        printf("%" PRIu32 " s", s);
    } else if (s < 2 * 3600) {
        printf("%" PRIu32 " min", s / 60);
    } else {
        printf("%" PRIu32 " h %02" PRIu32 " min", s / 3600, s / 60 % 60);
    }
}

void PrintOffset(int offset) {
    printf("UTC%c%02d:%02d", offset < 0 ? '-' : '+', abs(offset) / 3600, abs(offset) % 3600 / 60);
}

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool Contains(const char *text, const char *part) {  // case-insensitive
    for (; *text != '\0'; ++text) {
        size_t i = 0;
        while (part[i] != '\0' && Lower(text[i]) == Lower(part[i])) {
            ++i;
        }
        if (part[i] == '\0') {
            return true;
        }
    }
    return part[0] == '\0';
}

int Save() {
    const esp_err_t err = config::Save();
    if (err != ESP_OK) {
        printf("applied, but config.json not saved: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("saved %s\n", config::Path());
    return 0;
}

int CmdInfo(int, char **) {
    const time_t now = time(nullptr);
    const config::Time &cfg = config::Get().time;
    if (rtc::Believable(now)) {
        const rtc::DateTime u = rtc::FromUnix(now);
        printf("utc        %04u-%02u-%02u %02u:%02u:%02u\n", u.year, u.month, u.day, u.hour,
               u.minute, u.second);
        struct tm local = {};
        localtime_r(&now, &local);
        char text[32];
        strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S %a", &local);
        printf("local      %s, ", text);
        PrintOffset(tz::OffsetSeconds(now));
        printf("%s\n", tz::IsDaylightSaving(now) ? ", DST" : "");
    } else {
        printf("utc        not set ('date set' or a sync)\n");
    }
    printf("zone       %s (TZ=%s)\n", cfg.zone, tz::Current());

    rtc::Clock &clock = boards::Clock();
    int64_t clock_us = 0;
    bool valid = false;
    const esp_err_t err = clock.Read(&clock_us, &valid);
    printf("clock      %s, %s, ", clock.Name(),
           clock.KeepsTimeWhenOff() ? "keeps time when off" : "lost at power-off");
    if (err != ESP_OK) {
        printf("read failed: %s\n", esp_err_to_name(err));
    } else if (!valid) {
        printf("no time\n");
    } else {
        const int64_t drift_ms = (clock_us - rtc::SystemTimeUs()) / 1000;
        printf("%+" PRId64 " ms from the system clock\n", drift_ms);
    }

    printf("sync       ");
    if (cfg.sync_hours == 0 || cfg.server[0] == '\0') {
        printf("off (server '%s', every %u h)\n", cfg.server, static_cast<unsigned>(cfg.sync_hours));
        return 0;
    }
    printf("%s every %u h", cfg.server, static_cast<unsigned>(cfg.sync_hours));
    if (!timesync::Ready()) {
        printf(", not started\n");
        return 0;
    }
    const timesync::Status s = timesync::Get();
    if (s.syncing) {
        printf(", syncing now");
    } else if (!s.internet) {
        printf(", waiting for the internet");
    } else if (s.next_in_ms != timesync::kNever) {
        printf(", next in ");
        PrintAge(s.next_in_ms);
    }
    printf("\n");
    if (s.ever_synced) {
        printf("last sync  ");
        PrintAge(s.since_last_ms);
        printf(" ago, stepped %+d s, %u ok\n", static_cast<int>(s.last_step_seconds),
               static_cast<unsigned>(s.successes));
    } else {
        printf("last sync  never\n");
    }
    if (s.failures > 0) {
        printf("failures   %u in a row (%s)\n", static_cast<unsigned>(s.failures),
               esp_err_to_name(s.last_error));
    }
    return 0;
}

int CmdSync(int, char **) {
    const config::Time &cfg = config::Get().time;
    if (cfg.sync_hours == 0 || cfg.server[0] == '\0') {
        printf("sync is off: 'date server <host>' and 'date hours <n>'\n");
        return 1;
    }
    timesync::Status s = timesync::Get();
    if (!s.internet) {
        printf("no internet: it syncs when the internet is up\n");
        return 1;
    }
    const uint16_t ok = s.successes;
    const uint16_t failed = s.failures;
    timesync::SyncNow();
    printf("asking %s...\n", cfg.server);
    for (int i = 0; i < 80; ++i) {  // 20 s: the exchange is bounded at 15 s
        vTaskDelay(pdMS_TO_TICKS(250));
        s = timesync::Get();
        if (!s.syncing && (s.successes != ok || s.failures != failed)) {
            break;
        }
    }
    if (s.successes != ok) {
        printf("synced, stepped %+d s\n", static_cast<int>(s.last_step_seconds));
        return CmdInfo(0, nullptr);
    }
    printf("no time: %s\n", s.failures != failed ? esp_err_to_name(s.last_error) : "still waiting");
    return 1;
}

int CmdSet(int argc, char **argv) {
    const bool utc = argc == 4 && strcmp(argv[1], "utc") == 0;
    if (argc != (utc ? 4 : 3)) {
        printf("usage: date set [utc] <YYYY-MM-DD> <HH:MM:SS>   (local time without 'utc')\n");
        return 1;
    }
    const char *date = argv[utc ? 2 : 1];
    const char *clock_text = argv[utc ? 3 : 2];
    unsigned year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    char extra = 0;
    if (sscanf(date, "%4u-%2u-%2u%c", &year, &month, &day, &extra) != 3 ||
        sscanf(clock_text, "%2u:%2u:%2u%c", &hour, &minute, &second, &extra) != 3) {
        printf("could not parse '%s %s': expected YYYY-MM-DD HH:MM:SS\n", date, clock_text);
        return 1;
    }
    const rtc::DateTime t = {static_cast<uint16_t>(year), static_cast<uint8_t>(month),
                             static_cast<uint8_t>(day),  static_cast<uint8_t>(hour),
                             static_cast<uint8_t>(minute), static_cast<uint8_t>(second)};
    if (!rtc::Valid(t)) {
        printf("not a date\n");
        return 1;
    }
    time_t seconds = 0;
    if (utc) {
        seconds = static_cast<time_t>(rtc::ToUnix(t));
    } else {
        struct tm local = {};
        local.tm_year = t.year - 1900;
        local.tm_mon = t.month - 1;
        local.tm_mday = t.day;
        local.tm_hour = t.hour;
        local.tm_min = t.minute;
        local.tm_sec = t.second;
        local.tm_isdst = -1;  // libc decides from the zone
        seconds = mktime(&local);
    }
    if (!rtc::Believable(seconds)) {
        printf("outside %d..%d: not set\n", rtc::kEarliestYear, rtc::kLatestYear);
        return 1;
    }
    const timeval tv = {.tv_sec = seconds, .tv_usec = 0};
    settimeofday(&tv, nullptr);
    const esp_err_t err = rtc::StoreSystemTime(boards::Clock());
    if (err != ESP_OK) {
        printf("system clock set, %s not: %s\n", boards::Clock().Name(), esp_err_to_name(err));
    }
    return CmdInfo(0, nullptr);
}

int CmdZone(int argc, char **argv) {
    config::Time &cfg = config::Get().time;
    if (argc < 2) {
        printf("zone       %s (TZ=%s), ", cfg.zone, tz::Current());
        PrintOffset(tz::OffsetSeconds());
        printf("%s\n", tz::IsDaylightSaving() ? ", DST now" : "");
        return 0;
    }
    const char *zone = argv[1];
    if (!config::ValidZone(zone)) {
        printf("'%s' is neither a zone ('date zones') nor a POSIX TZ rule\n", zone);
        return 1;
    }
    // A raw rule a table zone shares is stored under the zone's name.
    const char *name = tz::Lookup(zone) == nullptr ? tz::NameFor(zone) : nullptr;
    snprintf(cfg.zone, sizeof cfg.zone, "%s", name != nullptr ? name : zone);
    tz::ApplyZone(cfg.zone);
    printf("zone       %s (TZ=%s), ", cfg.zone, tz::Current());
    PrintOffset(tz::OffsetSeconds());
    printf("%s\n", tz::IsDaylightSaving() ? ", DST now" : "");
    return Save();
}

int CmdZones(int argc, char **argv) {
    const char *filter = argc >= 2 ? argv[1] : nullptr;
    size_t shown = 0;
    for (size_t i = 0; i < tz::Count(); ++i) {
        const tz::Zone &zone = tz::At(i);
        if (filter != nullptr && !Contains(zone.name, filter)) {
            continue;
        }
        printf("%-22s %s\n", zone.name, zone.posix);
        ++shown;
    }
    printf("%u of %u zones; a POSIX TZ rule is accepted too\n", static_cast<unsigned>(shown),
           static_cast<unsigned>(tz::Count()));
    return 0;
}

int CmdServer(int argc, char **argv) {
    config::Time &cfg = config::Get().time;
    if (argc < 2) {
        printf("server     %s\n", cfg.server[0] != '\0' ? cfg.server : "(none: no sync)");
        return 0;
    }
    const char *host = strcmp(argv[1], "off") == 0 ? "" : argv[1];
    if (!config::ValidHost(host)) {
        printf("'%s' is not a host name or IPv4 address\n", host);
        return 1;
    }
    snprintf(cfg.server, sizeof cfg.server, "%s", host);
    timesync::Apply();
    printf("server     %s\n", cfg.server[0] != '\0' ? cfg.server : "(none: no sync)");
    return Save();
}

int CmdHours(int argc, char **argv) {
    config::Time &cfg = config::Get().time;
    if (argc < 2) {
        printf("every      %u h%s\n", static_cast<unsigned>(cfg.sync_hours),
               cfg.sync_hours == 0 ? " (no sync)" : "");
        return 0;
    }
    char *end = nullptr;
    const long hours = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || hours < 0 || hours > config::kMaxSyncHours) {
        printf("expected 0..%u hours (0: no sync)\n", static_cast<unsigned>(config::kMaxSyncHours));
        return 1;
    }
    cfg.sync_hours = static_cast<uint8_t>(hours);
    timesync::Apply();
    printf("every      %u h%s\n", static_cast<unsigned>(cfg.sync_hours),
           cfg.sync_hours == 0 ? " (no sync)" : "");
    return Save();
}

constexpr Subcommand kDateSubcommands[] = {
    {"info", &CmdInfo},     {"sync", &CmdSync},     {"set", &CmdSet},     {"zone", &CmdZone},
    {"zones", &CmdZones},   {"server", &CmdServer}, {"hours", &CmdHours},
};

}  // namespace

int CmdDate(int argc, char **argv) {
    if (argc < 2) {
        return CmdInfo(argc, argv);
    }
    return Dispatch(kDateSubcommands, argc, argv,
                    "date [info] | sync | set [utc] <YYYY-MM-DD> <HH:MM:SS> | "
                    "zone [<name|rule>] | zones [filter] | server <host|off> | hours <0..168>");
}

int CmdBoard(int, char **) {
    boards::PrintInfo();
    return 0;
}

namespace {

struct NamedColour {
    const char *name;
    led::Rgb rgb;
};

constexpr NamedColour kColours[] = {
    {"off", led::colour::kOff},       {"white", led::colour::kWhite},
    {"red", led::colour::kRed},       {"green", led::colour::kGreen},
    {"blue", led::colour::kBlue},     {"amber", led::colour::kAmber},
    {"yellow", led::colour::kYellow}, {"cyan", led::colour::kCyan},
    {"magenta", led::colour::kMagenta},
};

constexpr led::Effect kEffects[] = {led::Effect::kSolid,     led::Effect::kBeacon,
                                    led::Effect::kSlowBlink, led::Effect::kNormBlink,
                                    led::Effect::kFastBlink, led::Effect::kBreathe};

const char *ColourName(led::Rgb rgb) {
    for (const NamedColour &c : kColours) {
        if (c.rgb == rgb) {
            return c.name;
        }
    }
    return "custom";
}

}  // namespace

int PrintLedInfo() {
    if (boards::kBoard.led == boards::LedType::kNone) {
        printf("led        none on this board\n");
        return 0;
    }
    printf("led        %s on GPIO%d", boards::Name(boards::kBoard.led),
           boards::kBoard.led_pins.data);
    if (!led::Ready()) {
        printf(", not running: the UART did not come up\n");
        return 1;
    }
    const led::Status s = led::Get();
    printf(", UART%d\n", static_cast<int>(s.port));
    printf("showing    %s (%u,%u,%u), %s%s\n", ColourName(s.colour),
           static_cast<unsigned>(s.colour.r), static_cast<unsigned>(s.colour.g),
           static_cast<unsigned>(s.colour.b), led::EffectName(s.effect),
           s.overriding ? ", for a while" : "");
    printf("ceilings   %u%% normal, %u%% idle\n", static_cast<unsigned>(s.percent),
           static_cast<unsigned>(s.idle_percent));
    printf("wire       %" PRIu32 " writes, %" PRIu32 " failed\n", s.writes, s.write_failures);
    printf("stack      %" PRIu32 " B free\n", s.stack_low_water);
    return 0;
}

// 0..100, the whole word a number.
bool ParsePercent(const char *text, uint8_t *out) {
    char *end = nullptr;
    const long v = strtol(text, &end, 10);
    if (end == text || *end != '\0' || v < 0 || v > 100) {
        return false;
    }
    *out = static_cast<uint8_t>(v);
    return true;
}

int CmdLight(int argc, char **argv) {
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "info") == 0)) {
        frontlight::PrintInfo();
        return 0;
    }
    if (!frontlight::Present()) {
        printf("this board has no frontlight\n");
        return 1;
    }
    // on / off: the state now, not saved (the light starts off).
    if (argc == 2 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0)) {
        const esp_err_t err =
            strcmp(argv[1], "on") == 0 ? frontlight::On() : frontlight::Off();
        frontlight::PrintInfo();
        return err == ESP_OK ? 0 : 1;
    }
    // A level: saved, and lit.
    config::Frontlight &cfg = config::Get().frontlight;
    uint8_t brightness = cfg.brightness;
    uint8_t warmth = cfg.warmth;
    bool ok = argc <= 3;
    if (ok && strcmp(argv[1], "warmth") == 0) {
        ok = argc == 3 && ParsePercent(argv[2], &warmth);
    } else if (ok) {
        ok = ParsePercent(argv[1], &brightness) && brightness > 0 &&
             (argc == 2 || ParsePercent(argv[2], &warmth));
    }
    if (!ok) {
        printf("usage: light [info] | light <1..100> [warmth 0..100] | light warmth <0..100> |"
               " light on | light off\n");
        return 1;
    }
    cfg.brightness = brightness;
    cfg.warmth = warmth;
    if (frontlight::SetLevel(brightness, warmth) != ESP_OK || frontlight::On() != ESP_OK) {
        return 1;
    }
    frontlight::PrintInfo();
    return Save();
}

namespace {

// One frame: status, then each contact raw and decoded both ways (the
// coordinates at byte 0, as on the X4 Pro, or after a track id at byte 1).
void PrintFrame(const touch::Snapshot &s, int64_t at_ms) {
    printf("%6lld ms  status 0x%02x  contacts %u%s\n", at_ms, s.status,
           static_cast<unsigned>(s.status & touch::gt911::kStatusCount),
           (s.status & touch::gt911::kStatusKey) != 0 ? "  home" : "");
    for (uint8_t i = 0; i < s.count; ++i) {
        const uint8_t *r = s.raw + i * touch::gt911::kPointSize;
        const touch::gt911::Point at0 = touch::gt911::DecodePoint(r);
        const touch::gt911::Point at1 = touch::gt911::DecodePoint(r + 1);
        printf("           #%u  %02x %02x %02x %02x %02x %02x %02x %02x  "
               "@0: x %u y %u size %u  @1: id %u x %u y %u\n",
               static_cast<unsigned>(i), r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], at0.x,
               at0.y, at0.size, static_cast<unsigned>(r[0]), at1.x, at1.y);
    }
}

}  // namespace

int CmdTouch(int argc, char **argv) {
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "info") == 0)) {
        touch::PrintInfo();
        return 0;
    }
    if (strcmp(argv[1], "watch") != 0 || argc > 3) {
        printf("usage: touch [info] | touch watch [seconds 1..120]\n");
        return 1;
    }
    if (!touch::Present()) {
        touch::PrintInfo();
        return 1;
    }
    long seconds = 10;
    if (argc == 3) {
        char *end = nullptr;
        seconds = strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || seconds < 1 || seconds > 120) {
            printf("seconds: 1..120\n");
            return 1;
        }
    }
    // The keypad task reads and clears the frames; this shows each new one.
    printf("touch the panel or Home (%ld s)\n", seconds);
    const int64_t start_ms = esp_timer_get_time() / 1000;
    const int64_t end_ms = start_ms + seconds * 1000;
    uint32_t seen = touch::GetSnapshot().frames;
    const uint32_t first = seen;
    while (esp_timer_get_time() / 1000 < end_ms) {
        vTaskDelay(pdMS_TO_TICKS(20));
        const touch::Snapshot s = touch::GetSnapshot();
        if (s.frames != seen) {
            if (s.frames - seen > 1) {
                printf("           (%lu frame(s) between)\n",
                       static_cast<unsigned long>(s.frames - seen - 1));
            }
            seen = s.frames;
            PrintFrame(s, esp_timer_get_time() / 1000 - start_ms);
        }
    }
    printf("%lu new frame(s)\n", static_cast<unsigned long>(seen - first));
    return 0;
}

int CmdKeys(int argc, char **argv) {
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "info") == 0)) {
        keypad::PrintInfo();
        return 0;
    }
    if (strcmp(argv[1], "watch") != 0 || argc > 3) {
        printf("usage: keys [info] | keys watch [seconds 1..120]\n");
        return 1;
    }
    long seconds = 10;
    if (argc == 3) {
        char *end = nullptr;
        seconds = strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || seconds < 1 || seconds > 120) {
            printf("seconds: 1..120\n");
            return 1;
        }
    }
    // The queue is for the UI later; here it shows what happens from now on.
    keypad::Flush();
    printf("press the buttons (%ld s)\n", seconds);
    const int64_t start_ms = esp_timer_get_time() / 1000;
    const int64_t end_ms = start_ms + seconds * 1000;
    unsigned seen = 0;
    buttons::KeyEvent e;
    while (esp_timer_get_time() / 1000 < end_ms) {
        if (!keypad::Next(&e, 100)) {
            continue;
        }
        ++seen;
        printf("%6lld ms  %-6s %s", esp_timer_get_time() / 1000 - start_ms,
               boards::Name(static_cast<boards::ButtonId>(e.key)), buttons::Name(e.kind));
        if (e.kind == buttons::Kind::kClick) {
            printf(" x%u", static_cast<unsigned>(e.count));
        }
        printf("\n");
    }
    printf("%u event(s)\n", seen);
    return 0;
}

int CmdBattery(int argc, char **argv) {
    if (argc == 1 || strcmp(argv[1], "info") == 0) {
        battery::PrintInfo();
        return battery::GaugeReady() ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "regs") == 0) {
        battery::PrintRegs();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "init") == 0) {
        const esp_err_t err = battery::Reinit();
        battery::PrintInfo();
        return err == ESP_OK ? 0 : 1;
    }
    printf("usage: battery [info | regs | init]\n");
    return 1;
}

int CmdLed(int argc, char **argv) {
    if (argc == 1 || strcmp(argv[1], "info") == 0) {
        return PrintLedInfo();
    }
    if (boards::kBoard.led == boards::LedType::kNone) {
        printf("this board has no LED\n");
        return 1;
    }
    if (!led::Ready()) {
        printf("led        not running: the UART did not come up\n");
        return 1;
    }
    if (strcmp(argv[1], "brightness") == 0) {
        config::Led &cfg = config::Get().led;
        if (argc == 2) {
            printf("brightness %u%%, idle %u%%\n", static_cast<unsigned>(cfg.percent),
                   static_cast<unsigned>(cfg.idle_percent));
            return 0;
        }
        char *end = nullptr;
        const long percent = strtol(argv[2], &end, 10);
        bool ok = end != argv[2] && *end == '\0' && percent >= 0 && percent <= 100 && argc <= 4;
        long idle = cfg.idle_percent;
        if (ok && argc == 4) {
            idle = strtol(argv[3], &end, 10);
            ok = end != argv[3] && *end == '\0' && idle >= 0 && idle <= 100;
        }
        if (!ok) {
            printf("usage: led brightness [<0..100> [idle 0..100]]\n");
            return 1;
        }
        cfg.percent = static_cast<uint8_t>(percent);
        cfg.idle_percent = static_cast<uint8_t>(idle);
        boards::SetLedBrightness(cfg.percent, cfg.idle_percent);
        printf("brightness %u%%, idle %u%%\n", static_cast<unsigned>(cfg.percent),
               static_cast<unsigned>(cfg.idle_percent));
        return Save();
    }
    const NamedColour *colour = nullptr;
    for (const NamedColour &c : kColours) {
        if (strcmp(argv[1], c.name) == 0) {
            colour = &c;
        }
    }
    led::Effect effect = led::Effect::kSolid;
    bool effect_ok = argc < 3;
    for (led::Effect e : kEffects) {
        if (argc >= 3 && strcmp(argv[2], led::EffectName(e)) == 0) {
            effect = e;
            effect_ok = true;
        }
    }
    const long seconds = argc >= 4 ? strtol(argv[3], nullptr, 10) : 5;
    if (colour == nullptr || !effect_ok || seconds < 1 || seconds > 600 || argc > 4) {
        printf("usage: led <colour> [effect] [seconds 1..600]\n");
        printf("       led brightness [<0..100> [idle 0..100]]\n");
        printf("  colours: off white red green blue amber yellow cyan magenta\n");
        printf("  effects: solid beacon slow blink fast breathe\n");
        return 1;
    }
    led::SetFor(colour->rgb, effect, static_cast<uint32_t>(seconds) * 1000);
    printf("%s, %s, for %ld s\n", colour->name, led::EffectName(effect), seconds);
    return 0;
}

void PrintDateInfo() { CmdInfo(0, nullptr); }

}  // namespace console
