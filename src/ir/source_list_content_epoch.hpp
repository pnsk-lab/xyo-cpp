// IR-001: process-wide CONSERVATIVE Scratch list CONTENT mutation epoch.
// This is not SList::storageEpoch, which only tracks list layout changes.
// Any list mutation invalidates all previously observed list reads.
// It does not establish that every external/plugin/GPU/AOT writer participates.
// No future continuation may be enabled merely because the epoch matches.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace xyo::ir {

inline bool listContentEpochEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("XYO_IR001_RUNTIME_REPORTER_AUDIT");
        const char* strict = std::getenv("XYO_RT_REQUIRE_REPORTER_SSA_AUDIT");
        const char* capture = std::getenv("XYO_RT_CAPTURE_LLVM_SSA");
        return capture && capture[0] == '1' &&
            ((value && value[0] == '1') ||
             (strict && strict[0] == '1'));
    }();
    return enabled;
}

inline std::atomic<uint64_t>& listContentMutationCounter() {
    static std::atomic<uint64_t> epoch{1};
    return epoch;
}

// Snapshot is valid only with no in-flight instrumented list writer.
// This is conservative diagnostic evidence, NOT thread-safe SList I/O.
inline std::atomic<uint32_t>& listContentActiveWriters() {
    static std::atomic<uint32_t> active{0};
    return active;
}
inline void bumpSourceListEpoch() {
    auto& counter = listContentMutationCounter();
    uint64_t epoch = counter.load(std::memory_order_relaxed);
    while (epoch != std::numeric_limits<uint64_t>::max() &&
           !counter.compare_exchange_weak(epoch, epoch + 1,
               std::memory_order_seq_cst, std::memory_order_relaxed)) {}
}
inline uint64_t sourceListContentEpoch() {
    if (!listContentEpochEnabled()) return 0;
    const uint64_t before = listContentMutationCounter().load(
        std::memory_order_seq_cst);
    if (listContentActiveWriters().load(std::memory_order_seq_cst))
        return 0;
    const uint64_t after = listContentMutationCounter().load(
        std::memory_order_seq_cst);
    return before == after &&
           before != std::numeric_limits<uint64_t>::max() ? before : 0;
}
inline void beginSourceListContentWrite() {
    if (!listContentEpochEnabled()) return;
    listContentActiveWriters().fetch_add(1, std::memory_order_seq_cst);
    bumpSourceListEpoch();
}
inline void endSourceListContentWrite() {
    if (!listContentEpochEnabled()) return;
    bumpSourceListEpoch();
    listContentActiveWriters().fetch_sub(1, std::memory_order_seq_cst);
}
class ScopedSourceListContentWrite {
    bool enabled_;
public:
    ScopedSourceListContentWrite()
        : enabled_(listContentEpochEnabled()) {
        if (enabled_) beginSourceListContentWrite();
    }
    ~ScopedSourceListContentWrite() {
        if (enabled_) endSourceListContentWrite();
    }
    ScopedSourceListContentWrite(
        const ScopedSourceListContentWrite&) = delete;
    ScopedSourceListContentWrite& operator=(
        const ScopedSourceListContentWrite&) = delete;
};

inline bool sourceListContentEpochValid(uint64_t readEpoch,
                                        uint64_t observedEpoch) {
    return listContentEpochEnabled() &&
        readEpoch != 0 && observedEpoch != 0 &&
        readEpoch != std::numeric_limits<uint64_t>::max() &&
        readEpoch == observedEpoch;
}

} // namespace xyo::ir
