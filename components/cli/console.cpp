#include "console.h"

#include <cinttypes>
#include <cstdio>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "files.h"
#include "hardware.h"

namespace console {

namespace {

constexpr const char *TAG = "cli";

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

const esp_console_cmd_t kCommands[] = {
    {
        .command = "spiffs",
        .help = "SPIFFS files: ls | cat <file> | catbase64 <file> | format confirm",
        .hint = "<ls|cat|catbase64|format> [file]",
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
