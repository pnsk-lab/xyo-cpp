// Allocation-focused A/B: each dynamic two-character result comes from
// scratch-like joins, with a bounded vocabulary. Not a generic string bench.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "core/gc.hpp"
#include "core/sstr.hpp"

int main(int argc, char** argv) {
    using namespace xyo;
    if (argc != 3 && argc != 4) return 2;
    const bool unique = argc == 4 && std::strcmp(argv[3], "unique") == 0;
    const bool enabled = std::strcmp(argv[1], "on") == 0;
    if (!setStringLeafMode(argv[2])) return 2;
    setStringShortInternEnabled(enabled);
    Gc::DeferCollect defer;
    auto& gc = Gc::get();
    constexpr int n = 250000;
    char16_t lhs[] = u"0";
    SStr* rhs = SStr::create(u":");
    // Warm the table so admission cost is not part of the steady-state
    // measurement (and don't charge the cached roots to the loop).
    for (int digit = 0; digit < 10; ++digit) {
        lhs[0] = char16_t(u'0' + digit);
        (void)sstrConcat(std::u16string_view(lhs, 1), u":");
    }
    const size_t startBytes = gc.bytesAllocated();
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t checksum = 0;
    for (int i = 0; i < n; ++i) {
        SStr* str = nullptr;
        if (unique) {
            // 64^3 distinct keys, up to 4 ASCII code units, with no cache hits.
            char16_t text[4] = {
                char16_t(33 + (i & 63)),
                char16_t(33 + ((i >> 6) & 63)),
                char16_t(33 + ((i >> 12) & 63)),
                u'!'
            };
            str = sstrConcat({text, 2}, {text + 2, 2});
        } else {
            lhs[0] = char16_t(u'0' + i % 10);
            str = sstrConcatStrings(SStr::create(lhs, 1), rhs);
        }
        checksum += str->unitAt(0) + str->unitAt(1);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const size_t bytes = gc.bytesAllocated() - startBytes;
    const double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf(
        "{\"intern\":\"%s\",\"leaf\":\"%s\",\"workload\":\"%s\",\"iterations\":%d,"
        "\"gc_allocated_bytes\":%zu,\"elapsed_ms\":%.3f,"
        "\"checksum\":%llu}\n",
        argv[1], argv[2], unique ? "unique" : "repeated", n, bytes, ms,
        static_cast<unsigned long long>(checksum));
    return 0;
}
