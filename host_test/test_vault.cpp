// vault records: what the user typed is what is kept, in the user's order
// (Move, SortByName), unique per (site, user), and survives a serialize /
// parse round trip byte for byte; format 1 files are read sorted by name.
// vault.cpp itself (fakes/fake_store.cpp: NVS, crypto stand-ins) on a real
// directory under build/: every path that makes a key refuses a weak PIN.

#include <cstring>
#include <filesystem>

#include "nvs.h"
#include "pin_guard.h"
#include "records.h"
#include "seed_phrase.h"
#include "unity.h"
#include "vault.h"

namespace {

using vault::Error;
using vault::Field;
using vault::Record;
using vault::Records;

Record g_slots[8];
uint8_t g_buffer[vault::MaxSerialized(8)];
Record g_other_slots[8];

void test_vault_add_goes_last_and_sort_by_name(void) {
    Records r(g_slots, 8);
    uint32_t id = 0;
    TEST_ASSERT_EQUAL(Error::kOk, r.Add("github.com", "bob", "pw1", "", 100, &id));
    TEST_ASSERT_EQUAL_UINT32(1, id);
    TEST_ASSERT_EQUAL(Error::kOk, r.Add("Amazon", "x", "", "", 0, nullptr));
    TEST_ASSERT_EQUAL(Error::kOk, r.Add("GitHub.com", "alice", "pw2", "", 0, &id));
    TEST_ASSERT_EQUAL_UINT32(3, id);
    TEST_ASSERT_EQUAL_UINT32(3, r.Count());
    TEST_ASSERT_EQUAL_STRING("github.com", r.At(0).site);  // as added
    TEST_ASSERT_EQUAL_STRING("Amazon", r.At(1).site);
    TEST_ASSERT_EQUAL_STRING("alice", r.At(2).user);
    r.SortByName();
    TEST_ASSERT_EQUAL_STRING("Amazon", r.At(0).site);
    TEST_ASSERT_EQUAL_STRING("alice", r.At(1).user);  // same site, users sorted
    TEST_ASSERT_EQUAL_STRING("bob", r.At(2).user);
    TEST_ASSERT_EQUAL(100, r.At(2).modified_utc);
}

void test_vault_move(void) {
    Records r(g_slots, 8);
    uint32_t ids[4] = {};
    const char *sites[4] = {"a", "b", "c", "d"};
    for (int i = 0; i < 4; ++i) {
        r.Add(sites[i], "", "", "", 0, &ids[i]);
    }
    TEST_ASSERT_EQUAL(Error::kOk, r.Move(ids[3], 0));  // d a b c
    TEST_ASSERT_EQUAL_STRING("d", r.At(0).site);
    TEST_ASSERT_EQUAL_STRING("c", r.At(3).site);
    TEST_ASSERT_EQUAL(Error::kOk, r.Move(ids[0], 2));  // d b a c
    TEST_ASSERT_EQUAL_STRING("b", r.At(1).site);
    TEST_ASSERT_EQUAL_STRING("a", r.At(2).site);
    TEST_ASSERT_EQUAL(Error::kOk, r.Move(ids[3], 99));  // past the end: last; b a c d
    TEST_ASSERT_EQUAL_STRING("b", r.At(0).site);
    TEST_ASSERT_EQUAL_STRING("d", r.At(3).site);
    TEST_ASSERT_EQUAL_UINT32(ids[3], r.At(3).id);
    TEST_ASSERT_EQUAL(Error::kOk, r.Move(ids[1], 0));  // already there
    TEST_ASSERT_EQUAL(Error::kNotFound, r.Move(999, 0));
    // The order travels with the data.
    const size_t n = r.Serialize(g_buffer, sizeof g_buffer);
    Records back(g_other_slots, 8);
    TEST_ASSERT_EQUAL(Error::kOk, back.Parse(g_buffer, n));
    TEST_ASSERT_EQUAL_STRING("b", back.At(0).site);
    TEST_ASSERT_EQUAL_STRING("d", back.At(3).site);
    // A format 1 file (the same bytes) is read sorted by name.
    g_buffer[0] = 1;
    TEST_ASSERT_EQUAL(Error::kOk, back.Parse(g_buffer, n));
    TEST_ASSERT_EQUAL_STRING("a", back.At(0).site);
    TEST_ASSERT_EQUAL_STRING("d", back.At(3).site);
}

void test_vault_the_same_user_on_a_site_is_a_duplicate(void) {
    Records r(g_slots, 8);
    TEST_ASSERT_EQUAL(Error::kOk, r.Add("site", "Bob", "", "", 0, nullptr));
    TEST_ASSERT_EQUAL(Error::kDuplicate, r.Add("SITE", "bob", "", "", 0, nullptr));
    TEST_ASSERT_EQUAL(Error::kOk, r.Add("site", "", "", "", 0, nullptr));  // no user: once
    TEST_ASSERT_EQUAL(Error::kDuplicate, r.Add("site", "", "", "", 0, nullptr));
    TEST_ASSERT_EQUAL_UINT32(2, r.Count());
}

void test_vault_fields_are_checked(void) {
    Records r(g_slots, 8);
    char long_site[vault::kSiteMax + 2];
    memset(long_site, 'a', sizeof long_site - 1);
    long_site[sizeof long_site - 1] = '\0';
    TEST_ASSERT_EQUAL(Error::kSiteEmpty, r.Add("", "u", "", "", 0, nullptr));
    TEST_ASSERT_EQUAL(Error::kTooLong, r.Add(long_site, "u", "", "", 0, nullptr));
    TEST_ASSERT_EQUAL(Error::kBadText, r.Add("a\tb", "u", "", "", 0, nullptr));
    TEST_ASSERT_EQUAL(Error::kBadOtp, r.Add("a", "u", "", "not base32!", 0, nullptr));
    TEST_ASSERT_EQUAL(Error::kOk, r.Add("a", "u", "p@ss w\xC3\xB6rd", "", 0, nullptr));  // UTF-8
    TEST_ASSERT_EQUAL_UINT32(1, r.Count());
}

void test_vault_otp_from_base64_is_kept_as_base32(void) {
    Record slots[2];
    Records r(slots, 2);
    // "Hello!\xde\xad\xbe\xef" = JBSWY3DPEHPK3PXP in Base32.
    TEST_ASSERT_EQUAL(Error::kOk, r.Add("a", "u", "", "base64:SGVsbG8h3q2+7w==", 0, nullptr));
    TEST_ASSERT_EQUAL_STRING("JBSWY3DPEHPK3PXP", r.At(0).otp);
    TEST_ASSERT_EQUAL(Error::kOk, r.Add("b", "u", "", "BASE64:SGVsbG8h3q2-7w", 0, nullptr));
    TEST_ASSERT_EQUAL_STRING("JBSWY3DPEHPK3PXP", r.At(1).otp);
    char out[vault::kOtpMax + 1];
    TEST_ASSERT_EQUAL(Error::kBadOtp, vault::CheckField(Field::kOtp, "base64:", out));
    TEST_ASSERT_EQUAL(Error::kBadOtp, vault::CheckField(Field::kOtp, "base64:a!b", out));
    // 40 bytes fit (64 Base32 digits), 41 do not.
    char text[80] = "base64:";
    for (int i = 0; i < 54; ++i) std::strcat(text, "A");  // 40 bytes
    TEST_ASSERT_EQUAL(Error::kOk, vault::CheckField(Field::kOtp, text, out));
    TEST_ASSERT_EQUAL_UINT32(64, std::strlen(out));
    std::strcat(text, "AA");
    TEST_ASSERT_EQUAL(Error::kTooLong, vault::CheckField(Field::kOtp, text, out));
}

void test_vault_otp_is_normalized_base32(void) {
    Records r(g_slots, 8);
    TEST_ASSERT_EQUAL(Error::kOk, r.Add("a", "u", "", "jbsw y3dp-ehpk 3pxp==", 0, nullptr));
    TEST_ASSERT_EQUAL_STRING("JBSWY3DPEHPK3PXP", r.At(0).otp);
}

void test_vault_full_store_refuses(void) {
    Records r(g_slots, 2);
    TEST_ASSERT_EQUAL(Error::kOk, r.Add("a", "", "", "", 0, nullptr));
    TEST_ASSERT_EQUAL(Error::kOk, r.Add("b", "", "", "", 0, nullptr));
    TEST_ASSERT_EQUAL(Error::kFull, r.Add("c", "", "", "", 0, nullptr));
}

void test_vault_set_keeps_the_place_and_the_id(void) {
    Records r(g_slots, 8);
    uint32_t id = 0;
    r.Add("b", "u", "old", "", 0, &id);
    r.Add("c", "u", "", "", 0, nullptr);
    TEST_ASSERT_EQUAL(Error::kOk, r.Set(id, Field::kSite, "d", 50));
    TEST_ASSERT_EQUAL_STRING("d", r.At(0).site);  // renamed, not moved
    TEST_ASSERT_EQUAL_STRING("c", r.At(1).site);
    TEST_ASSERT_EQUAL_UINT32(id, r.At(0).id);
    TEST_ASSERT_EQUAL(50, r.At(0).modified_utc);
    TEST_ASSERT_EQUAL(Error::kOk, r.Set(id, Field::kPassword, "new", 60));
    TEST_ASSERT_EQUAL_STRING("new", r.At(0).password);
    TEST_ASSERT_EQUAL(Error::kDuplicate, r.Set(id, Field::kSite, "C", 0));
    TEST_ASSERT_EQUAL(Error::kOk, r.Set(id, Field::kSite, "D", 0));  // its own case change
    TEST_ASSERT_EQUAL(Error::kNotFound, r.Set(999, Field::kUser, "x", 0));
    TEST_ASSERT_EQUAL(Error::kBadOtp, r.Set(id, Field::kOtp, "1", 0));
}

void test_vault_remove_and_ids_never_repeat(void) {
    Records r(g_slots, 8);
    uint32_t a = 0;
    uint32_t b = 0;
    r.Add("a", "", "", "", 0, &a);
    r.Add("b", "", "", "", 0, &b);
    TEST_ASSERT_EQUAL(Error::kOk, r.Remove(a));
    TEST_ASSERT_EQUAL(Error::kNotFound, r.Remove(a));
    TEST_ASSERT_EQUAL_UINT32(1, r.Count());
    TEST_ASSERT_EQUAL_STRING("b", r.At(0).site);
    uint32_t c = 0;
    r.Add("a", "", "", "", 0, &c);
    TEST_ASSERT_EQUAL_UINT32(3, c);
}

void test_vault_round_trip(void) {
    Records r(g_slots, 8);
    r.Add("site", "user", "secret", "JBSWY3DPEHPK3PXP", 1700000000, nullptr);
    r.Add("other", "", "", "", 0, nullptr);
    r.Remove(2);
    r.Add("third", "x", "y", "", -1, nullptr);
    const size_t n = r.Serialize(g_buffer, sizeof g_buffer);
    TEST_ASSERT_NOT_EQUAL(0, n);
    Records back(g_other_slots, 8);
    TEST_ASSERT_EQUAL(Error::kOk, back.Parse(g_buffer, n));
    TEST_ASSERT_EQUAL_UINT32(2, back.Count());
    TEST_ASSERT_EQUAL_STRING("secret", back.At(0).password);
    TEST_ASSERT_EQUAL_STRING("JBSWY3DPEHPK3PXP", back.At(0).otp);
    TEST_ASSERT_TRUE(back.At(0).modified_utc == 1700000000);
    TEST_ASSERT_TRUE(back.At(1).modified_utc == -1);
    uint32_t id = 0;
    back.Add("fourth", "", "", "", 0, &id);
    TEST_ASSERT_EQUAL_UINT32(4, id);  // next id travels with the data
    TEST_ASSERT_EQUAL(0, r.Serialize(g_buffer, n - 1));  // no room: nothing
}

void test_vault_parse_refuses_malformed_data(void) {
    Records r(g_slots, 8);
    r.Add("site", "user", "pw", "", 0, nullptr);
    const size_t n = r.Serialize(g_buffer, sizeof g_buffer);
    Records back(g_other_slots, 8);
    TEST_ASSERT_EQUAL(Error::kBadFormat, back.Parse(g_buffer, n - 1));  // truncated
    g_buffer[n] = 0;
    TEST_ASSERT_EQUAL(Error::kBadFormat, back.Parse(g_buffer, n + 1));  // trailing byte
    g_buffer[0] = 3;
    TEST_ASSERT_EQUAL(Error::kBadFormat, back.Parse(g_buffer, n));  // other format
    g_buffer[0] = 2;
    g_buffer[19] = 200;  // site length beyond the data
    TEST_ASSERT_EQUAL(Error::kBadFormat, back.Parse(g_buffer, n));
    TEST_ASSERT_EQUAL_UINT32(0, back.Count());
    Records small(g_other_slots, 0);
    r.Serialize(g_buffer, sizeof g_buffer);
    TEST_ASSERT_EQUAL(Error::kFull, small.Parse(g_buffer, n));
}

void test_vault_pin_is_six_digits(void) {
    TEST_ASSERT_TRUE(vault::ValidPin("333333"));
    TEST_ASSERT_TRUE(vault::ValidPin("000000"));
    TEST_ASSERT_FALSE(vault::ValidPin("33333"));
    TEST_ASSERT_FALSE(vault::ValidPin("3333333"));
    TEST_ASSERT_FALSE(vault::ValidPin("33333a"));
    TEST_ASSERT_FALSE(vault::ValidPin(""));
}

void test_vault_no_lock_below_ten_failures(void) {
    TEST_ASSERT_EQUAL_UINT32(0, vault::LockSecondsAtBoot(9, 5000, 1000, 600));
    TEST_ASSERT_EQUAL_UINT32(0, vault::LockSecondsAtBoot(0, 0, 0, 600));
}

void test_vault_lock_resumes_where_it_was_with_a_known_clock(void) {
    TEST_ASSERT_EQUAL_UINT32(250, vault::LockSecondsAtBoot(10, 1250, 1000, 600));
    TEST_ASSERT_EQUAL_UINT32(0, vault::LockSecondsAtBoot(10, 1000, 1000, 600));  // over
    TEST_ASSERT_EQUAL_UINT32(0, vault::LockSecondsAtBoot(12, 900, 1000, 600));
}

void test_vault_lock_starts_again_without_a_clock_or_with_one_moved_back(void) {
    TEST_ASSERT_EQUAL_UINT32(600, vault::LockSecondsAtBoot(10, 0, 1000, 600));
    TEST_ASSERT_EQUAL_UINT32(600, vault::LockSecondsAtBoot(10, 1250, 0, 600));
    // A deadline further than a full lock: the clock went back.
    TEST_ASSERT_EQUAL_UINT32(600, vault::LockSecondsAtBoot(10, 100000, 1000, 600));
}

void test_vault_wipe_text_round_trips(void) {
    uint8_t salt[vault::kWipeSaltSize];
    uint8_t hash[vault::kWipeHashSize];
    for (size_t i = 0; i < sizeof salt; ++i) salt[i] = static_cast<uint8_t>(i * 17);
    for (size_t i = 0; i < sizeof hash; ++i) hash[i] = static_cast<uint8_t>(255 - i);
    char text[vault::kWipeTextSize];
    vault::FormatWipe(salt, hash, text);
    TEST_ASSERT_EQUAL_UINT32(vault::kWipeTextSize - 1, strlen(text));
    TEST_ASSERT_EQUAL_CHAR(':', text[32]);
    TEST_ASSERT_EQUAL_STRING_LEN("0011223344", text, 10);
    uint8_t salt2[vault::kWipeSaltSize];
    uint8_t hash2[vault::kWipeHashSize];
    TEST_ASSERT_TRUE(vault::ParseWipe(text, salt2, hash2));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(salt, salt2, sizeof salt);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(hash, hash2, sizeof hash);
}

void test_vault_wipe_text_must_be_exact(void) {
    uint8_t salt[vault::kWipeSaltSize] = {};
    uint8_t hash[vault::kWipeHashSize] = {};
    char text[vault::kWipeTextSize + 1];
    vault::FormatWipe(salt, hash, text);
    TEST_ASSERT_TRUE(vault::ParseWipe(text, salt, hash));
    text[32] = '-';
    TEST_ASSERT_FALSE(vault::ParseWipe(text, salt, hash));
    text[32] = ':';
    text[5] = 'g';
    TEST_ASSERT_FALSE(vault::ParseWipe(text, salt, hash));
    text[5] = '0';
    text[vault::kWipeTextSize - 2] = '\0';  // one short
    TEST_ASSERT_FALSE(vault::ParseWipe(text, salt, hash));
    text[vault::kWipeTextSize - 2] = '0';
    text[vault::kWipeTextSize - 1] = '0';  // one long
    text[vault::kWipeTextSize] = '\0';
    TEST_ASSERT_FALSE(vault::ParseWipe(text, salt, hash));
    TEST_ASSERT_FALSE(vault::ParseWipe("", salt, hash));
}

void test_vault_clear_wipes_secrets(void) {
    Records r(g_slots, 8);
    r.Add("site", "user", "secret", "", 0, nullptr);
    r.Clear();
    for (size_t i = 0; i < sizeof g_slots; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0, reinterpret_cast<const uint8_t *>(g_slots)[i]);
    }
}

