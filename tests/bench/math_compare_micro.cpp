// Reproducible pre/post microbenchmark for Scratch comparison semantics.
// Usage: ./math-compare-micro [repetitions]; output is ns/compare.
#include "core/cast.hpp"
#include "core/gc.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace xyo;
int main(int argc, char** argv) {
    Gc::DeferCollect defer;
    int repeats = argc > 1 ? std::atoi(argv[1]) : 12000000;
    if (repeats <= 0) return 2;
    const Value x = Value::num(2048.0), y = Value::num(1024.0);
    const Value x2 = Value::num(2049.0), y2 = Value::num(1025.0);
    const Value numstr1 = Value::string(SStr::create(u"2048"));
    const Value numstr2 = Value::string(SStr::create(u"1024"));
    const Value word1 = Value::string(SStr::create(u"Hello World"));
    const Value word2 = Value::string(SStr::create(u"HELLO WORLD"));
    const Value space = Value::string(SStr::create(u"  "));
    const Value word3 = Value::string(SStr::create(u"Goodbye"));
    const Value inf = Value::num(std::numeric_limits<double>::infinity());
    struct Case { const char* name; Value a[2], b[2]; };
    const std::array<Case, 7> cases{{
        {"numeric", {x, x2}, {y, y2}},
        {"equal_numeric", {x,x2}, {x, x2}},
        {"cached_numeric_strings", {numstr1,numstr2}, {numstr2,numstr1}},
        {"casefolded_strings", {word1,word1}, {word2,word3}},
        {"mixed_number_text", {x, y}, {numstr1, numstr2}},
        {"lexical_zero", {space,space}, {Value::num(0), Value::emptyStr()}},
        {"infinity", {inf,inf}, {inf,Value::num(-1)}},
    }};
    volatile int64_t sink = 0;
    for (const auto& cs : cases) {
        // Warm up the immutable string numeric cache, without timing its cold parse.
        for (int k=0;k<2;++k) (void)cast::compare(cs.a[k],cs.b[k]);
        auto begin = std::chrono::steady_clock::now();
        int64_t n = 0;
        for (int i=0; i<repeats; ++i) {
            double r=cast::compare(cs.a[i&1],cs.b[i&1]);
            n += (r > 0) - (r < 0);
        }
        auto duration = std::chrono::duration<double,std::nano>(
            std::chrono::steady_clock::now()-begin).count();
        sink += n;
        std::printf("%s %.3f ns/compare checksum=%lld\n",cs.name,
                    duration/repeats,static_cast<long long>(n));
    }
    std::fprintf(stderr, "sink=%lld\n",static_cast<long long>(sink));
}
