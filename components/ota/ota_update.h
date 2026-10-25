#pragma once

// Firmware updates into the other OTA slot (app0/app1), from the web site's
// upload or /sdcard/firmware.bin, and the trial boot that follows.
//
// Nothing switches the boot slot until the whole image passed: ImageCheck
// (image_check.h) as it streams, before the slot is even erased, then
// esp_ota_end (SHA-256, the image as the bootloader sees it). The new app
// boots PENDING_VERIFY on the stock bootloader (rollback enabled): it is
// confirmed kConfirmAfterMs after a finished boot (BootDone); a reset, a
// panic or a watchdog before that boots the previous slot again.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "image_check.h"

namespace ota {

inline constexpr const char *kSdFile = "/sdcard/firmware.bin";

struct Hooks {
    void (*activity)();        // per written chunk (idle power-off, LED)
    void (*before_restart)();  // the BLE link dropped cleanly
    // Per written chunk, in the writer's task (`version` "" until the image
    // is named); then once at the end, `error` nullptr when installed.
    void (*progress)(const char *version, const char *source, size_t written, size_t total);
    void (*finished)(const char *error);
};
void SetHooks(const Hooks &hooks);

// app_main: first thing, and once the boot is done (starts the confirm timer
// of a trial boot).
void Boot();
void BootDone();

enum class Job : uint8_t { kIdle, kWriting, kDone, kFailed };
enum class Source : uint8_t { kNone, kWeb, kSd };
const char *JobName(Job job);
const char *SourceName(Source source);

struct Status {
    char running_slot[17];
    char running_version[33];
    char running_date[17];
    char running_time[17];
    char running_idf[33];
    const char *running_state;  // valid, pending-verify, undefined, ...
    bool trial;                 // pending-verify: waiting to be confirmed
    uint32_t confirm_in_s;      // trial: seconds left
    char next_slot[17];
    size_t next_size;
    char rolled_back[33];       // version of the last image refused at boot, "" none
    Job job;
    Source source;
    size_t written;
    size_t total;
    char error[64];
    char job_version[33];       // the image being / last written
};
void GetStatus(Status *out);

// Why an update cannot start now (busy, trial boot, no slot); nullptr when it can.
const char *Refusal();

// One update at a time; Write feeds the check and, once the image is named,
// the slot. Any error aborts (Status::error says why); End switches the boot
// slot. `total`: the expected length (0 unknown).
esp_err_t Begin(Source source, size_t total);
esp_err_t Write(const uint8_t *data, size_t length);
esp_err_t End();
void Abort(const char *why);

// /sdcard/firmware.bin: present, its size, and the check of its first bytes
// (verdict kMore: named and accepted so far).
struct SdFile {
    bool mounted;
    bool present;
    size_t size;
    Verdict verdict;
    AppInfo info;
    bool same_as_running;  // the same ELF SHA-256
};
void ReadSdFile(SdFile *out);

// Checks and writes /sdcard/firmware.bin in a task of its own, then restarts.
esp_err_t StartFromSd();

// before_restart, then esp_restart.
void Restart();

}  // namespace ota
