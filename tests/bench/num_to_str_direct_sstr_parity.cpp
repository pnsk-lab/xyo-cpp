// Full numerical cast parity: the on-stack ASCII buffer must reach SStr
// without changing text, numeric state, or signed-zero semantics.
#include "core/cast.hpp"
#include "core/gc.hpp"
#include "core/jsnum.hpp"
#include "core/sstr.hpp"
#include "core/value.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>

using namespace xyo;

static bool check(double value) {
    const auto expected = jsnum::toString(value);
    char buffer[72];
    std::memset(buffer, 0x5a, sizeof(buffer));
    const size_t length = jsnum::toChars(value, buffer + 4);
    if (length != expected.size() ||
        std::memcmp(buffer + 4, expected.data(), length) != 0 ||
        buffer[3] != char(0x5a) || buffer[68] != char(0x5a)) {
        std::printf("num-to-buffer mismatch: %a\n", value);
        return false;
    }

    // SStr may use the small-integer cache, or create a fresh GC object.
    SStr* result = cast::toString(Value::numCanonical(value));
    if (result->len != expected.size()) {
        std::printf("num-to-SStr length mismatch: %a\n", value);
        return false;
    }
    const char16_t* data = result->data();
    for (size_t index = 0; index < expected.size(); ++index) {
        if (data[index] != static_cast<unsigned char>(expected[index])) {
            std::printf("num-to-SStr text mismatch: %a\n", value);
            return false;
        }
    }

    if (std::isfinite(value)) {
        if (result->loadNumState() != 1 ||
            std::bit_cast<uint64_t>(result->loadNumCache()) !=
                std::bit_cast<uint64_t>(value == 0 ? 0.0 : value)) {
            std::printf("num-to-SStr numeric cache mismatch: %a\n", value);
            return false;
        }
    } else if (std::isnan(value)) {
        if (result->loadNumState() != 2) return false;
    } else {
        if (result->loadNumState() != 4) return false;
    }
    return true;
}

int main() {
    Gc::DeferCollect noCollection;
    constexpr std::array<double, 20> edges = {
        0.0, -0.0, 1.0, -1.0, 1023.0, 1024.0,
        1e-6, -1e-6, 1e-7, -1e-7, 1e20, 1e21,
        9007199254740991.0, 9007199254740992.0,
        std::numeric_limits<double>::min(),
        std::numeric_limits<double>::denorm_min(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(),
    };
    for (double x : edges) if (!check(x)) return 1;
    std::mt19937_64 generator(0xA93DC7150B2ull);
    for (int i=0;i<100000;++i) {
        const double value=std::bit_cast<double>(generator());
        if (!check(value)) return 2;
    }
    std::puts("PASS 100,020 Number->ASCII buffer->SStr parity and cache states");
}
