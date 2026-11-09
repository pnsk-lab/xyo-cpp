// Measure actual GC payload bytes and construction/view cost, not just layout.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "core/gc.hpp"
#include "core/sstr.hpp"

int main(int argc, char** argv) {
    using namespace xyo;
    if (argc != 2) return 2;
    if (!setStringLeafMode(argv[1])) return 2;
    constexpr int kCount = 100000;
    const std::u16string text(48, u'a');
    std::vector<SStr*> roots;
    roots.reserve(kCount);
    Gc::DeferCollect defer;
    auto& gc = Gc::get();
    const size_t before = gc.bytesAllocated();
    const auto t0 = std::chrono::steady_clock::now();
    size_t payloadBytes = 0;
    for (int i = 0; i < kCount; ++i) {
        auto* str = SStr::create(text);
        roots.push_back(str);
        payloadBytes += (reinterpret_cast<GcHeader*>(str) - 1)->size;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const size_t allocated = gc.bytesAllocated() - before;
    volatile uint64_t checksum = 0;
    const auto t2 = std::chrono::steady_clock::now();
    for (auto* str : roots) {
        checksum = checksum + str->unitAt(17);
    }
    const auto t3 = std::chrono::steady_clock::now();
    for (auto* str : roots) {
        checksum = checksum + str->view()[17];
    }
    const auto t4 = std::chrono::steady_clock::now();
    using Ms = std::chrono::duration<double, std::milli>;
    const double createMs = Ms(t1 - t0).count();
    const double unitMs = Ms(t3 - t2).count();
    const double viewMs = Ms(t4 - t3).count();
    std::printf("{\"leaf\":\"%s\",\"count\":%d,\"code_units\":%zu,"
                "\"gc_payload_bytes\":%zu,\"gc_allocated_bytes\":%zu,"
                "\"create_ms\":%.3f,\"unit_at_ms\":%.3f,"
                "\"first_view_ms\":%.3f,\"checksum\":%llu}\n",
                argv[1], kCount, text.size(), payloadBytes, allocated,
                createMs, unitMs, viewMs,
                static_cast<unsigned long long>(checksum));
    return 0;
}
