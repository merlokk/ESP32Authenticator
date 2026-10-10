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
#include "ble.h"
#include "ble_cmd.h"
#include "wifi_cmd.h"
#include "files.h"
#include "hardware.h"
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
        .command = "wifi",
        .help = "Wi-Fi station: on | off | info | scan | networks | set <ssid> [password] | "
                "forget <ssid|all>",
        .hint = "<on|off|info|scan|networks|set|forget> [args]",
        .func = &CmdWifi,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    },
    {
        .command = "config",
        .help = "config.json: show (passwords masked) | info | reload | save",
        .hint = "<show|info|reload|save>",
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
        .help = "Print chip and IDs, flash, PSRAM, eFuse, NVS, partitions, temperature",
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