// --- vault.cpp: a weak new PIN (pin_policy.h) under config `pin.complexity`

constexpr const char *kStoreDir = "build/vaultfs";
constexpr const char *kSeedPath = "build/vaultfs/seed.txt";
constexpr const char *kStrongPin = "583920";
constexpr const char *kWeakPin = "123456";  // a run

// An empty store directory and NVS, the key out, the given policy.
void FreshVault(bool complexity) {
    static bool inited = false;
    vault::Lock();
    std::error_code ec;
    std::filesystem::remove_all(kStoreDir, ec);
    std::filesystem::create_directories(kStoreDir, ec);
    fake::NvsClear();
    if (!inited) {
        TEST_ASSERT_EQUAL(ESP_OK, vault::Init(kStoreDir, 8, kSeedPath));
        inited = true;
    }
    vault::SetPolicy({600, "", complexity});
}

void test_vault_new_store_refuses_a_weak_pin(void) {
    FreshVault(true);
    TEST_ASSERT_EQUAL(Error::kWeakPin, vault::Unlock(kWeakPin));
    TEST_ASSERT_FALSE(vault::GetInfo().has_key);
    TEST_ASSERT_EQUAL(0, vault::GetInfo().file_size);  // no store made
    TEST_ASSERT_EQUAL(Error::kOk, vault::Unlock(kStrongPin));
    TEST_ASSERT_TRUE(vault::GetInfo().state == vault::State::kOpen);

    FreshVault(false);
    TEST_ASSERT_EQUAL(Error::kOk, vault::Unlock(kWeakPin));
    TEST_ASSERT_TRUE(vault::GetInfo().state == vault::State::kOpen);
}

