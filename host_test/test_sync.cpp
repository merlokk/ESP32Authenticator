// The stores' sync with the server (components/server/sync_logic.h).

#include <cstring>

#include "sync_logic.h"
#include "unity.h"

namespace {

using server::sync::Copy;
using server::sync::Phase;
using server::sync::Step;

Copy Make(uint8_t store, uint32_t version, uint8_t nonce, uint8_t seed = 0x5e) {
    Copy c;
    c.has = true;
    memset(c.store_id, store, sizeof c.store_id);
    c.version = version;
    memset(c.nonce, nonce, sizeof c.nonce);
    memset(c.seed_id, seed, sizeof c.seed_id);
    return c;
}

void test_sync_decide(void) {
    const Copy none;
    const Copy synced = Make(1, 3, 0x33);
    struct Case {
        Copy local;
        Copy synced;
        Copy server;
        bool open;
        bool empty;
        Step step;
        const char *what;
    };
    const Case kCases[] = {
        {none, synced, Make(1, 3, 0x33), true, false, Step::kNothing, "no local store"},
        {Make(1, 3, 0x33), synced, Make(1, 3, 0x33), true, false, Step::kNothing, "in sync"},
        {Make(1, 4, 0x44), none, Make(1, 4, 0x44), true, false, Step::kRecord, "the same, not kept yet"},
        {Make(1, 4, 0x44), none, Make(1, 4, 0x44), false, false, Step::kNothing,
         "the same, not kept, locked: empty or not unknown"},
        {Make(1, 4, 0x44), none, Make(1, 4, 0x44), true, true, Step::kLook,
         "the same, not kept, empty: another device's store first"},
        {Make(1, 4, 0x44, 0x4c), Make(1, 3, 0x33, 0x48), Make(1, 4, 0x44, 0x4c), true, true,
         Step::kLook, "uploaded under a new phrase (seed set): another device's store first"},
        {Make(1, 4, 0x44), Make(1, 4, 0x44, 0), Make(1, 4, 0x44), true, true, Step::kNothing,
         "an old record (no seed id): ours"},
        {Make(1, 4, 0x44), synced, none, true, false, Step::kUpload, "the server lost it"},
        {Make(1, 4, 0x44), synced, Make(1, 3, 0x33), true, false, Step::kUpload, "ours changed"},
        {Make(1, 3, 0x33), synced, Make(1, 5, 0x55), true, false, Step::kDownload, "theirs changed"},
        {Make(1, 3, 0x33), synced, Make(1, 3, 0x99), true, false, Step::kUpload,
         "theirs at our version but another write: not above, ours goes"},
        {Make(1, 4, 0x44), synced, Make(1, 5, 0x55), true, false, Step::kConflict, "both changed"},
        {Make(1, 4, 0x44), synced, Make(1, 4, 0x45), true, false, Step::kConflict, "one version, two writes"},
        {Make(2, 1, 0x11), none, none, true, true, Step::kLook, "never synced, not on the server"},
        {Make(2, 1, 0x11), synced, none, true, false, Step::kLook, "another store than the synced one"},
        {Make(1, 4, 0x44, 0x4c), synced, none, true, false, Step::kLook,
         "a new phrase (seed set / seed new): not on the server under it"},
        {Make(1, 1, 0x11), none, Make(1, 5, 0x55), true, true, Step::kDownload, "never synced, empty"},
        {Make(1, 6, 0x11), none, Make(1, 5, 0x55), true, false, Step::kConflict, "never synced, ours full"},
        {Make(1, 6, 0x11), none, Make(1, 5, 0x55), true, true, Step::kConflict, "empty but not below"},
    };
    for (const Case &c : kCases) {
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(c.step),
                                  static_cast<int>(server::sync::Decide(c.local, c.synced,
                                                                        c.server, c.open, c.empty)),
                                  c.what);
    }
    TEST_ASSERT_TRUE(server::sync::AfterLook(false, 0, false) == Step::kUpload);
    TEST_ASSERT_TRUE(server::sync::AfterLook(true, 0, false) == Step::kUpload);
    TEST_ASSERT_TRUE(server::sync::AfterLook(true, 0, true) == Step::kRecord);
    TEST_ASSERT_TRUE(server::sync::AfterLook(true, 1, true) == Step::kAdopt);
    TEST_ASSERT_TRUE(server::sync::AfterLook(true, 2, false) == Step::kConflict);
    TEST_ASSERT_TRUE(server::sync::AfterLook(false, 1, false) == Step::kConflict);
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
    TEST_ASSERT_EQUAL_HEX8(0x5e, bytes[24]);
    const Copy out = server::sync::DecodeSynced(bytes, sizeof bytes);
    TEST_ASSERT_TRUE(server::sync::SameWrite(in, out));
    TEST_ASSERT_TRUE(server::sync::Known(in, out));
    // An old record (no seed id): taken as ours under any phrase.
    const Copy old = server::sync::DecodeSynced(bytes, server::sync::kSyncedOldSize);
    TEST_ASSERT_TRUE(server::sync::SameWrite(in, old));
    TEST_ASSERT_EQUAL_HEX8(0, old.seed_id[0]);
    TEST_ASSERT_TRUE(server::sync::Known(Make(0xA5, 1, 1, 0x77), old));
    TEST_ASSERT_FALSE(server::sync::Known(Make(0xA5, 1, 1, 0x77), out));
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

void test_sync_others(void) {
    const char kAnswer[] =
        R"({"stores":[)"
        R"({"storeId":"1111111111111111","seedId":"5eed5eed","kind":2,"version":9,)"
        R"("nonce":"000000000000000000000000"},)"
        R"({"storeId":"1111111111111111","seedId":"0a0b0c0d","kind":2,"version":4,)"
        R"("nonce":"000000000000000000000000","uploadedAt":"2026-10-20T08:00:00+00:00"},)"
        R"({"storeId":"3333333333333333","seedId":"0e0e0e0e","kind":1,"version":2,)"
        R"("nonce":"000000000000000000000000","uploadedAt":"2026-10-21T09:30:00+00:00"},)"
        R"({"storeId":"1111111111111111","seedId":"0f0f0f0f","kind":2,"version":12,)"
        R"("nonce":"000000000000000000000000","uploadedAt":"2026-10-27T19:07:30+00:00"}]})";
    const uint8_t own[4] = {0x5e, 0xed, 0x5e, 0xed};
    const Copy ours[2] = {Make(0x11, 9, 0x01), Copy()};
    server::sync::Others o;
    TEST_ASSERT_TRUE(server::sync::ParseOthers(kAnswer, strlen(kAnswer), own, ours, &o));
    TEST_ASSERT_EQUAL(3, o.count);  // not the own phrase's
    TEST_ASSERT_TRUE(o.shown.why == server::sync::OtherKind::kMoved);  // ours, v12 > v9
    TEST_ASSERT_EQUAL_HEX8(0x0f, o.shown.seed_id[0]);
    TEST_ASSERT_EQUAL_UINT32(12, o.shown.version);
    TEST_ASSERT_EQUAL_STRING("2026-10-27 19:07", o.shown.at);
    // Without the moved one: another device's comes before an old copy.
    const char kTwo[] =
        R"({"stores":[{"storeId":"1111111111111111","seedId":"0a0b0c0d","kind":2,"version":4,)"
        R"("nonce":"000000000000000000000000"},)"
        R"({"storeId":"3333333333333333","seedId":"0e0e0e0e","kind":1,"version":2,)"
        R"("nonce":"000000000000000000000000"}]})";
    TEST_ASSERT_TRUE(server::sync::ParseOthers(kTwo, strlen(kTwo), own, ours, &o));
    TEST_ASSERT_EQUAL(2, o.count);
    TEST_ASSERT_TRUE(o.shown.why == server::sync::OtherKind::kOther);
    TEST_ASSERT_EQUAL(1, o.shown.kind);
    const char kOld[] =
        R"({"stores":[{"storeId":"1111111111111111","seedId":"0a0b0c0d","kind":2,"version":4,)"
        R"("nonce":"000000000000000000000000"}]})";
    TEST_ASSERT_TRUE(server::sync::ParseOthers(kOld, strlen(kOld), own, ours, &o));
    TEST_ASSERT_TRUE(o.shown.why == server::sync::OtherKind::kOld);
    TEST_ASSERT_TRUE(server::sync::ParseOthers(R"({"stores":[]})", 13, own, ours, &o));
    TEST_ASSERT_EQUAL(0, o.count);
    TEST_ASSERT_FALSE(server::sync::ParseOthers("{}", 2, own, ours, &o));
    TEST_ASSERT_EQUAL_STRING("passkeys", server::sync::KindName(1));
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
    RUN_TEST(test_sync_others);
    RUN_TEST(test_sync_texts);
}
