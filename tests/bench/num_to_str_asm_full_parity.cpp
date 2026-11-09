// Compare the ECMAScript num->str output, byte-for-byte, between the
// ordinary std::to_chars implementation and the x86-64 assembly variant.
#include "core/jsnum.hpp"
#include <bit>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <array>
#include <cmath>

static void print(double x) {
    auto result=xyo::jsnum::toString(x);
    uint8_t length=uint8_t(result.size());
    if (std::fwrite(&length,1,1,stdout)!=1) std::abort();
    if (std::fwrite(result.data(),1,length,stdout)!=length) std::abort();
}
int main() {
    constexpr std::array<double, 24> boundaries = {
        0.0,-0.0,1.0,-1.0,9.0,-9.0,10.0,-10.0,99.0,-99.0,
        100.0,-100.0,9007199254740991.0,9007199254740992.0,
        -9007199254740992.0,1e21,-1e21,1e-6,1e-7,
        std::numeric_limits<double>::denorm_min(),
        std::numeric_limits<double>::min(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(),
    };
    for(double x:boundaries) print(x);
    std::mt19937_64 gen(0x1921AC6543ull);
    for(int i=0;i<500000;i++) {
        const uint64_t bits=gen();
        print(std::bit_cast<double>(bits));
    }
    for(int i=0;i<500000;i++) {
        const uint64_t value=gen()%9007199254740993ull;
        print(double(i&1 ? -int64_t(value) : int64_t(value)));
    }
    for (int i=-999;i<=999;i++) print(double(i));
}
