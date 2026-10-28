// The server's firmware releases (components/server/firmware_logic.h).

#include <cstring>

#include "firmware_logic.h"
#include "unity.h"

namespace {

namespace fw = server::firmware;

void test_firmware_versions(void) {
    const fw::Version v = fw::ParseVersion("0.1.0-238-g0844f88-dirty");
    TEST_ASSERT_TRUE(v.ok);
    TEST_ASSERT_EQUAL_UINT32(0, v.major);
    TEST_ASSERT_EQUAL_UINT32(1, v.minor);
    TEST_ASSERT_EQUAL_UINT32(0, v.patch);
    TEST_ASSERT_EQUAL_UINT32(238, v.commits);
    TEST_ASSERT_TRUE(fw::ParseVersion("1.2.3").ok);
    TEST_ASSERT_EQUAL_UINT32(0, fw::ParseVersion("1.2.3-nogit").commits);
    TEST_ASSERT_FALSE(fw::ParseVersion("").ok);
    TEST_ASSERT_FALSE(fw::ParseVersion("1.2").ok);
    TEST_ASSERT_FALSE(fw::ParseVersion("v1.2.3").ok);
    TEST_ASSERT_FALSE(fw::ParseVersion("1.2.3-12").ok);  // commits with no hash after

    TEST_ASSERT_TRUE(fw::Newer("0.1.0-240-gaaaaaaa", "0.1.0-238-g0844f88-dirty"));
    TEST_ASSERT_TRUE(fw::Newer("0.2.0-0-gbbbbbbb", "0.1.0-999-gccccccc"));
    TEST_ASSERT_TRUE(fw::Newer("1.0.0", "0.9.9-5-gddddddd"));
    TEST_ASSERT_TRUE(fw::Newer("0.1.10-0-geeeeeee", "0.1.9-50-gfffffff"));
    TEST_ASSERT_FALSE(fw::Newer("0.1.0-238-g1111111", "0.1.0-238-g0844f88-dirty"));  // same
    TEST_ASSERT_FALSE(fw::Newer("0.1.0-200-g1111111", "0.1.0-238-g0844f88"));
    TEST_ASSERT_FALSE(fw::Newer("garbage", "0.1.0"));
    TEST_ASSERT_TRUE(fw::Newer("0.1.0", "garbage"));
    TEST_ASSERT_EQUAL(0, fw::Compare("garbage", "other"));
}

void test_firmware_list(void) {
    const char kList[] =
        R"({"releases":[)"
        R"({"id":"01a17f4a-ec9e-7733-85e7-b65f35c133c8","board":"x4pro","version":"0.1.0-230-gaaaaaaa",)"
        R"("built":"Oct 27 2026 21:07:30","size":1843200,)"
        R"("sha256":"9f2c000000000000000000000000000000000000000000000000000000000001","active":true,)"
        R"("uploadedAt":"2026-10-28T10:00:00+00:00"},)"
        R"({"id":"01a17f4a-ec9e-7733-85e7-b65f35c133c9","board":"x4pro","version":"0.1.0-245-gbbbbbbb",)"
        R"("built":"Oct 28 2026 09:00:00","size":1843300,)"
        R"("sha256":"9f2c000000000000000000000000000000000000000000000000000000000002","active":false,)"
        R"("uploadedAt":"2026-10-27T10:00:00+00:00"},)"
        R"({"id":"not-an-id","board":"x4pro","version":"9.9.9","built":"","size":1,)"
        R"("sha256":"9f2c000000000000000000000000000000000000000000000000000000000003","active":false})"
        R"(]})";
    fw::Release r[fw::kMaxReleases] = {};
    TEST_ASSERT_EQUAL(2, fw::ParseReleases(kList, strlen(kList), r));
    TEST_ASSERT_EQUAL_STRING("0.1.0-245-gbbbbbbb", r[0].version);  // newest first, not by upload
    TEST_ASSERT_FALSE(r[0].active);
    TEST_ASSERT_EQUAL_UINT32(1843300, r[0].size);
    TEST_ASSERT_EQUAL_STRING("Oct 28 2026 09:00:00", r[0].built);
    TEST_ASSERT_EQUAL_STRING("0.1.0-230-gaaaaaaa", r[1].version);
    TEST_ASSERT_TRUE(r[1].active);
    TEST_ASSERT_EQUAL_STRING("01a17f4a-ec9e-7733-85e7-b65f35c133c8", r[1].id);
    TEST_ASSERT_TRUE(fw::AnyNewer(r, 2, "0.1.0-238-g0844f88"));
    TEST_ASSERT_FALSE(fw::AnyNewer(r, 2, "0.1.0-245-gccccccc"));

    TEST_ASSERT_EQUAL(0, fw::ParseReleases(R"({"releases":[]})", 15, r));
    TEST_ASSERT_EQUAL(-1, fw::ParseReleases("{}", 2, r));
}

void test_firmware_list_keeps_newest(void) {
    // Ten releases, oldest first: the eight newest kept.
    char body[4096] = R"({"releases":[)";
    for (int i = 0; i < 10; ++i) {
        char one[320];
        snprintf(one, sizeof one,
                 R"(%s{"id":"01a17f4a-ec9e-7733-85e7-b65f35c133%02d","version":"0.1.%d-0-gaaaaaaa",)"
                 R"("size":10,"sha256":"%064d","active":false})",
                 i == 0 ? "" : ",", i, i, 0);
        strcat(body, one);
    }
    strcat(body, "]}");
    fw::Release r[fw::kMaxReleases] = {};
    TEST_ASSERT_EQUAL(8, fw::ParseReleases(body, strlen(body), r));
    TEST_ASSERT_EQUAL_STRING("0.1.9-0-gaaaaaaa", r[0].version);
    TEST_ASSERT_EQUAL_STRING("0.1.2-0-gaaaaaaa", r[7].version);
}

void test_firmware_requests(void) {
    char out[128];
    TEST_ASSERT_TRUE(fw::ListUrl("auth.example.com", "x4pro", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("https://auth.example.com/api/device/firmware?board=x4pro", out);
    TEST_ASSERT_TRUE(
        fw::ImageUrl("auth.example.com", "01a17f4a-ec9e-7733-85e7-b65f35c133c8", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING(
        "https://auth.example.com/api/device/firmware/01a17f4a-ec9e-7733-85e7-b65f35c133c8/image",
        out);
    TEST_ASSERT_FALSE(fw::ImageUrl("auth.example.com", "../x", out, sizeof out));

    TEST_ASSERT_TRUE(fw::RangeHeader(0, 100000, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("bytes=0-65535", out);
    TEST_ASSERT_TRUE(fw::RangeHeader(65536, 100000, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("bytes=65536-99999", out);
    TEST_ASSERT_FALSE(fw::RangeHeader(100000, 100000, out, sizeof out));

    TEST_ASSERT_TRUE(fw::Transient(502));
    TEST_ASSERT_TRUE(fw::Transient(503));
    TEST_ASSERT_TRUE(fw::Transient(408));
    TEST_ASSERT_TRUE(fw::Transient(429));
    TEST_ASSERT_FALSE(fw::Transient(403));
    TEST_ASSERT_FALSE(fw::Transient(416));

    uint32_t first = 0;
    TEST_ASSERT_TRUE(fw::RangeStart("bytes 65536-99999/100000", &first));
    TEST_ASSERT_EQUAL_UINT32(65536, first);
    TEST_ASSERT_FALSE(fw::RangeStart("bytes */100000", &first));
    TEST_ASSERT_FALSE(fw::RangeStart("bytes 5-4/100000", &first));
    TEST_ASSERT_FALSE(fw::RangeStart("bytes 0-100000/100000", &first));
}

}  // namespace

void RegisterFirmwareTests(void) {
    RUN_TEST(test_firmware_versions);
    RUN_TEST(test_firmware_list);
    RUN_TEST(test_firmware_list_keeps_newest);
    RUN_TEST(test_firmware_requests);
}
