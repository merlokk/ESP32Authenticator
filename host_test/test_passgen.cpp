// The password generator (components/passgen): the word lists as on
// merlokk.com/password-generator, uniform draws, what a password is made of,
// its strength.

#include <cstring>
#include <string>

#include "passgen.h"
#include "passgen_words.h"
#include "unity.h"

namespace {

using passgen::List;
using passgen::Options;

// A made-up source: the numbers given, then a counter.
struct Script {
    const uint32_t *values;
    size_t count;
    size_t at;
    uint32_t next;
};

uint32_t Scripted(void *ctx) {
    auto *s = static_cast<Script *>(ctx);
    if (s->at < s->count) {
        return s->values[s->at++];
    }
    return s->next++;
}

// A simple LCG: enough spread for the shape checks.
uint32_t Lcg(void *ctx) {
    auto *x = static_cast<uint32_t *>(ctx);
    *x = *x * 1664525u + 1013904223u;
    return *x;
}

void test_passgen_lists(void) {
    struct Want {
        List list;
        size_t count;
        size_t starts;  // distinct 3-letter starts, as the site counts them
        const char *first;
        const char *last;
    };
    const Want wants[] = {{List::kEff, 7772, 1373, "abacus", "zoom"},
                          {List::kBip39, 2048, 1096, "abandon", "zoo"},
                          {List::kSlip39, 1024, 657, "academic", "zero"}};
    for (const Want &w : wants) {
        const passgen::Words words = passgen::WordsOf(w.list);
        TEST_ASSERT_EQUAL_size_t(w.count, words.count);
        TEST_ASSERT_EQUAL_STRING(w.first, words.word[0]);
        TEST_ASSERT_EQUAL_STRING(w.last, words.word[words.count - 1]);
        TEST_ASSERT_EQUAL_size_t(w.count, passgen::PoolSize(words, false));
        TEST_ASSERT_EQUAL_size_t(w.starts, passgen::PoolSize(words, true));
        for (size_t i = 0; i < words.count; ++i) {
            const size_t n = std::strlen(words.word[i]);
            TEST_ASSERT_TRUE(n >= 3 && n <= passgen::kMaxWord);
            for (size_t j = 0; j < n; ++j) {
                TEST_ASSERT_TRUE(words.word[i][j] >= 'a' && words.word[i][j] <= 'z');
            }
            // Sorted, no repeats: a start's words are together.
            TEST_ASSERT_TRUE(i == 0 || std::strcmp(words.word[i - 1], words.word[i]) < 0);
        }
    }
}

void test_passgen_pool_items(void) {
    const passgen::Words bip = passgen::WordsOf(List::kBip39);
    size_t len = 0;
    TEST_ASSERT_EQUAL_STRING("abandon", passgen::PoolItem(bip, false, 0, &len));
    TEST_ASSERT_EQUAL_size_t(7, len);
    // abandon, ability, able: "aba", "abi", "abl".
    const char *w = passgen::PoolItem(bip, true, 2, &len);
    TEST_ASSERT_EQUAL_size_t(3, len);
    TEST_ASSERT_EQUAL_STRING_LEN("abl", w, 3);
    w = passgen::PoolItem(bip, true, passgen::PoolSize(bip, true) - 1, &len);
    TEST_ASSERT_EQUAL_STRING_LEN("zoo", w, 3);
}

void test_passgen_uniform_rejects_the_tail(void) {
    // n = 3: 0xFFFFFFFF is past the last whole multiple (0xFFFFFFFF = 3 * 0x55555555).
    const uint32_t values[] = {0xFFFFFFFFu, 7};
    Script s = {values, 2, 0, 0};
    TEST_ASSERT_EQUAL_UINT32(1, passgen::Uniform(3, Scripted, &s));
    TEST_ASSERT_EQUAL_size_t(2, s.at);
    // A power of two takes everything.
    const uint32_t top[] = {0xFFFFFFFFu};
    s = {top, 1, 0, 0};
    TEST_ASSERT_EQUAL_UINT32(1023, passgen::Uniform(1024, Scripted, &s));
    TEST_ASSERT_EQUAL_UINT32(0, passgen::Uniform(1, Scripted, &s));
}

void test_passgen_generate(void) {
    // Words 0, 1, 2 of BIP-39, '-' between.
    Options o;
    o.words = 3;
    o.list = List::kBip39;
    const passgen::Words bip = passgen::WordsOf(List::kBip39);
    Script s = {nullptr, 0, 0, 0};
    char out[passgen::kTextSize];
    TEST_ASSERT_EQUAL_size_t(20, passgen::Generate(o, bip, Scripted, &s, out));
    TEST_ASSERT_EQUAL_STRING("abandon-ability-able", out);

    // Capitals, no separator, 3-letter starts, a digit after word 1, a symbol after word 0.
    o.capitalize = true;
    o.separator = static_cast<uint8_t>(passgen::FindSeparator(""));
    o.prefix = true;
    o.digit = true;
    o.symbol = true;
    const uint32_t values[] = {0, 1, 2, /*digit*/ 1, 7, /*symbol*/ 0, 2};
    s = {values, 7, 0, 0};
    passgen::Generate(o, bip, Scripted, &s, out);
    TEST_ASSERT_EQUAL_STRING("Aba#Abi7Abl", out);

    // Space between.
    o = Options{};
    o.words = 3;
    o.list = List::kBip39;
    o.separator = static_cast<uint8_t>(passgen::FindSeparator(" "));
    s = {nullptr, 0, 0, 0};
    passgen::Generate(o, bip, Scripted, &s, out);
    TEST_ASSERT_EQUAL_STRING("abandon ability able", out);
}

void test_passgen_generate_shape(void) {
    char out[passgen::kTextSize];
    uint32_t seed = 1;
    Options o;
    o.words = passgen::kMaxWords;
    o.separator = static_cast<uint8_t>(passgen::FindSeparator(" "));
    o.digit = true;
    o.symbol = true;
    for (int round = 0; round < 200; ++round) {
        const size_t n = passgen::Generate(o, passgen::WordsOf(List::kEff), Lcg, &seed, out);
        TEST_ASSERT_EQUAL_size_t(std::strlen(out), n);
        TEST_ASSERT_TRUE(n < passgen::kTextSize);
        size_t spaces = 0;
        size_t digits = 0;
        size_t symbols = 0;
        for (size_t i = 0; i < n; ++i) {
            spaces += out[i] == ' ';
            digits += out[i] >= '0' && out[i] <= '9';
            symbols += std::strchr(passgen::kSymbols, out[i]) != nullptr;
        }
        TEST_ASSERT_EQUAL_size_t(passgen::kMaxWords - 1, spaces);
        TEST_ASSERT_EQUAL_size_t(1, digits);
        TEST_ASSERT_EQUAL_size_t(1, symbols);
    }
    // The longest it can be fits: the words, 1-character separators, a digit, a symbol.
    TEST_ASSERT_TRUE(passgen::kMaxWords * passgen::kMaxWord + passgen::kMaxWords - 1 + 2 <
                     passgen::kTextSize);
}

void test_passgen_strength(void) {
    TEST_ASSERT_EQUAL_UINT32(0, passgen::Log2Milli(1));
    TEST_ASSERT_EQUAL_UINT32(1000, passgen::Log2Milli(2));
    TEST_ASSERT_EQUAL_UINT32(10000, passgen::Log2Milli(1024));
    TEST_ASSERT_UINT32_WITHIN(1, 3322, passgen::Log2Milli(10));
    TEST_ASSERT_UINT32_WITHIN(1, 12924, passgen::Log2Milli(7772));
    TEST_ASSERT_UINT32_WITHIN(1, 10098, passgen::Log2Milli(1096));
    Options o;  // 5 EFF words: 64.6 bits
    TEST_ASSERT_UINT32_WITHIN(5, 64621, passgen::MilliBits(o, 7772));
    TEST_ASSERT_EQUAL_STRING("Strong", passgen::StrengthLabel(passgen::MilliBits(o, 7772)));
    o.digit = true;  // + log2(10) + log2(5)
    TEST_ASSERT_UINT32_WITHIN(5, 64621 + 3322 + 2322, passgen::MilliBits(o, 7772));
    o = Options{};
    o.words = 3;
    TEST_ASSERT_EQUAL_STRING("Weak", passgen::StrengthLabel(passgen::MilliBits(o, 1024)));
    o.words = 12;
    TEST_ASSERT_EQUAL_STRING("Very strong", passgen::StrengthLabel(passgen::MilliBits(o, 7772)));
}

void test_passgen_names(void) {
    TEST_ASSERT_EQUAL_INT(0, passgen::FindList("eff"));
    TEST_ASSERT_EQUAL_INT(2, passgen::FindList("slip39"));
    TEST_ASSERT_EQUAL_INT(-1, passgen::FindList("ef"));
    TEST_ASSERT_EQUAL_INT(-1, passgen::FindList("effx"));
    TEST_ASSERT_EQUAL_INT(0, passgen::FindSeparator("-"));
    TEST_ASSERT_EQUAL_INT(3, passgen::FindSeparator(" "));
    TEST_ASSERT_EQUAL_INT(4, passgen::FindSeparator(""));
    TEST_ASSERT_EQUAL_INT(-1, passgen::FindSeparator("--"));
    TEST_ASSERT_EQUAL_INT(-1, passgen::FindSeparator("/"));
}

}  // namespace

void RegisterPassgenTests(void) {
    RUN_TEST(test_passgen_lists);
    RUN_TEST(test_passgen_pool_items);
    RUN_TEST(test_passgen_uniform_rejects_the_tail);
    RUN_TEST(test_passgen_generate);
    RUN_TEST(test_passgen_generate_shape);
    RUN_TEST(test_passgen_strength);
    RUN_TEST(test_passgen_names);
}
