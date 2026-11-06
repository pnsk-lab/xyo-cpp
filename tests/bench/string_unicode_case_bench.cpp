// Standalone microbenchmark of Scratch's Unicode case-insensitive operations.
// Build: c++ -O3 -DNDEBUG -std=c++20 -Isrc \
//          tests/bench/string_unicode_case_bench.cpp build/libxyo.a \
//          -lz -lLLVM-23 -ldl -lSDL2 -o build/string_unicode_case_bench
#include "core/sstr.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace xyo;
using Clock = std::chrono::steady_clock;
static volatile uint64_t sink = 0;

template<class F>
static double measure(F&& fn, unsigned rounds = 4000) {
    std::vector<double> times;
    for (int rep = 0; rep < 7; ++rep) {
        uint64_t count = 0;
        auto start = Clock::now();
        for (unsigned i = 0; i < rounds; ++i)
            count += static_cast<uint64_t>(fn());
        auto end = Clock::now();
        sink = count;
        times.push_back(
            std::chrono::duration<double, std::micro>(end - start).count());
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2] / rounds;
}

static void testContains(const char* name,
                         std::u16string hayText,
                         std::u16string needleText) {
    auto* hay = SStr::create(hayText);
    auto* needle = SStr::create(needleText);
    auto reference = [&] {
        const auto h = u16ToLower(hay->view());
        const auto n = u16ToLower(needle->view());
        return h.find(n) != std::u16string::npos;
    };
    auto optimized = [&] { return sstrContains(hay, needle); };
    if (reference() != optimized()) {
        std::fprintf(stderr, "Mismatch in %s\n", name);
        std::abort();
    }
    (void) optimized();
    const double baseline = measure(reference);
    const double fast = measure(optimized);
    std::printf("%s contains: old=%.3f us/call new=%.3f us/call speedup=%.2fx\n",
                name, baseline, fast, baseline / fast);
}

static void testCompare(const char* name,
                        std::u16string leftText,
                        std::u16string rightText) {
    auto* a = SStr::create(leftText);
    auto* b = SStr::create(rightText);
    auto reference = [&] {
        return compareStringsCaseInsensitive(a->view(), b->view());
    };
    auto optimized = [&] {
        return compareStringsCaseInsensitive(a, b);
    };
    if (reference() != optimized()) {
        std::fprintf(stderr, "Comparison mismatch in %s\n", name);
        std::abort();
    }
    (void) optimized();
    const double baseline = measure(reference);
    const double fast = measure(optimized);
    std::printf("%s compare: old=%.3f us/call new=%.3f us/call speedup=%.2fx\n",
                name, baseline, fast, baseline / fast);
}

int main() {
    std::u16string japanese(768, u'漢');
    japanese += u"東京の天気";
    testContains("CJK-short", u"今日は東京に行く", u"東京");
    testCompare("CJK-short", u"日本語と東京", u"日本語と東京");
    testContains("CJK-mixed-uppercase", u"今日は東京HELLO", u"hello");
    testContains("CJK-present", japanese, u"東京");
    testContains("CJK-missing", japanese, u"大阪");
    testCompare("CJK-equal", japanese, japanese);
    auto different = japanese; different.back() = u'雨';
    testCompare("CJK-last-diff", japanese, different);
    testContains("Unicode-changing", u"KΟΣ İstanbul東京", u"kος");
    testCompare("Unicode-changing", u"ΟΣΩ", u"οςω");
    return sink == UINT64_MAX ? 1 : 0;
}
