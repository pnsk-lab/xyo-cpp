// Compare radix parsing independently from the Scratch runtime.
// Build against a saved baseline src/core/jsnum.cpp and against the
// modified implementation, using the same C++ compiler and options.
#include "core/jsnum.hpp"

#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

static std::u16string makeValue(unsigned index, unsigned radix,
                                unsigned digits, bool invalid = false) {
    std::u16string s = radix == 16 ? u"0x" :
                       radix == 8 ? u"0o" : u"0b";
    for (unsigned digit = 0; digit < digits; ++digit) {
        unsigned value = (index * 17u + digit * 13u) % radix;
        char16_t c = value < 10 ? u'0' + value : u'A' + (value - 10);
        if (index % 11 == 0 && digit < digits / 2) c = u'0';
        s.push_back(c);
    }
    if (invalid) {
        s[index % s.size()] = (index % 2) ? u'G' : u'x';
    }
    return s;
}

int main() {
    for (const auto& [name, radix, digits, invalid] :
        std::vector<std::tuple<const char*, unsigned, unsigned, bool>>{
            {"hex-short", 16, 8, false},
            {"hex-long", 16, 25, false},
            {"binary", 2, 44, false},
            {"octal", 8, 16, false},
            {"zero-prefix", 16, 36, false},
            {"invalid", 16, 8, true},
        }) {
        std::vector<std::u16string> inputs;
        inputs.reserve(512);
        for (unsigned i = 0; i < 512; ++i)
            inputs.push_back(makeValue(i, radix, digits, invalid));
        uint64_t checksum = 0;
        constexpr int kRounds = 2000;
        const auto begin = std::chrono::steady_clock::now();
        for (int round = 0; round < kRounds; ++round) {
            for (const auto& input : inputs)
                checksum += std::bit_cast<uint64_t>(
                    xyo::jsnum::toNumber(input));
        }
        const auto ns = std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - begin).count();
        std::printf("%s %.3f ns/op checksum=%016llx\n",
            name, ns / (kRounds * inputs.size()),
            static_cast<unsigned long long>(checksum));
    }
}
