#include "ota_update.h"

#include <sys/stat.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/efuse_hal.h"
#include "sd_card.h"

namespace ota {
namespace {

constexpr const char *TAG = "ota";
constexpr size_t kChunk = 4096;

Hooks g_hooks = {};
bool g_boot_done = false;
esp_timer_handle_t g_confirm_timer = nullptr;

// One writer at a time (web upload in the httpd task, or the SD task).
std::atomic<bool> g_busy{false};
Job g_job = Job::kIdle;
Source g_source = Source::kNone;
size_t g_written = 0;
size_t g_total = 0;
char g_error[64] = "";
char g_job_version[33] = "";

ImageCheck g_check(Expect{});
const esp_partition_t *g_slot = nullptr;
esp_ota_handle_t g_handle = 0;
bool g_begun = false;            // esp_ota_begin done (the slot is being written)
uint8_t g_held[kIdentifySize];   // fed but not yet written: the image is not named yet
size_t g_held_size = 0;

const char *StateName(esp_ota_img_states_t state) {
    switch (state) {
        case ESP_OTA_IMG_NEW: return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
        case ESP_OTA_IMG_VALID: return "valid";
        case ESP_OTA_IMG_INVALID: return "invalid";
        case ESP_OTA_IMG_ABORTED: return "aborted";
        case ESP_OTA_IMG_UNDEFINED: return "undefined";
    }
    return "?";
}

bool Trial() {
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

uint32_t UptimeMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void Confirm(void *) {
    if (!ShouldConfirm(Trial(), g_boot_done, UptimeMs())) {
        return;
    }
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "trial boot confirmed: %s", esp_err_to_name(err));
}

Expect ThisDevice() {
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
    return Expect{kChipEsp32S3, chip.revision, static_cast<uint16_t>(efuse_hal_blk_version()),
                  esp_app_get_description()->project_name, next != nullptr ? next->size : 0};
}

void Fail(const char *why) {
    snprintf(g_error, sizeof g_error, "%s", why);
    g_job = Job::kFailed;
    ESP_LOGW(TAG, "update failed: %s", why);
    if (g_hooks.finished != nullptr) {
        g_hooks.finished(g_error);
    }
}

void Release() {
    if (g_begun) {
        esp_ota_abort(g_handle);
        g_begun = false;
    }
    g_held_size = 0;
    g_busy = false;
}

esp_err_t WriteSlot(const uint8_t *data, size_t length) {
    const esp_err_t err = esp_ota_write(g_handle, data, length);
    if (err != ESP_OK) {
        Abort(esp_err_to_name(err));
    }
    return err;
}

void SdTask(void *) {
    uint8_t *buffer = static_cast<uint8_t *>(malloc(kChunk));
    FILE *file = buffer != nullptr ? fopen(kSdFile, "rb") : nullptr;
    if (file == nullptr) {
        Abort(buffer == nullptr ? "out of memory" : "cannot open firmware.bin");
    } else {
        esp_err_t err = ESP_OK;
        size_t got = 0;
        while (err == ESP_OK && (got = fread(buffer, 1, kChunk, file)) > 0) {
            err = Write(buffer, got);
        }
        if (err == ESP_OK && ferror(file)) {
            Abort("sd card read error");
            err = ESP_FAIL;
        }
        fclose(file);
        if (err == ESP_OK && End() == ESP_OK) {
            free(buffer);
            vTaskDelay(pdMS_TO_TICKS(1000));
            Restart();
        }
    }
    free(buffer);
    vTaskDelete(nullptr);
}

}  // namespace

const char *JobName(Job job) {
    switch (job) {
        case Job::kIdle: return "idle";
        case Job::kWriting: return "writing";
        case Job::kDone: return "done";
        case Job::kFailed: return "failed";
    }
    return "?";
}

const char *SourceName(Source source) {
    switch (source) {
        case Source::kNone: return "";
        case Source::kWeb: return "web";
        case Source::kSd: return "sd";
    }
    return "?";
}

void SetHooks(const Hooks &hooks) { g_hooks = hooks; }

void Boot() {
    if (Trial()) {
        ESP_LOGW(TAG, "trial boot: confirmed %u s after the boot, else rolled back at the next reset",
                 static_cast<unsigned>(kConfirmAfterMs / 1000));
    }
}

void BootDone() {
    g_boot_done = true;
    if (!Trial()) {
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = Confirm,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_confirm",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &g_confirm_timer) != ESP_OK) {
        return;  // never confirmed: the next reset rolls back (safe side)
    }
    const uint32_t up = UptimeMs();
    const uint32_t wait_ms = up >= kConfirmAfterMs ? 1 : kConfirmAfterMs - up;
    esp_timer_start_once(g_confirm_timer, static_cast<uint64_t>(wait_ms) * 1000);
}

void GetStatus(Status *out) {
    *out = {};
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(out->running_slot, sizeof out->running_slot, "%s", running->label);
    snprintf(out->running_version, sizeof out->running_version, "%s", app->version);
    snprintf(out->running_date, sizeof out->running_date, "%s", app->date);
    snprintf(out->running_time, sizeof out->running_time, "%s", app->time);
    snprintf(out->running_idf, sizeof out->running_idf, "%s", app->idf_ver);
    esp_ota_img_states_t state;
    out->running_state = esp_ota_get_state_partition(running, &state) == ESP_OK
                             ? StateName(state)
                             : "none (not by OTA)";
    out->trial = Trial();
    if (out->trial) {
        const uint32_t up = UptimeMs();
        out->confirm_in_s = up >= kConfirmAfterMs ? 0 : (kConfirmAfterMs - up + 999) / 1000;
    }
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
    if (next != nullptr) {
        snprintf(out->next_slot, sizeof out->next_slot, "%s", next->label);
        out->next_size = next->size;
    }
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t desc;
    if (invalid != nullptr && esp_ota_get_partition_description(invalid, &desc) == ESP_OK) {
        snprintf(out->rolled_back, sizeof out->rolled_back, "%s", desc.version);
    }
    out->job = g_job;
    out->source = g_source;
    out->written = g_written;
    out->total = g_total;
    snprintf(out->error, sizeof out->error, "%s", g_error);
    snprintf(out->job_version, sizeof out->job_version, "%s", g_job_version);
}

const char *Refusal() {
    if (g_busy) {
        return "an update is already running";
    }
    if (Trial()) {
        return "this firmware is on trial: wait until it is confirmed";
    }
    if (esp_ota_get_next_update_partition(nullptr) == nullptr) {
        return "no OTA slot to write";
    }
    return nullptr;
}

esp_err_t Begin(Source source, size_t total) {
    const char *refusal = Refusal();
    bool idle = false;
    if (refusal != nullptr || !g_busy.compare_exchange_strong(idle, true)) {
        return ESP_ERR_INVALID_STATE;
    }
    g_slot = esp_ota_get_next_update_partition(nullptr);
    g_check = ImageCheck(ThisDevice());
    g_begun = false;
    g_held_size = 0;
    g_source = source;
    g_written = 0;
    g_total = total;
    g_error[0] = '\0';
    g_job_version[0] = '\0';
    g_job = Job::kWriting;
    if (total > g_slot->size) {
        Abort(VerdictName(Verdict::kTooLarge));
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "update from %s into %s (%u bytes)", SourceName(source), g_slot->label,
             static_cast<unsigned>(total));
    return ESP_OK;
}

esp_err_t Write(const uint8_t *data, size_t length) {
    if (g_job != Job::kWriting) {
        return ESP_ERR_INVALID_STATE;
    }
    const Verdict verdict = g_check.Feed(data, length);
    if (verdict != Verdict::kMore) {
        Abort(VerdictName(verdict));
        return ESP_ERR_INVALID_ARG;
    }
    g_written += length;
    if (g_hooks.activity != nullptr) {
        g_hooks.activity();
    }
    if (g_hooks.progress != nullptr) {
        g_hooks.progress(g_check.Identified() ? g_check.Info().version : "", SourceName(g_source),
                         g_written, g_total);
    }
    if (!g_begun) {
        if (!g_check.Identified()) {
            // Fewer than kIdentifySize bytes so far: they fit.
            memcpy(g_held + g_held_size, data, length);
            g_held_size += length;
            return ESP_OK;
        }
        // Named and accepted: only now is the slot erased (sector by sector).
        snprintf(g_job_version, sizeof g_job_version, "%s", g_check.Info().version);
        const esp_err_t err = esp_ota_begin(g_slot, OTA_WITH_SEQUENTIAL_WRITES, &g_handle);
        if (err != ESP_OK) {
            Abort(esp_err_to_name(err));
            return err;
        }
        g_begun = true;
        if (g_held_size > 0) {
            const esp_err_t held = WriteSlot(g_held, g_held_size);
            g_held_size = 0;
            if (held != ESP_OK) {
                return held;
            }
        }
    }
    return WriteSlot(data, length);
}

esp_err_t End() {
    if (g_job != Job::kWriting) {
        return ESP_ERR_INVALID_STATE;
    }
    const Verdict verdict = g_check.Finish();
    if (verdict != Verdict::kOk || !g_begun) {
        Abort(VerdictName(verdict == Verdict::kOk ? Verdict::kBadLength : verdict));
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_ota_end(g_handle);  // the SHA-256 and the image, as the bootloader checks them
    g_begun = false;
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(g_slot);
    }
    if (err != ESP_OK) {
        Fail(err == ESP_ERR_OTA_VALIDATE_FAILED ? "image validation failed (SHA-256)"
                                                : esp_err_to_name(err));
        Release();
        return err;
    }
    g_job = Job::kDone;
    ESP_LOGW(TAG, "%s written to %s: boots on trial at the next start", g_job_version, g_slot->label);
    Release();
    if (g_hooks.finished != nullptr) {
        g_hooks.finished(nullptr);
    }
    return ESP_OK;
}

void Abort(const char *why) {
    if (g_job == Job::kWriting) {
        Fail(why);
    }
    Release();
}

void ReadSdFile(SdFile *out) {
    *out = {};
    out->mounted = sd_card::Mounted();
    struct stat st = {};
    if (!out->mounted || stat(kSdFile, &st) != 0 || !S_ISREG(st.st_mode)) {
        return;
    }
    out->present = true;
    out->size = static_cast<size_t>(st.st_size);
    // Static: the check object and the head are over a few hundred bytes.
    static ImageCheck check{Expect{}};
    static uint8_t head[kIdentifySize];
    check = ImageCheck(ThisDevice());
    FILE *file = fopen(kSdFile, "rb");
    const size_t got = file != nullptr ? fread(head, 1, sizeof head, file) : 0;
    if (file != nullptr) {
        fclose(file);
    }
    out->verdict = check.Feed(head, got);
    if (out->verdict == Verdict::kMore && !check.Identified()) {
        out->verdict = Verdict::kBadLength;  // shorter than a header and a descriptor
    }
    if (out->size > ThisDevice().max_size && out->verdict == Verdict::kMore) {
        out->verdict = Verdict::kTooLarge;
    }
    out->info = check.Info();
    out->same_as_running = out->verdict == Verdict::kMore &&
                           memcmp(out->info.elf_sha256, esp_app_get_description()->app_elf_sha256,
                                  sizeof out->info.elf_sha256) == 0;
}

esp_err_t StartFromSd() {
    struct stat st = {};
    if (!sd_card::Mounted() || stat(kSdFile, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t err = Begin(Source::kSd, static_cast<size_t>(st.st_size));
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(SdTask, "ota_sd", 4096, nullptr, 3, nullptr) != pdPASS) {
        Abort("no memory for the task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void Restart() {
    if (g_hooks.before_restart != nullptr) {
        g_hooks.before_restart();
    }
    esp_restart();
}

}  // namespace ota
