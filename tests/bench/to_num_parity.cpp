// Generate a binary stream of ES numeric string results for baseline-vs-fast
// parser bitwise parity. Build once against HEAD's jsnum.cpp and once against
// the current source; compare the stdout byte streams with cmp.
#include "core/jsnum.hpp"
#include <bit>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

int main() {
    std::vector<std::u16string> values{
        u"", u" ", u"\u3000\u00A0", u"-0", u" -0 ", u"+0",
        u"0", u"000000", u"000000000000000000000000001",
        u"9007199254740991", u"9007199254740992",
        u"9007199254740993", u"999999999999999",
        u"9007199254740992.5", u"9007199254740993.0",
        u"1e309", u"-1e309", u"1e-324", u"-1e-324",
        u"4.9406564584124654e-324", u"2.2250738585072014e-308",
        u"0x10", u"0b1011", u"0o077", u"-0x10",
        u"Infinity", u"-Infinity", u"+Infinity",
        u"infinity", u"NaN", u"nan", u"  +.5 ", u" 1. ",
        u"1.e3", u".01e-1", u"1e", u"0x", u"0xG",
        u"1\u3000", u"\uFEFF-3.5\u2029", u"1 2",
    };

    std::mt19937_64 rng(0x5E7AF009b13cull);
    constexpr auto digit = [](uint64_t n) {
        return static_cast<char16_t>(u'0' + (n % 10));
    };
    for (int test = 0; test < 240000; ++test) {
        std::u16string s;
        if (test % 9 == 0) s += u" \t";
        if (test % 13 == 0) s += u"\u3000";
        if (test % 3 == 0) s += u'-';
        else if (test % 7 == 0) s += u'+';
        int length = int(rng() % 28 + 1);
        for (int i = 0; i < length; ++i) s += digit(rng());
        if (test % 5 == 0) s.insert(size_t(rng() % (length + 1)), u".");
        if (test % 4 == 0) {
            s += (test % 2 == 0 ? u'e' : u'E');
            if (test % 8 == 0) s += u'+';
            else if (test % 11 == 0) s += u'-';
            int expLen = int(rng() % 4 + 1);
            for (int i = 0; i < expLen; ++i) s += digit(rng());
        }
        if (test % 23 == 0) s += u"hello";
        if (test % 17 == 0) s += u"\u2028";
        if (test % 29 == 0) s.insert(0, u"  ");
        if (test % 37 == 0) s.insert(0, u"0x");
        values.push_back(std::move(s));
    }
    for (const auto& s : values) {
        double d = xyo::jsnum::toNumber(s);
        uint64_t bits = std::bit_cast<uint64_t>(d);
        if (std::fwrite(&bits, sizeof(bits), 1, stdout) != 1) return 2;
    }
    // Short decimal syntax can be deceptively permissive. Exhaust every
    // combination of representative letters, digits and punctuation through
    // length five so a new from_chars path cannot accept a JS-invalid form.
    constexpr std::u16string_view alphabet = u"01. eE+-xXbBoOnNiIfF";
    uint64_t count = 1;
    for (size_t length = 1; length <= 5; ++length) {
        count *= alphabet.size();
        for (uint64_t index = 0; index < count; ++index) {
            std::u16string literal(length, u' ');
            uint64_t bitsIndex = index;
            for (size_t position = 0; position < length; ++position) {
                literal[position] = alphabet[bitsIndex % alphabet.size()];
                bitsIndex /= alphabet.size();
            }
            const uint64_t bits = std::bit_cast<uint64_t>(
                xyo::jsnum::toNumber(literal));
            if (std::fwrite(&bits, sizeof(bits), 1, stdout) != 1) return 2;
        }
    }

    // Radix fast path: compare against the previous FP recurrence across the
    // exact-integer boundary (2^53), leading zeros, long literals, and
    // malformed digits. Keep the seed fixed so the binary reference is
    // reproducible. This deliberately includes values that go through
    // the unchanged long-literal fallback.
    for (unsigned index = 0; index < 250000; ++index) {
        unsigned radix = index % 3 == 0 ? 16 :
                         index % 3 == 1 ? 8 : 2;
        std::u16string literal =
            radix == 16 ? u"0x" : radix == 8 ? u"0o" : u"0b";
        const unsigned size = 1 + unsigned(rng() % 145);
        const unsigned leadingZeroes = index % 7 == 0
            ? unsigned(rng() % 50) : 0;
        for (unsigned j = 0; j < size; ++j) {
            unsigned digit = unsigned(rng() % radix);
            if (j < leadingZeroes) digit = 0;
            const char16_t value = digit < 10 ? u'0' + digit :
                                  (index % 2 == 0 ? u'a' : u'A') +
                                      (digit - 10);
            literal.push_back(value);
        }
        if (index % 101 == 0) literal.push_back(u'G');
        if (index % 103 == 0) literal.insert(0, 1, u' ');
        if (index % 107 == 0) literal.push_back(u' ');
        if (index % 109 == 0) literal.push_back(u'\\');
        if (index % 113 == 0) literal.insert(0, 1, u'-');
        double result = xyo::jsnum::toNumber(literal);
        const uint64_t bits = std::bit_cast<uint64_t>(result);
        if (std::fwrite(&bits, sizeof(bits), 1, stdout) != 1) return 2;
    }
}