void test_vault_change_pin_refuses_a_weak_pin(void) {
    FreshVault(true);
    TEST_ASSERT_EQUAL(Error::kOk, vault::Unlock(kStrongPin));
    TEST_ASSERT_EQUAL(Error::kWeakPin, vault::ChangePin(kStrongPin, kWeakPin));
    vault::Lock();
    TEST_ASSERT_EQUAL(Error::kWrongPin, vault::Unlock(kWeakPin));
    TEST_ASSERT_EQUAL(Error::kOk, vault::Unlock(kStrongPin));  // the key kept

    vault::SetPolicy({600, "", false});
    TEST_ASSERT_EQUAL(Error::kOk, vault::ChangePin(kStrongPin, kWeakPin));
    vault::Lock();
    TEST_ASSERT_EQUAL(Error::kOk, vault::Unlock(kWeakPin));
}

void test_vault_reset_with_a_new_key_refuses_a_weak_pin(void) {
    FreshVault(true);
    TEST_ASSERT_EQUAL(Error::kWeakPin, vault::Reset(kWeakPin));
    TEST_ASSERT_FALSE(vault::GetInfo().has_key);
    TEST_ASSERT_EQUAL(0, vault::GetInfo().file_size);
    TEST_ASSERT_EQUAL(Error::kOk, vault::Reset(kStrongPin));
    TEST_ASSERT_TRUE(vault::GetInfo().state == vault::State::kOpen);

    FreshVault(false);
    TEST_ASSERT_EQUAL(Error::kOk, vault::Reset(kWeakPin));
    TEST_ASSERT_TRUE(vault::GetInfo().state == vault::State::kOpen);
}

