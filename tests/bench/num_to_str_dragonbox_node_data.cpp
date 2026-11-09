// xyo V8 Dragonbox: deterministic data for byte-for-byte comparison
// with actual Node.js/V8 NumberToString. Output:
//   16 hex bits + space + ECMAScript decimal text + newline
#include "core/jsnum.hpp"
#include <bit>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>

static void emit(double v) {
    auto text = xyo::jsnum::toString(v);
    std::printf("%016llx %s\n",
        static_cast<unsigned long long>(std::bit_cast<uint64_t>(v)),
        text.c_str());
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
    for(double value:boundaries) emit(value);
    std::mt19937_64 rng(0x1921AC6543ull);
    for(int i=0;i<300000;i++) emit(std::bit_cast<double>(rng()));
    for(int i=0;i<300000;i++) {
        uint64_t n = rng()%9007199254740993ull;
        emit(double(i&1 ? -int64_t(n) : int64_t(n)));
    }
    for(int i=-999;i<=999;i++) emit(double(i));
}
