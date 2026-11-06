// Measure Scratch Value->SStr conversions, including GC allocation count.
// Build: c++ -O3 -std=c++20 -Isrc tests/bench/string_common_cast_bench.cpp
//        build/libxyo.a -lz -lLLVM-23 -ldl -lSDL2 -o build/string_common_cast_bench
#include "core/cast.hpp"
#include "core/gc.hpp"
#include "core/sstr.hpp"
#include <chrono>
#include <cstdio>
#include <cstdint>

using namespace xyo;
static volatile uintptr_t observed;

int main() {
    // Roots are initially created before taking allocation measurements.
    (void)cast::toString(Value::num(1));
    // Prevent expensive collection from masking the cost of individual casts.
    Gc::DeferCollect defer;
    constexpr unsigned N = 100000;
    struct Case { const char* name; Value value; };
    for (Case sample : {Case{"empty", Value::emptyStr()},
                        Case{"true", Value::boolean(true)},
                        Case{"false", Value::boolean(false)},
                        Case{"undefined", Value::undefined()}}) {
        const auto prev = Gc::get().objectCount();
        const auto t0 = std::chrono::steady_clock::now();
        uintptr_t acc = 0;
        for (unsigned i = 0; i < N; ++i) {
            SStr* str = cast::toString(sample.value);
            acc ^= reinterpret_cast<uintptr_t>(str);
        }
        const auto t1 = std::chrono::steady_clock::now();
        observed = acc;
        const auto delta = Gc::get().objectCount() - prev;
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
        std::printf("%s: %.1f ns/cast, %zu new GC objects / %u calls\n",
                    sample.name, ns, delta, N);
    }
}