void test_vault_a_kept_key_opens_the_store_without_a_pin(void) {
    FreshVault(true);
    vault::Key key;
    TEST_ASSERT_FALSE(vault::CopyKey(&key));
    TEST_ASSERT_EQUAL(Error::kOk, vault::Unlock(kStrongPin));
    TEST_ASSERT_EQUAL(Error::kOk, vault::Add("site", "u", "pw", "", nullptr));
    TEST_ASSERT_TRUE(vault::CopyKey(&key));
    vault::Lock();
    TEST_ASSERT_EQUAL(Error::kOk, vault::Open(key));
    TEST_ASSERT_TRUE(vault::GetInfo().state == vault::State::kOpen);
    TEST_ASSERT_EQUAL_size_t(1, vault::GetInfo().count);
    TEST_ASSERT_EQUAL(Error::kOk, vault::Open(key));  // already in: the same key

    vault::Key other = key;
    other.key[0] ^= 1;
    TEST_ASSERT_EQUAL(Error::kLocked, vault::Open(other));  // in: another key refused
    vault::Lock();
    TEST_ASSERT_EQUAL(Error::kLocked, vault::Open(other));  // the file's key id differs
    TEST_ASSERT_TRUE(vault::GetInfo().state == vault::State::kLocked);
    TEST_ASSERT_FALSE(vault::GetInfo().has_key);
    TEST_ASSERT_EQUAL(0, vault::GetInfo().pin_failures);  // nothing counted
    TEST_ASSERT_EQUAL(Error::kOk, vault::Unlock(kStrongPin));
}

