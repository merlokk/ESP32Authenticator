// The stores' sync with the server (components/server/sync_logic.h).

#include <cstring>

#include "sync_logic.h"
#include "unity.h"

namespace {

using server::sync::Copy;
using server::sync::Phase;
using server::sync::Step;

Copy Make(uint8_t store, uint32_t version, uint8_t nonce) {
    Copy c;
    c.has = true;
    memset(c.store_id, store, sizeof c.store_id);
    c.version = version;
    memset(c.nonce, nonce, sizeof c.nonce);
    return c;
}

void test_sync_decide(void) {
    const Copy none;
    const Copy synced = Make(1, 3, 0x33);
    struct Case {
        Copy local;
        Copy synced;
        Copy server;
        bool empty;
        Step step;
        const char *what;
    };
    const Case kCases[] = {
        {none, synced, Make(1, 3, 0x33), false, Step::kNothing, "no local store"},
        {Make(1, 3, 0x33), synced, Make(1, 3, 0x33), false, Step::kNothing, "in sync"},
        {Make(1, 4, 0x44), none, Make(1, 4, 0x44), false, Step::kRecord, "the same, not kept yet"},
        {Make(1, 4, 0x44), synced, none, false, Step::kUpload, "the server lost it"},
        {Make(1, 4, 0x44), synced, Make(1, 3, 0x33), false, Step::kUpload, "ours changed"},
        {Make(1, 3, 0x33), synced, Make(1, 5, 0x55), false, Step::kDownload, "theirs changed"},
        {Make(1, 3, 0x33), synced, Make(1, 3, 0x99), false, Step::kUpload,
         "theirs at our version but another write: not above, ours goes"},
        {Make(1, 4, 0x44), synced, Make(1, 5, 0x55), false, Step::kConflict, "both changed"},
        {Make(1, 4, 0x44), synced, Make(1, 4, 0x45), false, Step::kConflict, "one version, two writes"},
        {Make(2, 1, 0x11), none, none, true, Step::kLook, "never synced, not on the server"},
        {Make(2, 1, 0x11), synced, none, false, Step::kLook, "another store than the synced one"},
        {Make(1, 1, 0x11), none, Make(1, 5, 0x55), true, Step::kDownload, "never synced, empty"},
        {Make(1, 6, 0x11), none, Make(1, 5, 0x55), false, Step::kConflict, "never synced, ours full"},
        {Make(1, 6, 0x11), none, Make(1, 5, 0x55), true, Step::kConflict, "empty but not below"},
    };
    for (const Case &c : kCases) {
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(c.step),
                                  static_cast<int>(server::sync::Decide(c.local, c.synced,
                                                                        c.server, c.empty)),
                                  c.what);
    }
    TEST_ASSERT_TRUE(server::sync::AfterLook(false, 0) == Step::kUpload);
    TEST_ASSERT_TRUE(server::sync::AfterLook(true, 0) == Step::kUpload);
    TEST_ASSERT_TRUE(server::sync::AfterLook(true, 1) == Step::kAdopt);
    TEST_ASSERT_TRUE(server::sync::AfterLook(true, 2) == Step::kConflict);
    TEST_ASSERT_TRUE(server::sync::AfterLook(false, 1) == Step::kConflict);
    TEST_ASSERT_EQUAL_UINT32(8, server::sync::KeepVersion(7));
}

void test_sync_icon(void) {
    using server::sync::Icon;
    using server::sync::IconOf;
    const Phase ok[2] = {Phase::kInSync, Phase::kNone};
    const Phase changed[2] = {Phase::kInSync, Phase::kChanged};
    const Phase failed[2] = {Phase::kFailed, Phase::kInSync};
    const Phase conflict[2] = {Phase::kChanged, Phase::kConflict};
    const Phase pin[2] = {Phase::kNeedPin, Phase::kInSync};
    TEST_ASSERT_TRUE(IconOf(false, true, conflict) == Icon::kHidden);
    TEST_ASSERT_TRUE(IconOf(true, false, conflict) == Icon::kOff);
    TEST_ASSERT_TRUE(IconOf(true, true, ok) == Icon::kOk);
    TEST_ASSERT_TRUE(IconOf(true, true, changed) == Icon::kPending);
    TEST_ASSERT_TRUE(IconOf(true, true, failed) == Icon::kPending);
    TEST_ASSERT_TRUE(IconOf(true, true, conflict) == Icon::kConflict);
    TEST_ASSERT_TRUE(IconOf(true, true, pin) == Icon::kConflict);
    TEST_ASSERT_TRUE(server::sync::ByHand(Phase::kNeedPin));
    TEST_ASSERT_FALSE(server::sync::ByHand(Phase::kNewer));
}

