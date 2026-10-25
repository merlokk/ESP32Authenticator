#include "console.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "battery.h"
#include "config.h"
#include "ble.h"
#include "ble_cmd.h"
#include "board_devices.h"
#include "date_cmd.h"
#include "totp_cmd.h"
#include "power.h"
#include "ui.h"
#include "ui_logic.h"
#include "fido_cmd.h"
#include "pass_cmd.h"
#include "nvs_cmd.h"
#include "wifi_cmd.h"
#include "files.h"
#include "hardware.h"
#include "sd_card.h"
#include "touch.h"
#include "ota_cmd.h"
#include "sd_cmd.h"
#include "epd_cmd.h"
#include "epd.h"
#include "web_server.h"
#include "usb.h"
#include "linenoise/linenoise.h"

#include <fcntl.h>
#include <unistd.h>

namespace console {

namespace {

constexpr const char *TAG = "cli";

// Commands kept for the up-arrow (esp_console's default).
constexpr int kHistoryLength = 32;

// The command line runs in a task of its own over stdin / stdout, which
// devices/usb keeps on the port in use: esp_console's REPLs are bound to one
// device. Stack and priority as esp_console's REPL.
constexpr const char *kPrompt = "auth> ";
constexpr uint32_t kReplStack = 4096;
constexpr UBaseType_t kReplPriority = 2;
constexpr size_t kMaxCommandLine = 256;

// linenoise reads with plain blocking read()s: a probe may leave stdin
// non-blocking, or (esp_console's select-based reader linked in) a reader
// our stdin device cannot serve.
void BlockingReads() {
    linenoiseSetReadFunction(read);
    const int fd = fileno(stdin);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
}

// off [lock] [wake <seconds>]: wake is for tests (a timer wakes it too).
int CmdOff(int argc, char **argv) {
    bool lock = false;
    unsigned long wake = 0;
    for (int i = 1; i < argc; ++i) {
        char *end = nullptr;
        if (strcmp(argv[i], "lock") == 0) {
            lock = true;
        } else if (strcmp(argv[i], "wake") == 0 && i + 1 < argc &&
                   (wake = strtoul(argv[i + 1], &end, 10)) > 0 && *end == '\0' && wake <= 86400) {
            ++i;
        } else {
            printf("usage: off [lock] [wake <1..86400 s>]\n");
            return 1;
        }
    }
    const esp_err_t err = power::RequestOff(lock, static_cast<uint32_t>(wake));
    if (err != ESP_OK) {
        printf("%s\n", esp_err_to_name(err));
        return 1;
    }
    const uint8_t keep = config::Get().power.keep_pin_hours;
    printf("powering off (Power wakes it%s); ", wake != 0 ? ", and a timer" : "");
    if (lock || keep == 0) {
        printf("the PIN after the wake\n");
    } else {
        printf("no PIN for a wake within %u h\n", static_cast<unsigned>(keep));
    }
    return 0;
}

int CmdUi(int argc, char **argv) {
    config::WallpaperMode mode = config::Get().display.wallpaper;
    if ((argc == 2 || (argc == 3 && config::ParseWallpaperMode(argv[2], &mode))) &&
        strcmp(argv[1], "wallpaper") == 0) {
        const char *shown = "";
        const esp_err_t err =
            ui::PreviewWallpaper(mode == config::WallpaperMode::kGrey, &shown);
        printf("%s: %s\n", shown[0] != '\0' ? shown : "wallpaper", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    const bool click = (argc == 4 || argc == 5) && strcmp(argv[1], "click") == 0;
    if (click || (argc == 6 && strcmp(argv[1], "swipe") == 0)) {
        int v[4] = {};
        int hold = 0;
        for (int i = 0; i < argc - 2; ++i) {
            v[i] = atoi(argv[i + 2]);
        }
        if (click) {
            hold = v[2];
            v[2] = v[0];
            v[3] = v[1];
        }
        for (const int c : v) {
            if (c < 0 || hold < 0 || hold > 10000) {
                printf("usage: ui click <x> <y> [hold ms, up to 10000] | ui swipe <x0> <y0> <x1> "
                       "<y1> (0..479, 0..799)\n");
                return 1;
            }
        }
        const esp_err_t err = ui::Touch(static_cast<uint16_t>(v[0]), static_cast<uint16_t>(v[1]),
                                        static_cast<uint16_t>(v[2]), static_cast<uint16_t>(v[3]),
                                        static_cast<uint16_t>(hold));
        if (err != ESP_OK) {
            printf("%s\n", err == ESP_ERR_INVALID_ARG    ? "off the screen (0..479, 0..799)"
                           : err == ESP_ERR_INVALID_STATE ? "a finger is down"
                                                          : "no touch");
            return 1;
        }
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "key") == 0) {
        const char *keys[] = {"left", "right", "home"};  // nav::Input's order
        for (int i = 0; i < 3; ++i) {
            if (strcmp(argv[2], keys[i]) == 0) {
                ui::Navigate(static_cast<ui::nav::Input>(i));
                return 0;
            }
        }
        printf("usage: ui key left|right|home\n");
        return 1;
    }
    if (argc == 3 && strcmp(argv[1], "show") == 0) {
        const char *names[] = {"list", "settings", "fido"};  // nav::Mode's order
        for (int i = 0; i < 3; ++i) {
            if (strcmp(argv[2], names[i]) == 0) {
                ui::ShowScreen(static_cast<ui::nav::Mode>(i));
                return 0;
            }
        }
        printf("usage: ui show list|settings|fido\n");
        return 1;
    }
    if (argc >= 3 && strcmp(argv[1], "demo") == 0 && strcmp(argv[2], "fido") == 0) {
        const int n = argc == 4 ? atoi(argv[3]) : 10;
        if (n < 0 || n > 30) {
            printf("usage: ui demo fido [0..30]\n");
            return 1;
        }
        ui::SetFidoDemo(static_cast<uint8_t>(n));
        printf("%d made-up key(s) in the U2F / FIDO2 list (screen only)\n", n);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "screenshot") == 0) {
        // 48 KB: PSRAM, not the console's stack.
        constexpr size_t kBytes = ui::kScreenBytes;
        auto *shot = static_cast<uint8_t *>(
            heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (shot == nullptr || !ui::CopyScreen(shot)) {
            printf("%s\n", shot == nullptr ? "no memory" : "no display");
            heap_caps_free(shot);
            return 1;
        }
        printf("screenshot %u %u 1bit\n", static_cast<unsigned>(ui::kWidth),
               static_cast<unsigned>(ui::kHeight));
        PrintBase64Lines(shot, kBytes);
        printf("end %u bytes, crc32 %08" PRIx32 "\n", static_cast<unsigned>(kBytes),
               esp_crc32_le(0, shot, kBytes));
        heap_caps_free(shot);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "demo") == 0 && strcmp(argv[2], "prompt") == 0) {
        const int n = argc == 4 ? atoi(argv[3]) : 1;
        if (n < 1 || n > 30) {
            printf("usage: ui demo prompt [1..30]\n");
            return 1;
        }
        ui::ShowFidoPromptDemo(true, static_cast<uint8_t>(n));
        printf("the FIDO2 prompt for a made-up sign-in (%d account(s)), on the screen only "
               "('ui demo off')\n",
               n);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "demo") == 0 && strcmp(argv[2], "off") == 0) {
        ui::SetFidoDemo(0);
        ui::ShowFidoPromptDemo(false);
        printf("no made-up keys, no made-up prompt\n");
        return 0;
    }
    ui::PrintInfo();
    return 0;
}

int CmdVersion(int, char **) {
    const esp_app_desc_t *app = esp_app_get_description();
    printf("firmware   %s %s\n", app->project_name, app->version);
    printf("built      %s %s\n", app->date, app->time);
    printf("idf        %s (built with %s)\n", esp_get_idf_version(), app->idf_ver);
    printf("lvgl       %s%s\n", ui::LvglVersion(), ui::Ready() ? "" : " (not initialised)");

    char sha[17] = {};
    esp_app_get_elf_sha256(sha, sizeof sha);
    printf("elf sha256 %s\n", sha);

    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    printf("chip       %s rev %d.%d, %d core(s)\n", CONFIG_IDF_TARGET, chip.revision / 100,
           chip.revision % 100, chip.cores);

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != nullptr) {
        printf("running    %s at 0x%06" PRIx32 "\n", running->label, running->address);
    }
    return 0;
}

