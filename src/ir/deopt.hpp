// Deoptimization checkpoint for an isolated speculative execution segment.
//
// Scratch values are recorded as raw NaN-boxed bits, not doubles: converting
// would destroy string/boolean/NaN payloads. The execution site is explicitly
// identified so a caller cannot accidentally replay already-committed work.
//
// This structure owns copies of the staged slots, but DOES NOT root GC
// pointers on its own. Callers must retain their original live Scratch roots
// while a checkpoint exists. No partial runtime writes are permitted between
// capture and commit; a failing guard discards only speculative slot writes.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace xyo::ir {

enum class ResumePhase : uint8_t {
    ScriptEntry,
    LoopBody,
    LoopSuffix,
};

struct ResumePoint {
    ResumePhase phase = ResumePhase::ScriptEntry;
    uint32_t loopIndex = 0;
    double loopRemaining = 0;
    // Logical Scratch block identity is owned by the caller's compiled
    // script metadata, rather than embedding a raw Block* in the IR.
    uint32_t blockIndex = 0;
};

class DeoptSnapshot {
public:
    bool capture(const uint64_t* slots, size_t count,
                 ResumePoint resume = {}) {
        if ((!slots && count != 0) || count > kMaxSlots) {
            discard();
            return false;
        }
        size_ = count;
        if (count > kInlineSlots) {
            overflow_.assign(slots, slots + count);
        } else {
            overflow_.clear();
            for (size_t i = 0; i < count; ++i) inline_[i] = slots[i];
        }
        resume_ = resume;
        valid_ = true;
        return true;
    }

    bool restore(uint64_t* destination, size_t count,
                 ResumePoint* resume = nullptr) const {
        if (!valid_ || count != size_ ||
            (!destination && count != 0)) return false;
        const uint64_t* values =
            count > kInlineSlots ? overflow_.data() : inline_.data();
        for (size_t i = 0; i < count; ++i)
            destination[i] = values[i];
        if (resume) *resume = resume_;
        return true;
    }

    // Commit is represented by discarding this rollback point only AFTER
    // the caller has checked the WASM status, all result tags, and all
    // captured loop bounds. The caller performs the actual runtime writes.
    void discard() {
        valid_ = false;
        overflow_.clear();
        size_ = 0;
    }

    bool valid() const { return valid_; }
    size_t size() const { return size_; }
    const ResumePoint& resumePoint() const { return resume_; }

    static constexpr size_t kMaxSlots = 4096;

private:
    static constexpr size_t kInlineSlots = 16;
    std::array<uint64_t, kInlineSlots> inline_{};
    std::vector<uint64_t> overflow_;
    size_t size_ = 0;
    ResumePoint resume_{};
    bool valid_ = false;
};

} // namespace xyo::ir