// --- seed_phrase.h: the BIP39 vectors (trezor/python-mnemonic, 128 bits)

struct SeedVector {
    uint8_t byte;  // the entropy: 16 times this (0x9e...: see below)
    uint8_t checksum;
    const char *words;
};

void test_seed_bip39_vectors(void) {
    const SeedVector vectors[] = {
        {0x00, 3, "abandon abandon abandon abandon abandon abandon abandon abandon abandon "
                  "abandon abandon about"},
        {0x7f, 8, "legal winner thank year wave sausage worth useful legal winner thank yellow"},
        {0x80, 4, "letter advice cage absurd amount doctor acoustic avoid letter advice cage "
                  "above"},
        {0xff, 5, "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong"},
    };
    for (const SeedVector &v : vectors) {
        uint8_t entropy[vault::seed::kEntropySize];
        std::memset(entropy, v.byte, sizeof entropy);
        char text[vault::seed::kTextSize];
        vault::seed::Encode(entropy, v.checksum, text);
        TEST_ASSERT_EQUAL_STRING(v.words, text);
        uint8_t back[vault::seed::kEntropySize];
        uint8_t checksum = 0xFF;
        TEST_ASSERT_TRUE(vault::seed::Parse(v.words, back, &checksum));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(entropy, back, sizeof entropy);
        TEST_ASSERT_EQUAL_UINT8(v.checksum, checksum);
    }
    const uint8_t mixed[vault::seed::kEntropySize] = {0x9e, 0x88, 0x5d, 0x95, 0x2a, 0xd3,
                                                      0x62, 0xca, 0xeb, 0x4e, 0xfe, 0x34,
                                                      0xa8, 0xe9, 0x1b, 0xd2};
    char text[vault::seed::kTextSize];
    vault::seed::Encode(mixed, 1, text);
    TEST_ASSERT_EQUAL_STRING(
        "ozone drill grab fiber curtain grace pudding thank cruise elder eight picnic", text);
}