int CmdHwInfo(int, char **) {
    hardware::PrintInfo();
    boards::PrintClock();
    printf("[battery]\n");
    battery::PrintInfo();
    printf("[epd]\n");
    epd::PrintInfo();
    printf("[touch]\n");
    touch::PrintInfo();
    printf("[sd card]\n");
    sd_card::PrintInfo();
    printf("[led]\n");
    PrintLedInfo();
    return 0;
}

int CmdEfuse(int, char **) {
    hardware::PrintEfuseFields();
    hardware::PrintEfuseRaw();
    return 0;
}

int CmdInfo(int, char **) {
    hardware::PrintShortInfo();
    return 0;
}

void Section(const char *command) { printf("\n== %s ==\n", command); }

// Composed of the other readouts, not a second copy of them. Also served as
// /api/devstatus (web::SetDiagnostics).
void PrintDevStatus() {
    Section("version");
    CmdVersion(0, nullptr);
    Section("info");
    hardware::PrintShortInfo();
    Section("usb");
    usb::PrintInfo();
    Section("board");
    CmdBoard(0, nullptr);
    Section("date");
    PrintDateInfo();
    Section("battery");
    battery::PrintInfo();
    Section("sd");
    sd_card::PrintInfo();
    Section("led info");
    PrintLedInfo();
    Section("wifi info");
    PrintWifiInfo();
    Section("web");
    PrintWebInfo();
    Section("ble info");
    ble::PrintInfo();
    Section("fido2 info");
    PrintFidoInfo();
    Section("pass info");
    PrintPassInfo();
}

