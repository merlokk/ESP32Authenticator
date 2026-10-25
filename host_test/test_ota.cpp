// Firmware image check before an OTA switch, and when a trial boot is
// confirmed (components/ota/image_check.h).

#include <cstring>
#include <vector>

#include "image_check.h"
#include "unity.h"

namespace {

using ota::Verdict;

constexpr const char *kProject = "esp32-authenticator";

void Put16(std::vector<uint8_t> &v, size_t at, uint16_t value) {
    v[at] = static_cast<uint8_t>(value);
    v[at + 1] = static_cast<uint8_t>(value >> 8);
}

void Put32(std::vector<uint8_t> &v, size_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        v[at + i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

struct Spec {
    uint8_t magic = 0xE9;
    uint16_t chip = ota::kChipEsp32S3;
    uint16_t min_rev = 0;
    uint16_t max_rev = 99;
    uint16_t min_efuse = 0;
    uint16_t max_efuse = 199;
    uint32_t desc_magic = ota::kAppDescMagic;
    const char *project = kProject;
    bool hash = true;
};

// Header, a 256-byte DROM segment opening with the descriptor, a 37-byte
// segment, padding with the checksum, a made-up SHA-256 trailer.
std::vector<uint8_t> Image(const Spec &spec = {}) {
    std::vector<uint8_t> v(ota::kHeaderSize, 0);
    v[0] = spec.magic;
    v[1] = 2;
    Put16(v, 12, spec.chip);
    Put16(v, 15, spec.min_rev);
    Put16(v, 17, spec.max_rev);
    v[23] = spec.hash ? 1 : 0;
    const uint32_t lengths[] = {256, 37};
    uint8_t checksum = 0xEF;
    for (int s = 0; s < 2; ++s) {
        const size_t head = v.size();
        v.resize(head + 8 + lengths[s], 0);
        Put32(v, head, 0x3C000000u + s);
        Put32(v, head + 4, lengths[s]);
        const size_t data = head + 8;
        for (uint32_t i = 0; i < lengths[s]; ++i) {
            v[data + i] = static_cast<uint8_t>(i * 7 + s);
        }
        if (s == 0) {
            Put32(v, data, spec.desc_magic);
            std::memset(&v[data + 16], 0, 32);
            std::memcpy(&v[data + 16], "1.2.3", 5);
            std::memset(&v[data + 48], 0, 32);
            std::memcpy(&v[data + 48], spec.project, std::strlen(spec.project));
            std::memset(&v[data + 80], 0, 64);
            std::memcpy(&v[data + 80], "12:00:00", 8);
            std::memcpy(&v[data + 96], "Oct 25 2026", 11);
            std::memcpy(&v[data + 112], "v6.0.2", 6);
            Put16(v, data + 176, spec.min_efuse);
            Put16(v, data + 178, spec.max_efuse);
        }
        for (uint32_t i = 0; i < lengths[s]; ++i) {
            checksum ^= v[data + i];
        }
    }
    const size_t pad_end = (v.size() + 16) & ~static_cast<size_t>(15);
    v.resize(pad_end, 0);
    v[pad_end - 1] = checksum;
    if (spec.hash) {
        for (int i = 0; i < 32; ++i) {
            v.push_back(static_cast<uint8_t>(0xA0 + i));
        }
    }
    return v;
}

ota::Expect Device(size_t max_size = 4096) {
    return {ota::kChipEsp32S3, 2, 100, kProject, max_size};
}

Verdict Check(const std::vector<uint8_t> &image, size_t chunk, const ota::Expect &expect = Device()) {
    ota::ImageCheck check(expect);
    for (size_t at = 0; at < image.size(); at += chunk) {
        const size_t n = image.size() - at < chunk ? image.size() - at : chunk;
        const Verdict v = check.Feed(image.data() + at, n);
        if (v != Verdict::kMore) {
            return v;
        }
    }
    return check.Finish();
}

void test_ota_good_image_any_chunks(void) {
    const std::vector<uint8_t> image = Image();
    for (size_t chunk : {size_t{1}, size_t{7}, size_t{24}, size_t{4096}}) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(Verdict::kOk), static_cast<int>(Check(image, chunk)));
    }
    Spec spec;
    spec.hash = false;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Verdict::kOk), static_cast<int>(Check(Image(spec), 5)));
}

void test_ota_info(void) {
    const std::vector<uint8_t> image = Image();
    ota::ImageCheck check(Device());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Verdict::kMore),
                          static_cast<int>(check.Feed(image.data(), ota::kIdentifySize - 1)));
    TEST_ASSERT_FALSE(check.Identified());
    check.Feed(image.data() + ota::kIdentifySize - 1, 1);
    TEST_ASSERT_TRUE(check.Identified());
    TEST_ASSERT_EQUAL_STRING("1.2.3", check.Info().version);
    TEST_ASSERT_EQUAL_STRING(kProject, check.Info().project);
    TEST_ASSERT_EQUAL_STRING("Oct 25 2026", check.Info().date);
    TEST_ASSERT_EQUAL_STRING("v6.0.2", check.Info().idf);
    TEST_ASSERT_EQUAL_size_t(0, check.ImageSize());  // known after the segments
    check.Feed(image.data() + ota::kIdentifySize, image.size() - ota::kIdentifySize);
    TEST_ASSERT_EQUAL_size_t(image.size(), check.ImageSize());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Verdict::kOk), static_cast<int>(check.Finish()));
    TEST_ASSERT_EQUAL_UINT(0xA0u, check.Trailer()[0]);
    TEST_ASSERT_EQUAL_UINT(0xBFu, check.Trailer()[31]);
}

