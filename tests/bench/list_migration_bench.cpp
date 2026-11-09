// Isolate bulk representation transitions from the Scratch scheduling/JIT.
// Only xyo's public SList APIs are used.
#include "core/slist.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace xyo;
using Clock = std::chrono::steady_clock;

static double millis(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

int main(int argc, char** argv) {
    const uint32_t size = argc > 1 ? std::uint32_t(std::strtoul(argv[1], nullptr, 10)) : 100000;
    const int cycles = argc > 2 ? std::atoi(argv[2]) : 40;
    if (size < 1000 || cycles < 1) return 1;
    xyo_list_storage_mode = 7;
    SList* list = SList::create();
    for (uint32_t i = 0; i < size; ++i)
        list->push(Value::numCanonical(double(i)));
    // Keep the list and values rooted while measuring repeated transitions.
    double gapPromote = 0, gapDemote = 0, chunkPromote = 0, chunkDemote = 0;
    for (int k = 0; k < cycles + 3; ++k) {
        const bool gap = (k & 1) == 0;
        const auto before = Clock::now();
        for (uint32_t edit = 0; edit < 32; ++edit) {
            const uint32_t at = gap ? size / 2 :
                64 + ((edit * 7919u) % (size - 128));
            list->insert0(at, Value::numCanonical(double(edit)));
            list->erase0(at + 1);
        }
        const auto promoted = Clock::now();
        if (!list->extra || list->adaptiveBackend != (gap ? 5u : 6u))
            return std::fprintf(stderr, "wrong backend selected: %u\n", list->adaptiveBackend), 2;
        // Read 64 values to trigger the regular read-heavy demotion.
        for (unsigned i = 0; i < 64; ++i)
            list->noteAdaptiveRead();
        const auto demoted = Clock::now();
        if (!list->isContiguous() || list->len != size)
            return std::fprintf(stderr, "bad demotion\n"), 3;
        if (k < 3) continue;
        if (gap) {
            gapPromote += millis(before, promoted);
            gapDemote += millis(promoted, demoted);
        } else {
            chunkPromote += millis(before, promoted);
            chunkDemote += millis(promoted, demoted);
        }
    }
    const double perMode = double(cycles) / 2;
    std::printf("size=%u cycles=%d gap_promote_ms=%.6f gap_demote_ms=%.6f "
                "chunk_promote_ms=%.6f chunk_demote_ms=%.6f\n",
                size, cycles, gapPromote / perMode, gapDemote / perMode,
                chunkPromote / perMode, chunkDemote / perMode);
    return 0;
}