int CmdDevStatus(int, char **) {
    PrintDevStatus();
    return 0;
}

int CmdReboot(int, char **) {
    // Drop the BLE link cleanly: otherwise the host keeps it until the
    // supervision timeout and the device cannot be reached meanwhile.
    if (ble::IsOn()) {
        ble::Off();
    }
    printf("rebooting\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));  // let the line reach the host
    esp_restart();
    return 0;
}

// Line editing and up-arrow history. linenoise probes the terminal once at
// boot; on USB Serial/JTAG nobody has the port open yet, so it times out and
// stays in dumb mode. This re-probes or forces the mode. Smart mode asks for
// the cursor position before every prompt and blocks until answered, so a
// port whose other end ignores escape sequences goes silent until reset.
// Ported from approver-esp32.
int CmdTerm(int argc, char **argv) {
    if (argc > 2) {
        printf("usage: term          ask the terminal again, and follow its answer\n");
        printf("       term smart    line editing and history on, regardless\n");
        printf("       term dumb     back to plain lines\n");
        return 1;
    }
    if (argc == 2) {
        if (strcmp(argv[1], "smart") == 0) {
            linenoiseSetDumbMode(0);
            printf("line editing on, up-arrow walks the last %d commands.\n", kHistoryLength);
            printf("if the console goes silent, the terminal does not answer escape\n");
            printf("sequences; reset the board (or send ESC[24;80R, then 'term dumb').\n");
            return 0;
        }
        if (strcmp(argv[1], "dumb") == 0) {
            linenoiseSetDumbMode(1);
            printf("plain lines: no history, no editing\n");
            return 0;
        }
        printf("expected 'smart' or 'dumb', got '%s'\n", argv[1]);
        return 1;
    }
    // Bounded probe (500 ms), safe to run from any terminal.
    const bool answered = linenoiseProbe() == 0;
    BlockingReads();
    linenoiseSetDumbMode(!answered);
    if (answered) {
        printf("the terminal answered: line editing and history are on\n");
    } else {
        printf("no answer, staying on plain lines ('term smart' forces it on)\n");
    }
    return 0;
}

// usb [info] | usb mode <jtag|serial|serial+fido>: the port, switched at once
// and saved.
int CmdUsb(int argc, char **argv) {
    static_assert(static_cast<int>(usb::Mode::kSerialFido) ==
                  static_cast<int>(config::UsbMode::kSerialFido));
    if (argc == 3 && strcmp(argv[1], "mode") == 0) {
        config::UsbMode mode = config::UsbMode::kJtag;
        if (!config::ParseUsbMode(argv[2], &mode)) {
            printf("usage: usb mode jtag|serial|serial+fido\n");
            return 1;
        }
        const usb::Mode next = static_cast<usb::Mode>(mode);
        config::Get().usb.mode = mode;
        const bool saved = config::Save() == ESP_OK;
        if (next == usb::GetMode()) {
            printf("usb %s already%s\n", argv[2], saved ? "" : " (config.json not saved)");
            return 0;
        }
        printf("usb %s%s: the console moves to the new port\n", argv[2],
               saved ? " (saved)" : " (config.json not saved)");
        const esp_err_t err = usb::SetMode(next);
        if (err != ESP_OK) {
            printf("%s: %s; the port is %s\n", argv[2], esp_err_to_name(err),
                   usb::Name(usb::GetMode()));
            return 1;
        }
        return 0;
    }
    if (strcmp(argv[argc > 1 ? 1 : 0], "reader") == 0 && argc <= 3) {
        const bool off = argc == 3 && strcmp(argv[2], "off") == 0;
        if (argc == 3 && !off && strcmp(argv[2], "on") != 0) {
            printf("usage: usb reader [on|off]\n");
            return 1;
        }
        // Off: the mode before it back. On: not saved; an eject on the PC, the PC
        // gone or a restart ends it too.
        if (!off && usb::GetMode() != usb::Mode::kReader) {
            printf("the card goes to the PC as a drive, the vault locks; an eject there or "
                   "'usb reader off' ends it\n");
        }
        const esp_err_t err = off ? usb::EndReader() : usb::SetMode(usb::Mode::kReader);
        if (err != ESP_OK) {
            printf("%s\n",
                   err == ESP_ERR_NOT_FOUND ? "no microSD card mounted" : esp_err_to_name(err));
            return 1;
        }
        printf("usb %s\n", usb::Name(usb::GetMode()));
        return 0;
    }
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "info") != 0)) {
        printf("usage: usb [info] | usb mode jtag|serial|serial+fido | usb reader [on|off]\n");
        return 1;
    }
    usb::PrintInfo();
    return 0;
}

