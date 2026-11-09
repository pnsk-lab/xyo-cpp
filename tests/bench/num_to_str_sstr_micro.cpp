// Microbenchmark of the actual Scratch number-to-SStr conversion.
// Run the binary separately for each backend; no per-iteration logging.
#include "core/cast.hpp"
#include "core/gc.hpp"
#include "core/sstr.hpp"
#include "core/value.hpp"
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string_view>
#include <cstring>

using namespace xyo;
static volatile uint64_t sink = 0;

int main() {
    Gc::DeferCollect noCollection;
    for (const char* name : {"fraction", "extreme", "long-int", "short"}) {
        std::array<double, 256> samples{};
        uint64_t seed = 0xA93DC7150B2ull;
        for (unsigned i=0;i<samples.size();i++) {
            seed ^= seed>>12;
            seed ^= seed<<25;
            seed ^= seed>>27;
            const uint64_t bits=seed*0x2545F4914F6CDD1Dull;
            if (std::strcmp(name,"fraction")==0) {
                samples[i]=static_cast<double>(bits%1000000000ull)/7.0;
            } else if (std::strcmp(name,"extreme")==0) {
                samples[i]=std::bit_cast<double>(bits);
            } else if (std::strcmp(name,"long-int")==0) {
                samples[i]=static_cast<double>(bits%9007199254740992ull);
            } else {
                samples[i]=static_cast<double>(i%32)/4.0;
            }
        }
        const int iterations=300;
        uint64_t checksum=0;
        const auto begin=std::chrono::steady_clock::now();
        for(int round=0;round<iterations;round++) {
            for(double value:samples) {
                SStr* s=cast::toString(Value::numCanonical(value));
                checksum += s->len + s->unitAt(0);
            }
        }
        sink=checksum;
        const double elapsed=std::chrono::duration<double,std::nano>(
            std::chrono::steady_clock::now()-begin).count();
        std::printf("%s %.3f ns/op sum=%016llx\n",
                    name,elapsed/(iterations*samples.size()),
                    static_cast<unsigned long long>(sink));
    }
}