void test_seed_entry_is_forgiving_but_exact(void) {
    uint8_t entropy[vault::seed::kEntropySize];
    uint8_t checksum = 0;
    // Any case, extra spaces, 4+ letter starts.
    TEST_ASSERT_TRUE(vault::seed::Parse("  OZON drill\tGRAB fibe curt grace pudd thank cruise "
                                        "elder eigh picnic\n",
                                        entropy, &checksum));
    TEST_ASSERT_EQUAL_UINT8(0x9e, entropy[0]);
    TEST_ASSERT_EQUAL_UINT8(1, checksum);
    TEST_ASSERT_EQUAL(0, vault::seed::Find("abandon", 7));
    TEST_ASSERT_EQUAL(0, vault::seed::Find("aban", 4));
    TEST_ASSERT_EQUAL(2047, vault::seed::Find("zoo", 3));
    TEST_ASSERT_EQUAL(-1, vault::seed::Find("aba", 3));      // 3 letters: a whole word only
    TEST_ASSERT_EQUAL(-1, vault::seed::Find("abandox", 7));  // the rest must match too
    TEST_ASSERT_EQUAL(-1, vault::seed::Find("zoos", 4));
    TEST_ASSERT_EQUAL(-1, vault::seed::Find("", 0));
    // 11 and 13 words, an unknown word: refused.
    TEST_ASSERT_FALSE(vault::seed::Parse("zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong", entropy,
                                         &checksum));
    TEST_ASSERT_FALSE(vault::seed::Parse("zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong",
                                         entropy, &checksum));
    TEST_ASSERT_FALSE(vault::seed::Parse("zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo bitcoin",
                                         entropy, &checksum));
    TEST_ASSERT_FALSE(vault::seed::Parse("", entropy, &checksum));
}

