// Resumable interpreter/WASM handoff at existing Scratch StackFrame boundaries.
// A statement range is atomic; the interpreter still owns non-warp repeat,
// procedure returns, warp deadlines, and every yield checkpoint.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace xyo {
class Thread;
struct Block;
class Blocks;

namespace wasm {

struct HotCode;

class NestedJitCache {
public:
    NestedJitCache() = default;
    ~NestedJitCache();

    NestedJitCache(const NestedJitCache&) = delete;
    NestedJitCache& operator=(const NestedJitCache&) = delete;

    // Commit a bounded consecutive range at the *current* interpreter
    // statement, without modifying Thread.stack/StackFrame. On success the
    // caller replaces only the current block ID with pastEndId, and lets the
    // ordinary interpreter handle loop/procedure return and yielding.
    bool execute(Thread& thread, const Block* first, std::string& pastEndId,
                 bool* permanentlyUnsupported = nullptr);

private:
    struct Entry {
        std::shared_ptr<HotCode> code;
        std::string pastEndId;
        uint32_t visits = 0;
        uint8_t failures = 0;
        bool attempted = false;
        const Blocks* owner = nullptr;
        uint64_t sourceRevision = 0;
    };
    std::unordered_map<const Block*, Entry> entries_;
    uint64_t attempted_ = 0;
    uint64_t compiled_ = 0;
    uint64_t executed_ = 0;
    uint64_t fastExecuted_ = 0;
    uint64_t bailed_ = 0;
    uint64_t warpCompiled_ = 0;
    uint64_t warpExecuted_ = 0;
    uint64_t warpBailed_ = 0;
    uint64_t treeCompiled_ = 0;
    uint64_t treeExecuted_ = 0;
    uint64_t treeBailed_ = 0;
    uint64_t tailCompiled_ = 0;
    uint64_t tailExecuted_ = 0;
    uint64_t tailBailed_ = 0;
    uint64_t returnCompiled_ = 0;
    uint64_t returnExecuted_ = 0;
    uint64_t returnBailed_ = 0;
    uint64_t pcCompiled_ = 0;
    uint64_t pcExecuted_ = 0;
    uint64_t pcBailed_ = 0;
};

} // namespace wasm
} // namespace xyo
