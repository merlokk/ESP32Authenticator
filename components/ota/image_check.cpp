#include "image_check.h"

namespace ota {
namespace {

constexpr uint8_t kImageMagic = 0xE9;
constexpr uint8_t kChecksumSeed = 0xEF;
constexpr uint16_t kUnset = 0xFFFF;

uint16_t U16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t U32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// A fixed-size descriptor field into a terminated string.
void CopyField(const uint8_t *from, size_t size, char *out) {
    size_t i = 0;
    for (; i < size && from[i] != '\0'; ++i) {
        out[i] = static_cast<char>(from[i]);
    }
    out[i] = '\0';
}

bool Same(const char *a, const char *b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

}  // namespace

const char *VerdictName(Verdict verdict) {
    switch (verdict) {
        case Verdict::kMore: return "incomplete";
        case Verdict::kOk: return "ok";
        case Verdict::kBadMagic: return "not a firmware image";
        case Verdict::kWrongChip: return "built for another chip";
        case Verdict::kChipRevision: return "chip revision out of the image's range";
        case Verdict::kEfuseRevision: return "eFuse block revision out of the image's range";
        case Verdict::kBadSegments: return "broken segment table";
        case Verdict::kNotApp: return "no app descriptor";
        case Verdict::kWrongProject: return "another firmware (project name)";
        case Verdict::kTooLarge: return "larger than the slot";
        case Verdict::kBadChecksum: return "checksum mismatch";
        case Verdict::kBadLength: return "wrong length";
    }
    return "?";
}

ImageCheck::ImageCheck(const Expect &expect) : expect_(expect), info_{} {
    checksum_ = kChecksumSeed;
}

Verdict ImageCheck::Fail(Verdict verdict) {
    fault_ = verdict;
    return verdict;
}

Verdict ImageCheck::Header() {
    const uint8_t *h = buffer_;
    if (h[0] != kImageMagic) {
        return Fail(Verdict::kBadMagic);
    }
    info_.segments = h[1];
    info_.chip_id = U16(h + 12);
    info_.min_revision = U16(h + 15);
    info_.max_revision = U16(h + 17);
    info_.hash_appended = h[23] != 0;
    if (info_.chip_id != expect_.chip_id) {
        return Fail(Verdict::kWrongChip);
    }
    if (expect_.chip_revision < info_.min_revision ||
        (info_.max_revision != kUnset && expect_.chip_revision > info_.max_revision)) {
        return Fail(Verdict::kChipRevision);
    }
    if (info_.segments == 0 || info_.segments > kMaxSegments) {
        return Fail(Verdict::kBadSegments);
    }
    return Verdict::kMore;
}

Verdict ImageCheck::SegmentHeader() {
    left_ = U32(buffer_ + 4);
    segment_offset_ = 0;
    if (left_ > expect_.max_size || fed_ + left_ > expect_.max_size) {
        return Fail(Verdict::kTooLarge);
    }
    // The descriptor opens the first segment.
    if (segment_ == 0 && left_ < kAppDescRead) {
        return Fail(Verdict::kNotApp);
    }
    return Verdict::kMore;
}

Verdict ImageCheck::Descriptor() {
    const uint8_t *d = buffer_;
    if (U32(d) != kAppDescMagic) {
        return Fail(Verdict::kNotApp);
    }
    CopyField(d + 16, 32, info_.version);
    CopyField(d + 48, 32, info_.project);
    CopyField(d + 80, 16, info_.time);
    CopyField(d + 96, 16, info_.date);
    CopyField(d + 112, 32, info_.idf);
    for (size_t i = 0; i < sizeof info_.elf_sha256; ++i) {
        info_.elf_sha256[i] = d[144 + i];
    }
    info_.min_efuse = U16(d + 176);
    info_.max_efuse = U16(d + 178);
    info_.foreign = !Same(info_.project, expect_.project);
    if (info_.foreign && !expect_.any_project) {
        return Fail(Verdict::kWrongProject);
    }
    if ((info_.min_efuse != kUnset && expect_.efuse_revision < info_.min_efuse) ||
        (info_.max_efuse != kUnset && expect_.efuse_revision > info_.max_efuse)) {
        return Fail(Verdict::kEfuseRevision);
    }
    identified_ = true;
    return Verdict::kMore;
}

void ImageCheck::EnterAfterSegments() {
    // Padding to a 16-byte boundary; its last byte is the checksum.
    const size_t pad_end = (fed_ + 16) & ~static_cast<size_t>(15);
    left_ = static_cast<uint32_t>(pad_end - fed_);
    image_size_ = pad_end + (info_.hash_appended ? kShaSize : 0);
    stage_ = Stage::kPadding;
}

Verdict ImageCheck::Feed(const uint8_t *data, size_t length) {
    if (fault_ != Verdict::kMore) {
        return fault_;
    }
    size_t at = 0;
    while (at < length) {
        const size_t avail = length - at;
        switch (stage_) {
            case Stage::kHeader:
            case Stage::kSegmentHeader: {
                const size_t want =
                    (stage_ == Stage::kHeader ? kHeaderSize : kSegmentHeaderSize) - buffered_;
                const size_t n = avail < want ? avail : want;
                for (size_t i = 0; i < n; ++i) {
                    buffer_[buffered_++] = data[at + i];
                }
                at += n;
                fed_ += n;
                if (n < want) {
                    break;
                }
                buffered_ = 0;
                if (stage_ == Stage::kHeader) {
                    if (Header() != Verdict::kMore) {
                        return fault_;
                    }
                    stage_ = Stage::kSegmentHeader;
                } else {
                    if (SegmentHeader() != Verdict::kMore) {
                        return fault_;
                    }
                    stage_ = Stage::kSegmentData;
                    if (left_ == 0) {  // an empty segment
                        if (++segment_ == info_.segments) {
                            EnterAfterSegments();
                        } else {
                            stage_ = Stage::kSegmentHeader;
                        }
                    }
                }
                break;
            }
            case Stage::kSegmentData: {
                const size_t n = avail < left_ ? avail : left_;
                for (size_t i = 0; i < n; ++i) {
                    const uint8_t byte = data[at + i];
                    checksum_ ^= byte;
                    if (segment_ == 0 && segment_offset_ + i < kAppDescRead) {
                        buffer_[segment_offset_ + i] = byte;
                    }
                }
                const bool descriptor_done =
                    segment_ == 0 && segment_offset_ < kAppDescRead && segment_offset_ + n >= kAppDescRead;
                segment_offset_ += n;
                left_ -= static_cast<uint32_t>(n);
                at += n;
                fed_ += n;
                if (descriptor_done && Descriptor() != Verdict::kMore) {
                    return fault_;
                }
                if (left_ == 0) {
                    if (++segment_ == info_.segments) {
                        EnterAfterSegments();
                        if (image_size_ > expect_.max_size) {
                            return Fail(Verdict::kTooLarge);
                        }
                    } else {
                        stage_ = Stage::kSegmentHeader;
                    }
                }
                break;
            }
            case Stage::kPadding: {
                const size_t n = avail < left_ ? avail : left_;
                padding_last_ = data[at + n - 1];
                left_ -= static_cast<uint32_t>(n);
                at += n;
                fed_ += n;
                if (left_ == 0) {
                    if (padding_last_ != checksum_) {
                        return Fail(Verdict::kBadChecksum);
                    }
                    if (info_.hash_appended) {
                        left_ = kShaSize;
                        stage_ = Stage::kSha;
                    } else {
                        stage_ = Stage::kDone;
                    }
                }
                break;
            }
            case Stage::kSha: {
                const size_t n = avail < left_ ? avail : left_;
                for (size_t i = 0; i < n; ++i) {
                    trailer_[kShaSize - left_ + i] = data[at + i];
                }
                left_ -= static_cast<uint32_t>(n);
                at += n;
                fed_ += n;
                if (left_ == 0) {
                    stage_ = Stage::kDone;
                }
                break;
            }
            case Stage::kDone:
                return Fail(Verdict::kBadLength);
        }
    }
    return Verdict::kMore;
}

Verdict ImageCheck::Finish() {
    if (fault_ != Verdict::kMore) {
        return fault_;
    }
    return stage_ == Stage::kDone ? Verdict::kOk : Fail(Verdict::kBadLength);
}

bool ShouldConfirm(bool pending, bool boot_done, uint32_t uptime_ms) {
    return pending && boot_done && uptime_ms >= kConfirmAfterMs;
}

bool ShouldGiveUp(bool pending, bool boot_done, uint32_t uptime_ms) {
    return pending && !boot_done && uptime_ms >= kTrialDeadlineMs;
}

}  // namespace ota