// --- vault.cpp with the seed phrase

void test_vault_a_new_store_makes_a_seed_phrase(void) {
    FreshVault(true);
    TEST_ASSERT_EQUAL(Error::kOk, vault::Unlock(kStrongPin));
    const vault::Info info = vault::GetInfo();
    TEST_ASSERT_TRUE(info.seed_set);  // made with the first store (kept in memory since)
    TEST_ASSERT_EQUAL_UINT8_ARRAY(info.seed_id, info.file_seed_id, sizeof info.seed_id);
    static const uint8_t kNone[8] = {};
    TEST_ASSERT_FALSE(std::memcmp(info.store_id, kNone, sizeof kNone) == 0);
    // The store id stays through writes and a PIN change.
    uint8_t store_id[8];
    std::memcpy(store_id, info.store_id, sizeof store_id);
    TEST_ASSERT_EQUAL(Error::kOk, vault::Add("site", "u", "pw", "", nullptr));
    TEST_ASSERT_EQUAL(Error::kOk, vault::ChangePin(kStrongPin, "583921"));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(store_id, vault::GetInfo().store_id, sizeof store_id);
    TEST_ASSERT_EQUAL(Error::kOk, vault::ChangePin("583921", kStrongPin));
}

void test_vault_another_seed_phrase_does_not_open_the_store_nor_counts(void) {
    FreshVault(true);
    char mine[vault::seed::kTextSize];
    char other[vault::seed::kTextSize];
    // No store yet: a random phrase is allowed while locked.
    TEST_ASSERT_EQUAL(Error::kOk, vault::SetSeed(nullptr, nullptr, other));
    TEST_ASSERT_EQUAL(Error::kOk, vault::Unlock(kStrongPin));  // makes the store with `other`
    TEST_ASSERT_EQUAL(Error::kOk, vault::Add("site", "u", "pw", "", nullptr));
    TEST_ASSERT_EQUAL(Error::kWrongPin, vault::CopySeed("111111", mine));
    TEST_ASSERT_EQUAL(1, vault::GetInfo().pin_failures);
    TEST_ASSERT_EQUAL(Error::kOk, vault::CopySeed(kStrongPin, mine));
    TEST_ASSERT_EQUAL_STRING(other, mine);
    // Open: a new phrase needs the PIN, then re-keys the store.
    TEST_ASSERT_EQUAL(Error::kBadPin, vault::SetSeed(nullptr, nullptr, other));
    TEST_ASSERT_EQUAL(Error::kOk, vault::SetSeed(kStrongPin, nullptr, other));
    TEST_ASSERT_FALSE(std::strcmp(mine, other) == 0);
    vault::Lock();
    // Locked with a store: a random phrase is refused, a given one taken.
    TEST_ASSERT_EQUAL(Error::kLocked, vault::SetSeed(nullptr, nullptr, nullptr));
    TEST_ASSERT_EQUAL(Error::kBadSeed, vault::SetSeed(nullptr, "zoo zoo", nullptr));
    TEST_ASSERT_EQUAL(Error::kOk, vault::SetSeed(nullptr, mine, nullptr));  // the old one
    TEST_ASSERT_EQUAL(Error::kWrongSeed, vault::Unlock(kStrongPin));
    TEST_ASSERT_EQUAL(0, vault::GetInfo().pin_failures);  // nothing counted
    TEST_ASSERT_EQUAL(Error::kOk, vault::SetSeed(nullptr, other, nullptr));
    TEST_ASSERT_EQUAL(Error::kOk, vault::Unlock(kStrongPin));
    TEST_ASSERT_EQUAL_size_t(1, vault::GetInfo().count);
}