void AssertRefused(const Spec &spec, Verdict expected) {
    TEST_ASSERT_EQUAL_STRING(ota::VerdictName(expected), ota::VerdictName(Check(Image(spec), 3)));
}

void test_ota_refuses_header(void) {
    Spec s;
    s.magic = 0x00;
    AssertRefused(s, Verdict::kBadMagic);
    s = {};
    s.chip = 5;  // ESP32-C3
    AssertRefused(s, Verdict::kWrongChip);
    s = {};
    s.min_rev = 3;  // v0.3 and up; the chip is v0.2
    AssertRefused(s, Verdict::kChipRevision);
    s = {};
    s.max_rev = 1;
    AssertRefused(s, Verdict::kChipRevision);
    s = {};
    s.max_rev = 0xFFFF;  // no limit
    AssertRefused(s, Verdict::kOk);
}

void test_ota_refuses_descriptor(void) {
    Spec s;
    s.desc_magic = 0x12345678;
    AssertRefused(s, Verdict::kNotApp);
    s = {};
    s.project = "xteink_app";  // the reader's firmware
    AssertRefused(s, Verdict::kWrongProject);
    s = {};
    s.project = "esp32-authenticator2";
    AssertRefused(s, Verdict::kWrongProject);
    s = {};
    s.min_efuse = 101;
    AssertRefused(s, Verdict::kEfuseRevision);
    s = {};
    s.min_efuse = 0xFFFF;
    s.max_efuse = 0xFFFF;
    AssertRefused(s, Verdict::kOk);
}

void test_ota_refuses_body(void) {
    std::vector<uint8_t> image = Image();
    image[250] ^= 0x01;  // a data byte past the descriptor: the checksum no longer matches
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Verdict::kBadChecksum), static_cast<int>(Check(image, 64)));

    image = Image();
    image.push_back(0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Verdict::kBadLength), static_cast<int>(Check(image, 64)));

    image = Image();
    image.pop_back();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Verdict::kBadLength), static_cast<int>(Check(image, 64)));

    image = Image();
    image[1] = 0;  // no segments
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Verdict::kBadSegments), static_cast<int>(Check(image, 64)));
    image[1] = 17;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Verdict::kBadSegments), static_cast<int>(Check(image, 64)));

    image = Image();
    Put32(image, ota::kHeaderSize + 4, 100);  // a first segment too short for the descriptor
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Verdict::kNotApp), static_cast<int>(Check(image, 64)));

    image = Image();
    Put32(image, ota::kHeaderSize + 4, 0x7FFFFFFF);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Verdict::kTooLarge), static_cast<int>(Check(image, 64)));
}

void test_ota_too_large_for_slot(void) {
    const std::vector<uint8_t> image = Image();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Verdict::kOk),
                          static_cast<int>(Check(image, 64, Device(image.size()))));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Verdict::kTooLarge),
                          static_cast<int>(Check(image, 64, Device(image.size() - 1))));
}

void test_ota_fault_is_sticky(void) {
    std::vector<uint8_t> image = Image();
    image[0] = 0;
    ota::ImageCheck check(Device());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Verdict::kBadMagic),
                          static_cast<int>(check.Feed(image.data(), 30)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Verdict::kBadMagic),
                          static_cast<int>(check.Feed(image.data() + 30, 10)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Verdict::kBadMagic), static_cast<int>(check.Finish()));
}

void test_ota_should_confirm(void) {
    TEST_ASSERT_FALSE(ota::ShouldConfirm(false, true, 60000));
    TEST_ASSERT_FALSE(ota::ShouldConfirm(true, false, 60000));
    TEST_ASSERT_FALSE(ota::ShouldConfirm(true, true, ota::kConfirmAfterMs - 1));
    TEST_ASSERT_TRUE(ota::ShouldConfirm(true, true, ota::kConfirmAfterMs));
}

}  // namespace

void RegisterOtaTests(void) {
    RUN_TEST(test_ota_good_image_any_chunks);
    RUN_TEST(test_ota_info);
    RUN_TEST(test_ota_refuses_header);
    RUN_TEST(test_ota_refuses_descriptor);
    RUN_TEST(test_ota_refuses_body);
    RUN_TEST(test_ota_too_large_for_slot);
    RUN_TEST(test_ota_fault_is_sticky);
    RUN_TEST(test_ota_should_confirm);
}
