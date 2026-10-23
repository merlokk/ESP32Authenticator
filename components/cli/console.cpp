#include "console.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "battery.h"
#include "ble.h"
#include "ble_cmd.h"
#include "board_devices.h"
#include "date_cmd.h"
#include "totp_cmd.h"
#include "ui.h"
#include "fido_cmd.h"
#include "pass_cmd.h"
#include "nvs_cmd.h"
#include "wifi_cmd.h"
#include "files.h"
#include "hardware.h"
#include "sd_card.h"
#include "sd_cmd.h"
#include "web_server.h"
#include "linenoise/linenoise.h"

namespace console {

namespace {

constexpr const char *TAG = "cli";

// Commands kept for the up-arrow (esp_console's default).
constexpr int kHistoryLength = 32;

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
    linenoiseSetDumbMode(!answered);
    if (answered) {
        printf("the terminal answered: line editing and history are on\n");
    } else {
        printf("no answer, staying on plain lines ('term smart' forces it on)\n");
    }
    return 0;
}

const esp_console_cmd_t kCommands[] = {
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
                "disconnect | unpair <#|addr|all> | "
                "kb <text>",
        .hint = "<on|off|info|pair|bonds|conns|use|disconnect|unpair|kb> [args]",
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
                "delete <#> | code <#> (TOTP) | reset [confirm [pin]]",
        .hint = "<info|list|add|set|delete|code|reset> [args]",
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
        .help = "microSD card (read-only): [info] card, size, clock, FAT free | ls [dir]",
        .hint = "[info | ls [dir]]",
        .func = &CmdSd,
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
        .help = "Everything at once: version, info, board, date, battery, sd, led info, wifi info, web, ble info, fido2 info, "
                "pass info "
                "(also the site's Device status page)",
        .hint = nullptr,
        .func = &CmdDevStatus,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "config",
        .help = "config.json: show (passwords masked) | info | reload | save | "
                "test [pin <6 digits>|off | presence on|off] (development, saved)",
        .hint = "<show|info|reload|save|test>",
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
    esp_console_repl_t *repl = nullptr;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "auth>";
    repl_config.max_cmdline_length = 256;
    repl_config.max_history_len = kHistoryLength;

    const esp_console_dev_usb_serial_jtag_config_t dev_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_config, &repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console init failed: %s", esp_err_to_name(err));
        return err;
    }

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

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "repl did not start: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "console on USB Serial/JTAG, type 'help'");
    return ESP_OK;
}

}  // namespace console