void test_sync_due(void) {
    using server::sync::SyncDue;
    TEST_ASSERT_FALSE(SyncDue(false, 10, false, true, 100, -1, -1));  // not ready
    TEST_ASSERT_FALSE(SyncDue(true, 0, false, true, 100, -1, -1));    // never
    TEST_ASSERT_FALSE(SyncDue(true, 10, true, true, 100, -1, -1));    // waits for a person
    TEST_ASSERT_TRUE(SyncDue(true, 10, false, false, 100, -1, -1));   // the first
    TEST_ASSERT_TRUE(SyncDue(true, 10, false, true, 100, 90, 95));    // a change
    TEST_ASSERT_FALSE(SyncDue(true, 10, false, false, 100, 90, 95));
    TEST_ASSERT_TRUE(SyncDue(true, 10, false, false, 690, 90, 90));   // 10 min on
    TEST_ASSERT_FALSE(SyncDue(true, 10, false, false, 140, 90, 50));  // failed: a minute
    TEST_ASSERT_TRUE(SyncDue(true, 10, false, false, 150, 90, 50));
    TEST_ASSERT_TRUE(SyncDue(true, 10, false, false, 150, 90, -1));
}

void test_sync_synced_round_trip(void) {
    const Copy in = Make(0xA5, 0x01020304, 0x5A);
    uint8_t bytes[server::sync::kSyncedSize];
    server::sync::EncodeSynced(in, bytes);
    TEST_ASSERT_EQUAL_HEX8(0x04, bytes[8]);
    TEST_ASSERT_EQUAL_HEX8(0x01, bytes[11]);
    const Copy out = server::sync::DecodeSynced(bytes);
    TEST_ASSERT_TRUE(server::sync::SameWrite(in, out));
}

void test_sync_requests(void) {
    server::sync::Entry e[2] = {};
    const uint8_t store[8] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18};
    const uint8_t seed[4] = {0x5e, 0xed, 0x5e, 0xed};
    memcpy(e[0].store_id, store, 8);
    memcpy(e[0].seed_id, seed, 4);
    e[0].kind = 2;
    memset(e[1].store_id, 0x11, 8);
    memcpy(e[1].seed_id, seed, 4);
    e[1].kind = 1;
    char body[server::sync::kHeadsBodySize];
    TEST_ASSERT_GREATER_THAN(0, server::sync::HeadsBody(e, 2, body, sizeof body));
    TEST_ASSERT_EQUAL_STRING(
        R"({"stores":[{"storeId":"a1b2c3d4e5f60718","seedId":"5eed5eed","kind":2},)"
        R"({"storeId":"1111111111111111","seedId":"5eed5eed","kind":1}]})",
        body);
    TEST_ASSERT_EQUAL(0, server::sync::HeadsBody(e, 2, body, 40));
    char url[128];
    TEST_ASSERT_TRUE(server::sync::StoresUrl("auth.example.org", seed, url, sizeof url));
    TEST_ASSERT_EQUAL_STRING("https://auth.example.org/api/device/stores?seedId=5eed5eed", url);
    TEST_ASSERT_TRUE(
        server::sync::DownloadUrl("auth.example.org", store, seed, 2, 7, url, sizeof url));
    TEST_ASSERT_EQUAL_STRING(
        "https://auth.example.org/api/device/stores/files/a1b2c3d4e5f60718/5eed5eed/2?above=7",
        url);
    TEST_ASSERT_FALSE(server::sync::DownloadUrl("auth.example.org", store, seed, 2, 7, url, 40));
}