void ReplTask(void *) {
    setvbuf(stdin, nullptr, _IONBF, 0);
    // Nobody may have the port open yet: no answer leaves plain lines ('term').
    if (linenoiseProbe() != 0) {
        linenoiseSetDumbMode(1);
    }
    BlockingReads();
    printf("\nType 'help' to get the list of commands.\n");
    for (;;) {
        char *line = linenoise(kPrompt);
        if (line == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));  // the port went away: no spin
            continue;
        }
        linenoiseHistoryAdd(line);
        int ret = 0;
        const esp_err_t err = esp_console_run(line, &ret);
        if (err == ESP_ERR_NOT_FOUND) {
            printf("Unrecognized command\n");
        } else if (err == ESP_OK && ret != ESP_OK) {
            printf("Command returned non-zero error code: 0x%x (%s)\n", ret, esp_err_to_name(ret));
        } else if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {  // INVALID_ARG: an empty line
            printf("Internal error: %s\n", esp_err_to_name(err));
        }
        linenoiseFree(line);
    }
}

const esp_console_cmd_t kCommands[] = {
    {
        .command = "usb",
        .help = "The USB port: [info] | mode jtag|serial|serial+fido (the chip's Serial/JTAG, a "
                "serial port, a serial port and a FIDO2 / U2F key; at once, saved) | "
                "reader [on|off] (the microSD card as a USB drive beside the serial port, the "
                "vault locked; off: the saved mode; not saved)",
        .hint = "[info | mode <jtag|serial|serial+fido> | reader [on|off]]",
        .func = &CmdUsb,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "term",
        .help = "Line editing and up-arrow history: 'term smart' on, 'term dumb' off, "
                "'term' asks the terminal",
        .hint = "[smart|dumb]",
        .func = &CmdTerm,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "info",
        .help = "Short info: model, MACs, unique ID, flash size, CPU temperature, reset, uptime",
        .hint = nullptr,
        .func = &CmdInfo,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "reboot",
        .help = "Restart the chip (software reset)",
        .hint = nullptr,
        .func = &CmdReboot,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "ble",
        .help = "BLE: on | off | info | pair [seconds [bg]|stop] | bonds | conns | use <#|addr|any> | "
                "disconnect | unpair <#|addr|all> | boot [on|off] | name [<name>] | "
                "kb <text>",
        .hint = "<on|off|info|pair|bonds|conns|use|disconnect|unpair|boot|name|kb> [args]",
        .func = &CmdBle,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "fido2",
        .help = "FIDO2 credentials: info | list | delete <#|id> | reset [confirm]",
        .hint = "<info|list|delete|reset> [args]",
        .func = &CmdFido,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "pass",
        .help = "Passwords (encrypted vault.bin; never printed): info | list [filter] | "
                "add <site> [user] [password] [otp] | set <#> <site|user|password|otp> [value] | "
                "delete <#> | code <#> (TOTP) | move <#> <to #> | sort | reset [confirm [pin]]",
        .hint = "<info|list|add|set|delete|code|move|sort|reset> [args]",
        .func = &CmdPass,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "pin",
        .help = "Vault PIN (6 digits; the key of vault.bin): [info] | <pin> to unlock (no store: "
                "makes one; the wipe PIN erases everything) | change <old> <new> | lock | "
                "wipe set <pin> | wipe off",
        .hint = "[info | <pin> | change <old> <new> | lock | wipe set <pin> | wipe off]",
        .func = &CmdPin,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "seed",
        .help = "Seed phrase (12 BIP39 words, in the store key with the PIN; the same on synced "
                "devices): [info] | show <pin> | set [<pin>] <words> | new [<pin>] (the hardware "
                "RNG; with the store open both stores get a new key)",
        .hint = "[info | show <pin> | set [<pin>] <words> | new [<pin>]]",
        .func = &CmdSeed,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "nvs",
        .help = "NVS keys (development): ls [namespace] (names and types, never values) | "
                "rm <namespace> <key>",
        .hint = "<ls|rm> [args]",
        .func = &CmdNvs,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "wifi",
        .help = "Wi-Fi: on | off | mode <client|ap|apsta> | info | scan | "
                "ap [<ssid> [password] [channel]] | retry | networks | set <ssid> [password] | "
                "static <n> <ip> <mask> <gw> [dns1] [dns2] | static <n> off | ping | "
                "check [on|off|<ip>...] | forget <ssid|all>",
        .hint = "<on|off|mode|info|scan|ap|retry|networks|set|static|ping|check|forget> [args]",
        .func = &CmdWifi,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "web",
        .help = "Configuration site: info | on | off | auto | login <user> <password> | "
                "login off | write on|off",
        .hint = "<info|on|off|auto|login|write> [args]",
        .func = &CmdWeb,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "date",
        .help = "Clock and time zone: info | sync | set [utc] <YYYY-MM-DD> <HH:MM:SS> | "
                "zone [<name|rule>] | zones [filter] | server <host|off> | hours <0..168>",
        .hint = "[info|sync|set|zone|zones|server|hours] [args]",
        .func = &CmdDate,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "board",
        .help = "The board this firmware is built for (devices/config.h): devices and pins",
        .hint = nullptr,
        .func = &CmdBoard,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "totp",
        .help = "TOTP codes for a Base32 secret: previous, current, next 30 s window "
                "(error without a time) | test: RFC 6238 vectors",
        .hint = "<base32 secret>|test",
        .func = &CmdTotp,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "sd",
        .help = "microSD card (read-only but wallpapers): [info] card, size, clock, FAT free | "
                "ls [dir] | put <name.png> <length> <crc32> (base64 line, utils/wallpaper.py) | "
                "rm <name.png> (in /sdcard/wallpapers) | put/rm firmware.bin (the root, for ota sd)",
        .hint = "[info | ls [dir] | put <name.png> <len> <crc32> | rm <name.png>]",
        .func = &CmdSd,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "ota",
        .help = "Firmware update: [info] slots, version, trial state, last job, /sdcard/firmware.bin | "
                "sd [confirm]: its check / write it into the next slot and restart",
        .hint = "[info | sd [confirm]]",
        .func = &CmdOta,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "light",
        .help = "Frontlight: [info] | <1..100> [warmth 0..100] | warmth <0..100> (level, saved) | "
                "on | off (now; off at boot, Power x2 toggles)",
        .hint = "[info | <1..100> [warmth] | warmth <0..100> | on | off]",
        .func = &CmdLight,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "epd",
        .help = "E-ink panel: [info] controller, probe, refreshes | test [orient|corners|edges|checker|black|white|grey] "
                "[full|fast] | sleep",
        .hint = "[info | test [pattern] [full|fast] | sleep]",
        .func = &CmdEpd,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "off",
        .help = "Power off: a random PNG from /sdcard/wallpapers on the screen, deep sleep; "
                "the Power button wakes it (a reboot). Also: Power held, idle "
                "(power.idleMinutes). The vault key is kept through the sleep for "
                "power.keepPinHours; lock: not (also Power x3). wake <s>: a timer wakes it "
                "too (tests)",
        .hint = "[lock] [wake <seconds>]",
        .func = &CmdOff,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "ui",
        .help = "LVGL display: [info] size, refreshes, the last one, the ui task's free stack | "
                "wallpaper [grey|bw]: draw a random power-off wallpaper now, as display.wallpaper in "
                "config.json or as given (stays on) | demo prompt [n]: the FIDO2 prompt, made up (n accounts) | demo fido [n] / demo off: n (10) made-up keys in the "
                "U2F / FIDO2 list, on the screen only | show list|settings|fido: that screen | "
                "click <x> <y> [hold ms] / swipe <x0> <y0> <x1> <y1>: a made-up finger (portrait "
                "px) | "
                "key left|right|home: as the button | "
                "screenshot: the screen as base64 (utils/screenshot.py makes a PNG)",
        .hint = "[wallpaper [grey|bw] | show <screen> | key <key> | click <x> <y> [ms] | "
                "swipe <x0> <y0> <x1> <y1> | "
                "demo fido [n] | demo prompt [n] | demo off | screenshot]",
        .func = &CmdUi,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "touch",
        .help = "Touch controller (GT911): [info] product, firmware, size, config | watch [seconds]: "
                "each frame, contacts raw and decoded",
        .hint = "[info | watch [seconds]]",
        .func = &CmdTouch,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "keys",
        .help = "Push buttons: [info] GPIO, level, held time | watch [seconds]: events "
                "(pressed, released, long, click xN)",
        .hint = "[info | watch [seconds]]",
        .func = &CmdKeys,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "battery",
        .help = "Battery: [info] voltage, level, charging | regs: gauge registers and profile | "
                "init: check the profile again, reset the gauge",
        .hint = "[info|regs|init]",
        .func = &CmdBattery,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "led",
        .help = "The board's LED: [info] | <colour> [effect] [seconds] to show it for a while | "
                "brightness [<n> [idle]] (saved)",
        .hint = "[info | <colour> [effect] [seconds] | brightness [<n> [idle]]]",
        .func = &CmdLed,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "devstatus",
        .help = "Everything at once: version, info, usb, board, date, battery, sd, led info, wifi info, web, ble info, fido2 info, "
                "pass info "
                "(also the site's Device page)",
        .hint = nullptr,
        .func = &CmdDevStatus,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "config",
        .help = "config.json: show (passwords masked) | info | reload | save | "
                "test [pin <6 digits>|off | presence on|off] (development, saved) | "
                "wallpaper [grey|bw] (power-off wallpaper tones, saved) | "
                "power [idle <minutes>] [keep <hours>] [rail on|off] (off when idle, the PIN kept through a "
                "sleep; saved)",
        .hint = "<show|info|reload|save|test|wallpaper|power>",
        .func = &CmdConfig,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "spiffs",
        .help = "SPIFFS files: info | ls | cat <file> | catbase64 <file> | write <file> [<length> <crc32>] | rm <file> | format confirm",
        .hint = "<info|ls|cat|catbase64|write|rm|format> [file]",
        .func = &CmdSpiffs,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "efuse",
        .help = "Print every eFuse field and the raw eFuse blocks",
        .hint = nullptr,
        .func = &CmdEfuse,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "hwinfo",
        .help = "Print chip and IDs, flash, PSRAM, eFuse, NVS, partitions, temperature, clock, battery, SD card, LED",
        .hint = nullptr,
        .func = &CmdHwInfo,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "version",
        .help = "Print firmware version, build date, IDF version, chip and running slot",
        .hint = nullptr,
        .func = &CmdVersion,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
};

}  // namespace

