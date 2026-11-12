// Experimental AOT v3: resolve BlockCached pointers only when executed.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace xyo {
struct Blocks;
struct BlockCached;

struct AotLazyCachedSlot {
    Blocks* blocks = nullptr;
    std::string blockId;
    // Must be the source Blocks revision at native AOT load. Unstamped
    // descriptors are forbidden: a missing certificate is never trusted.
    uint64_t sourceRevision = 0;
    std::mutex mutex;
    // Hot path requires no lock after the initial successful resolution.
    std::atomic<BlockCached*> resolved{nullptr};
    AotLazyCachedSlot(Blocks* b, std::string id, uint64_t revision)
        : blocks(b), blockId(std::move(id)), sourceRevision(revision) {}
};
} // namespace xyo

extern "C" const xyo::BlockCached* xyo_rt_aot_cached(void* descriptor);