void test_sync_heads(void) {
    const char kAnswer[] =
        R"({"stores": [{"storeId": "a1b2c3d4e5f60718", "seedId": "5eed5eed", "kind": 2,)"
        R"( "version": 2, "modified": 1793128050, "nonce": "0a0b0c0d0e0f101112131415"},)"
        R"( {"storeId": "1111111111111111", "seedId": "5eed5eed", "kind": 1,)"
        R"( "version": null, "modified": null, "nonce": null}]})";
    const uint8_t vault[8] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18};
    uint8_t fido[8];
    memset(fido, 0x11, sizeof fido);
    Copy c;
    TEST_ASSERT_TRUE(server::sync::ParseHeads(kAnswer, strlen(kAnswer), vault, 2, &c));
    TEST_ASSERT_TRUE(c.has);
    TEST_ASSERT_EQUAL_UINT32(2, c.version);
    TEST_ASSERT_EQUAL_HEX8(0x0a, c.nonce[0]);
    TEST_ASSERT_EQUAL_HEX8(0x15, c.nonce[11]);
    TEST_ASSERT_EQUAL_MEMORY(vault, c.store_id, 8);
    TEST_ASSERT_TRUE(server::sync::ParseHeads(kAnswer, strlen(kAnswer), fido, 1, &c));
    TEST_ASSERT_FALSE(c.has);
    TEST_ASSERT_FALSE(server::sync::ParseHeads(kAnswer, strlen(kAnswer), vault, 1, &c));
    TEST_ASSERT_FALSE(server::sync::ParseHeads(kAnswer, 60, vault, 2, &c));  // cut
    TEST_ASSERT_FALSE(server::sync::ParseHeads("{}", 2, vault, 2, &c));
}

void test_sync_stores(void) {
    const char kAnswer[] =
        R"({"stores":[{"storeId":"a1b2c3d4e5f60718","seedId":"5eed5eed","kind":2,)"
        R"("version":2,"modified":1793128050,"cipher":1,"length":120,"kdfIterations":200000,)"
        R"("keyId":"1111111111111111","nonce":"0a0b0c0d0e0f101112131415",)"
        R"("uploadedByDeviceId":"01a17ec2-98d3-7bbb-a8de-9aa8e0089116",)"
        R"("uploadedAt":"2026-10-27T19:07:30.123+00:00"},)"
        R"({"storeId":"2222222222222222","seedId":"5eed5eed","kind":2,"version":9,)"
        R"("nonce":"000000000000000000000000","uploadedAt":"x"},)"
        R"({"storeId":"3333333333333333","seedId":"5eed5eed","kind":1,"version":1,)"
        R"("nonce":"000000000000000000000000"}]})";
    uint8_t ours[8];
    memset(ours, 0x44, sizeof ours);
    server::sync::Candidate c;
    TEST_ASSERT_EQUAL(2, server::sync::ParseStores(kAnswer, strlen(kAnswer), 2, ours, &c));
    TEST_ASSERT_EQUAL_UINT32(2, c.copy.version);
    TEST_ASSERT_TRUE(c.copy.store_id[0] == 0xa1);
    TEST_ASSERT_EQUAL_HEX8(0x11, c.key_id[7]);
    TEST_ASSERT_EQUAL_INT64(1793128050, c.modified);
    TEST_ASSERT_EQUAL_STRING("01a17ec2-98d3-7bbb-a8de-9aa8e0089116", c.by);
    TEST_ASSERT_EQUAL_STRING("2026-10-27 19:07", c.at);
    memset(ours, 0x22, sizeof ours);  // ours is not counted
    TEST_ASSERT_EQUAL(1, server::sync::ParseStores(kAnswer, strlen(kAnswer), 2, ours, &c));
    TEST_ASSERT_EQUAL(1, server::sync::ParseStores(kAnswer, strlen(kAnswer), 1, ours, &c));
    TEST_ASSERT_EQUAL_UINT32(1, c.copy.version);
    TEST_ASSERT_EQUAL(0, server::sync::ParseStores(R"({"stores":[]})", 13, 2, ours, &c));
    TEST_ASSERT_EQUAL(-1, server::sync::ParseStores("{}", 2, 2, ours, &c));
}

void test_sync_texts(void) {
    TEST_ASSERT_EQUAL_STRING("in sync", server::sync::PhaseText(Phase::kInSync));
    TEST_ASSERT_EQUAL_STRING("conflict", server::sync::PhaseText(Phase::kConflict));
    TEST_ASSERT_EQUAL_STRING("passwords", server::sync::StoreName(server::sync::Store::kVault));
    TEST_ASSERT_EQUAL(2, server::sync::KindOf(server::sync::Store::kVault));
    TEST_ASSERT_EQUAL(1, server::sync::KindOf(server::sync::Store::kFido));
}

}  // namespace

void RegisterSyncTests(void) {
    RUN_TEST(test_sync_decide);
    RUN_TEST(test_sync_icon);
    RUN_TEST(test_sync_due);
    RUN_TEST(test_sync_synced_round_trip);
    RUN_TEST(test_sync_requests);
    RUN_TEST(test_sync_heads);
    RUN_TEST(test_sync_stores);
    RUN_TEST(test_sync_texts);
}