esp_err_t Init() {
    esp_console_config_t console_config = ESP_CONSOLE_CONFIG_DEFAULT();
    console_config.max_cmdline_length = kMaxCommandLine;
    console_config.hint_color = -1;  // no log colours either
    esp_err_t err = esp_console_init(&console_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console init failed: %s", esp_err_to_name(err));
        return err;
    }
    linenoiseSetMultiLine(1);
    linenoiseSetCompletionCallback(&esp_console_get_completion);
    linenoiseSetHintsCallback(reinterpret_cast<linenoiseHintsCallback *>(&esp_console_get_hint));
    linenoiseHistorySetMaxLen(kHistoryLength);
    linenoiseSetMaxLineLen(kMaxCommandLine);

    web::SetDiagnostics(&PrintDevStatus);
    web::SetHardwareInfo([] { CmdHwInfo(0, nullptr); });

    ESP_ERROR_CHECK(esp_console_register_help_command());
    for (const esp_console_cmd_t &cmd : kCommands) {
        err = esp_console_cmd_register(&cmd);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "'%s' not registered: %s", cmd.command, esp_err_to_name(err));
            return err;
        }
    }

    if (xTaskCreate(ReplTask, "console", kReplStack, nullptr, kReplPriority, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "no console task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "console on USB (%s), type 'help'", usb::Name(usb::GetMode()));
    return ESP_OK;
}

}  // namespace console
