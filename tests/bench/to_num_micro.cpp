// Standalone, repeatable microbenchmark for ES numeric-string parsing.
// Build: g++ -std=c++20 -O3 -I src tests/bench/to_num_micro.cpp
//              src/core/jsnum.cpp -o build/to_num_micro
#include "core/jsnum.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

static std::vector<std::u16string> cases(std::string_view category) {
    std::vector<std::u16string> out;
    out.reserve(512);
    for (unsigned i = 0; i < 512; ++i) {
        std::string ascii;
        switch (category[0]) {
        case 'i':
            ascii = i % 5 == 0 ? "-0" :
                    i % 5 == 1 ? "+" + std::to_string((i * 1597u) % 900719925u) :
                    i % 5 == 2 ? "   " + std::to_string((i * 7727u) % 1000000u) + "  " :
                    i % 5 == 3 ? "-0000" + std::to_string(i * 793u) :
                    "900719925474099" + std::to_string(i % 3u);
            break;
        case 'd':
            ascii = std::to_string(i * 1237u % 100000u) + "." +
                    std::to_string(100000u - (i * 17u) % 99999u);
            break;
        case 'e':
            ascii = std::to_string(i % 97u) + "e-" + std::to_string(i % 325u);
            break;
        case 'x':
            ascii = (i % 2 ? "0x" : "0b") + std::to_string(i % 10u);
            break;
        default:
            ascii = i % 4 ? "hello-" + std::to_string(i) : " false ";
        }
        out.emplace_back(ascii.begin(), ascii.end());
    }
    return out;
}

int main() {
    for (const char* category : {"integer", "decimal", "exponent",
                                 "hex", "invalid"}) {
        auto input = cases(category);
        volatile uint64_t result = 0;
        constexpr unsigned kRounds = 2000; // 1,024,000 parses per category
        auto begin = std::chrono::steady_clock::now();
        uint64_t checksum = 0;
        for (unsigned j = 0; j < kRounds; ++j) {
            for (const auto& str : input) {
                double num = xyo::jsnum::toNumber(std::u16string_view(str));
                checksum += std::bit_cast<uint64_t>(num);
            }
        }
        result = checksum;
        auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count();
        std::printf("%s %.3f ms %.2f ns/op checksum=%016llx\n",
                    category, elapsed, elapsed * 1e6 / (kRounds * input.size()),
                    static_cast<unsigned long long>(result));
    }
}