void test_vault_the_wipe_pin_erases_the_seed_phrase(void) {
    FreshVault(true);
    TEST_ASSERT_EQUAL(Error::kOk, vault::Unlock(kStrongPin));
    char wipe[vault::kWipeTextSize];
    TEST_ASSERT_EQUAL(Error::kOk, vault::MakeWipe("902614", wipe));
    vault::SetPolicy({600, wipe, true});
    vault::Lock();
    TEST_ASSERT_EQUAL(Error::kWrongPin, vault::Unlock("902614"));
    TEST_ASSERT_FALSE(vault::GetInfo().seed_set);
    TEST_ASSERT_FALSE(std::filesystem::exists(kSeedPath));
    vault::SetPolicy({600, "", true});
    TEST_ASSERT_EQUAL(Error::kOk, vault::Unlock(kStrongPin));  // a new store, a new phrase
    TEST_ASSERT_TRUE(vault::GetInfo().seed_set);
    TEST_ASSERT_TRUE(std::filesystem::exists(kSeedPath));
}

}  // namespace

void RegisterVaultTests(void) {
    RUN_TEST(test_vault_add_goes_last_and_sort_by_name);
    RUN_TEST(test_vault_move);
    RUN_TEST(test_vault_the_same_user_on_a_site_is_a_duplicate);
    RUN_TEST(test_vault_fields_are_checked);
    RUN_TEST(test_vault_otp_is_normalized_base32);
    RUN_TEST(test_vault_otp_from_base64_is_kept_as_base32);
    RUN_TEST(test_vault_full_store_refuses);
    RUN_TEST(test_vault_set_keeps_the_place_and_the_id);
    RUN_TEST(test_vault_remove_and_ids_never_repeat);
    RUN_TEST(test_vault_round_trip);
    RUN_TEST(test_vault_parse_refuses_malformed_data);
    RUN_TEST(test_vault_pin_is_six_digits);
    RUN_TEST(test_vault_no_lock_below_ten_failures);
    RUN_TEST(test_vault_lock_resumes_where_it_was_with_a_known_clock);
    RUN_TEST(test_vault_lock_starts_again_without_a_clock_or_with_one_moved_back);
    RUN_TEST(test_vault_wipe_text_round_trips);
    RUN_TEST(test_vault_wipe_text_must_be_exact);
    RUN_TEST(test_vault_clear_wipes_secrets);
    RUN_TEST(test_vault_new_store_refuses_a_weak_pin);
    RUN_TEST(test_vault_change_pin_refuses_a_weak_pin);
    RUN_TEST(test_vault_reset_with_a_new_key_refuses_a_weak_pin);
    RUN_TEST(test_vault_a_kept_key_opens_the_store_without_a_pin);
    RUN_TEST(test_seed_bip39_vectors);
    RUN_TEST(test_seed_entry_is_forgiving_but_exact);
    RUN_TEST(test_vault_a_new_store_makes_a_seed_phrase);
    RUN_TEST(test_vault_another_seed_phrase_does_not_open_the_store_nor_counts);
    RUN_TEST(test_vault_the_wipe_pin_erases_the_seed_phrase);
}
