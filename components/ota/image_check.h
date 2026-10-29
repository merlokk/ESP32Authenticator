#pragma once

// Is a firmware .bin safe to boot on this device? A streaming check of the
// ESP-IDF app image, fed in any chunk sizes (web upload, SD card), so a bad
// file is refused before the boot slot is switched. <cstdint>/<cstddef> only:
// host-tested.
//
// Image: 24-byte header (magic 0xE9, segment count, chip id at 12, chip
// revision range at 15/17, hash-appended flag at 23), then segments (load
// address, length, data); the first one starts with esp_app_desc_t (magic
// 0xABCD5432, version, project name, build time, IDF). Then zero padding up to
// a 16-byte boundary whose last byte is the XOR of all segment data seeded
// with 0xEF, then the SHA-256 of everything before it when hash-appended
// (checked by esp_ota_end, not here: the trailer is kept for the caller).
//
// What refuses a file: not an app image, another chip, a chip or eFuse block
// revision out of the image's range (0xFFFF: no limit), another project (the
// reader's own firmware, say; unless Expect::any_project), larger than the slot, a broken segment table, a wrong checksum, a wrong length.

#include <cstddef>
#include <cstdint>

namespace ota {

inline constexpr uint16_t kChipEsp32S3 = 9;
inline constexpr size_t kHeaderSize = 24;
inline constexpr size_t kSegmentHeaderSize = 8;
inline constexpr size_t kMaxSegments = 16;
inline constexpr size_t kShaSize = 32;
inline constexpr uint32_t kAppDescMagic = 0xABCD5432;
// The descriptor up to its eFuse block revision range (ELF SHA-256 at 144).
inline constexpr size_t kAppDescRead = 180;
// Enough of the file to name it (header, segment header, descriptor).
inline constexpr size_t kIdentifySize = kHeaderSize + kSegmentHeaderSize + kAppDescRead;

enum class Verdict : uint8_t {
    kMore = 0,       // fine so far: feed more
    kOk,             // the whole image checked (Finish)
    kBadMagic,       // not an ESP app image
    kWrongChip,      // built for another chip
    kChipRevision,   // this chip's revision is out of the image's range
    kEfuseRevision,  // this chip's eFuse block revision is out of the image's range
    kBadSegments,    // segment count or lengths make no sense
    kNotApp,         // no app descriptor in the first segment
    kWrongProject,   // another firmware (project name)
    kTooLarge,       // larger than the slot
    kBadChecksum,    // the XOR checksum does not match
    kBadLength,      // bytes after the image, or the file ended early
};

const char *VerdictName(Verdict verdict);

struct Expect {
    uint16_t chip_id;        // kChipEsp32S3
    uint16_t chip_revision;  // major * 100 + minor (v0.2 = 2)
    uint16_t efuse_revision;  // the eFuse block revision, the same way
    const char *project;     // esp_app_desc_t::project_name of this firmware
    size_t max_size;         // the OTA slot's size
    bool any_project;        // another firmware too (asked for: back to the reader's)
};

struct AppInfo {
    char version[33];
    char project[33];
    char time[17];
    char date[17];
    char idf[33];
    uint8_t elf_sha256[32];
    uint16_t chip_id;
    uint16_t min_revision;  // major * 100 + minor
    uint16_t max_revision;  // 0xFFFF: no limit
    uint16_t min_efuse;
    uint16_t max_efuse;
    uint8_t segments;
    bool hash_appended;
    bool foreign;  // its project is not Expect::project
};

class ImageCheck {
   public:
    explicit ImageCheck(const Expect &expect);

    // Every byte of the file in order. kMore while it holds up, else the
    // first fault (sticky: later calls return it again).
    Verdict Feed(const uint8_t *data, size_t length);
    // After the last byte: kOk when the image was complete and nothing more.
    Verdict Finish();

    // The header and descriptor have been read and accepted.
    bool Identified() const { return identified_; }
    const AppInfo &Info() const { return info_; }
    // The image's length once the segment table is read, else 0.
    size_t ImageSize() const { return image_size_; }
    size_t Fed() const { return fed_; }
    // The SHA-256 trailer as found in the file (hash_appended).
    const uint8_t *Trailer() const { return trailer_; }

   private:
    enum class Stage : uint8_t { kHeader, kSegmentHeader, kSegmentData, kPadding, kSha, kDone };

    Verdict Fail(Verdict verdict);
    Verdict Header();
    Verdict SegmentHeader();
    Verdict Descriptor();
    void EnterAfterSegments();

    Expect expect_;
    AppInfo info_;
    Stage stage_ = Stage::kHeader;
    Verdict fault_ = Verdict::kMore;
    bool identified_ = false;
    uint8_t buffer_[kAppDescRead];  // the header, a segment header, the descriptor
    size_t buffered_ = 0;
    uint8_t segment_ = 0;
    uint32_t left_ = 0;             // of the current segment's data / the padding / the SHA
    size_t segment_offset_ = 0;     // bytes of the current segment's data seen
    uint8_t checksum_ = 0xEF;
    uint8_t padding_last_ = 0;
    size_t fed_ = 0;
    size_t image_size_ = 0;
    uint8_t trailer_[kShaSize] = {};
};

// The running app was installed by OTA and boots on trial (PENDING_VERIFY):
// it is confirmed once the boot finished and it stayed up kConfirmAfterMs;
// any reset before that rolls the bootloader back to the previous slot.
inline constexpr uint32_t kConfirmAfterMs = 30000;
bool ShouldConfirm(bool pending, bool boot_done, uint32_t uptime_ms);

// A trial boot that has not finished (BootDone) by kTrialDeadlineMs hangs
// somewhere: it is restarted, so the bootloader rolls back to the previous slot.
inline constexpr uint32_t kTrialDeadlineMs = 120000;
bool ShouldGiveUp(bool pending, bool boot_done, uint32_t uptime_ms);

}  // namespace ota
