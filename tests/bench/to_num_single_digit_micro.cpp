// Standalone benchmark for one-character and common small numeric strings.
#include "core/jsnum.hpp"

#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

int main() {
    for (const char* workload : {"digit", "signed-digit", "two-digit",
                                 "fraction", "space-digit", "invalid"}) {
        std::vector<std::u16string> strings;
        strings.reserve(256);
        for (unsigned i = 0; i < 256; ++i) {
            char16_t c = u'0' + i % 10;
            if (workload[0] == 'd')
                strings.emplace_back(1, c);
            else if (workload[0] == 's' && workload[1] == 'i')
                strings.emplace_back(std::u16string(1, i % 2 ? u'-' : u'+') + c);
            else if (workload[0] == 't')
                strings.emplace_back(std::u16string(1, c) +
                                     char16_t(u'0' + (i * 7) % 10));
            else if (workload[0] == 'f')
                strings.emplace_back(std::u16string(1, c) +
                                     u"." + char16_t(u'0' + (i * 7) % 10));
            else if (workload[0] == 's')
                strings.emplace_back(u"  " + std::u16string(1,c) + u" ");
            else
                strings.emplace_back(1, u'x');
        }
        volatile uint64_t sink = 0;
        uint64_t checksum = 0;
        constexpr int rounds = 10000;
        auto start = std::chrono::steady_clock::now();
        for (int j = 0; j < rounds; ++j) {
            for (const auto& string : strings)
                checksum += std::bit_cast<uint64_t>(xyo::jsnum::toNumber(string));
        }
        sink = checksum;
        double elapsed = std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - start).count();
        std::printf("%s %.3f ns/op checksum=%016llx\n",
                    workload, elapsed / (rounds * strings.size()),
                    static_cast<unsigned long long>(sink));
    }
}
