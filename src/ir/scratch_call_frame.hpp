// Scheduler-owned procedure continuation stack for shared LLVM/WASM IR.
// Only raw numeric/SSA bits are supported. GC-managed Values need separately
// rooted handles before enabling general string/list argument frames.
#pragma once

#include "execution_abi.hpp"
#include "scratch_transfer.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace xyo::ir {

// Snapshot-local procedure IDs avoid pointers, hash collisions, and
// nondeterministic std::unordered_map iteration order. 0 is the main script.
class ScratchProcedureTable {
public:
    static ScratchProcedureTable capture(const ScratchTransferGraph& graph) {
        ScratchProcedureTable result;
        if (!graph.valid) return result;
        for (const auto& site : graph.sites) {
            if (site.kind == ScratchTransferKind::ProcedureEntry)
                result.sourceIds_.push_back(site.sourceBlockId);
        }
        std::sort(result.sourceIds_.begin(), result.sourceIds_.end());
        result.sourceIds_.erase(
            std::unique(result.sourceIds_.begin(), result.sourceIds_.end()),
            result.sourceIds_.end());
        result.valid_ = true;
        return result;
    }
    bool valid() const { return valid_; }
    std::optional<uint64_t> idFor(const std::string& sourceId) const {
        if (!valid_ || sourceId.empty()) return std::nullopt;
        const auto it = std::lower_bound(
            sourceIds_.begin(), sourceIds_.end(), sourceId);
        if (it == sourceIds_.end() || *it != sourceId)
            return std::nullopt;
        return uint64_t(1 + (it - sourceIds_.begin()));
    }
    const std::string* sourceFor(uint64_t procedureId) const {
        if (!valid_ || procedureId == 0 ||
            procedureId > sourceIds_.size()) return nullptr;
        return &sourceIds_[size_t(procedureId - 1)];
    }
    size_t size() const { return sourceIds_.size(); }

private:
    std::vector<std::string> sourceIds_;
    bool valid_ = false;
};

struct CallFrameLayout {
    uint32_t argumentCapacity = 0;
    uint32_t localCapacity = 0;
    uint32_t maximumDepth = 0;

    bool valid() const {
        return maximumDepth > 0 &&
               maximumDepth <= CallFrameAbi::kMaxDepth &&
               argumentCapacity <= CallFrameAbi::kMaxArguments &&
               localCapacity <= CallFrameAbi::kMaxLocals;
    }
    uint32_t strideWords() const {
        return CallFrameAbi::frameWords(argumentCapacity, localCapacity);
    }
    size_t arenaWords() const {
        return size_t(strideWords()) * maximumDepth;
    }
    bool operator==(const CallFrameLayout&) const = default;
};

struct CallFrameInfo {
    uint64_t procedureId = 0;
    uint64_t returnPc = CallFrameAbi::kNoPc;
    uint64_t resumePc = 0;
    uint32_t arguments = 0;
    uint32_t locals = 0;
};

class ScratchCallStack {
public:
    explicit ScratchCallStack(CallFrameLayout layout)
        : layout_(layout),
          words_(layout.valid() ? layout.arenaWords() : 0, 0) {}

    bool valid() const { return layout_.valid(); }
    bool empty() const { return depth_ == 0; }
    uint32_t depth() const { return depth_; }
    uint64_t revision() const { return revision_; }
    const CallFrameLayout& layout() const { return layout_; }
    // The address is stable throughout this ScratchCallStack lifetime.
    // Before invoking a backend, stage the whole frame arena; NEVER pass
    // live owned frames to speculative code, which may fail or yield.
    const uint64_t* words() const { return words_.data(); }
    size_t arenaWords() const { return words_.size(); }

    bool push(uint64_t procedureId, uint64_t returnPc,
              uint64_t entryPc, std::span<const uint64_t> arguments,
              uint32_t localCount) {
        if (!valid() || depth_ >= layout_.maximumDepth ||
            arguments.size() > layout_.argumentCapacity ||
            localCount > layout_.localCapacity ||
            entryPc == CallFrameAbi::kNoPc ||
            (depth_ == 0 && (returnPc != CallFrameAbi::kNoPc ||
                             procedureId != CallFrameAbi::kRootProcedure)) ||
            (depth_ > 0 && (returnPc == CallFrameAbi::kNoPc ||
                            procedureId == CallFrameAbi::kRootProcedure)))
            return false;

        const size_t base = size_t(depth_) * layout_.strideWords();
        std::fill(words_.begin() + base,
                  words_.begin() + base + layout_.strideWords(), 0);
        words_[base + CallFrameAbi::kProcedureIdWord] = procedureId;
        words_[base + CallFrameAbi::kReturnPcWord] = returnPc;
        words_[base + CallFrameAbi::kResumePcWord] = entryPc;
        words_[base + CallFrameAbi::kArgumentCountWord] = arguments.size();
        words_[base + CallFrameAbi::kLocalCountWord] = localCount;
        std::copy(arguments.begin(), arguments.end(),
                  words_.begin() + base + CallFrameAbi::kHeaderWords);
        ++depth_;
        ++revision_;
        return true;
    }

    std::optional<CallFrameInfo> top() const {
        if (!valid() || !depth_) return std::nullopt;
        const size_t base = size_t(depth_ - 1) * layout_.strideWords();
        if (words_[base + CallFrameAbi::kArgumentCountWord] >
                layout_.argumentCapacity ||
            words_[base + CallFrameAbi::kLocalCountWord] >
                layout_.localCapacity)
            return std::nullopt;
        return CallFrameInfo{
            words_[base + CallFrameAbi::kProcedureIdWord],
            words_[base + CallFrameAbi::kReturnPcWord],
            words_[base + CallFrameAbi::kResumePcWord],
            uint32_t(words_[base + CallFrameAbi::kArgumentCountWord]),
            uint32_t(words_[base + CallFrameAbi::kLocalCountWord]),
        };
    }

    std::optional<uint64_t> argument(uint32_t index) const {
        const auto frame = top();
        if (!frame || index >= frame->arguments) return std::nullopt;
        return words_[frameBase() + CallFrameAbi::argumentWord(index)];
    }
    std::optional<uint64_t> local(uint32_t index) const {
        const auto frame = top();
        if (!frame || index >= frame->locals) return std::nullopt;
        return words_[frameBase() +
                      CallFrameAbi::localWord(layout_.argumentCapacity, index)];
    }
    bool setLocal(uint32_t index, uint64_t rawBits) {
        const auto frame = top();
        if (!frame || index >= frame->locals) return false;
        words_[frameBase() +
               CallFrameAbi::localWord(layout_.argumentCapacity, index)] =
            rawBits;
        ++revision_;
        return true;
    }
    bool setResumePc(uint64_t nextPc) {
        if (!top() || nextPc == CallFrameAbi::kNoPc) return false;
        words_[frameBase() + CallFrameAbi::kResumePcWord] = nextPc;
        ++revision_;
        return true;
    }
    // The parent continuation always comes from the popped frame,
    // never from a newly guessed PC in the parent or in the backend.
    std::optional<uint64_t> pop() {
        const auto frame = top();
        if (!frame) return std::nullopt;
        const size_t base = frameBase();
        const uint64_t returnPc = frame->returnPc;
        std::fill(words_.begin() + base,
                  words_.begin() + base + layout_.strideWords(), 0);
        --depth_;
        ++revision_;
        if (depth_)
            words_[frameBase() + CallFrameAbi::kResumePcWord] = returnPc;
        return returnPc;
    }

    // Check the full image *after* untrusted WASM generated code modifies
    // staged memory, before publishing any frame or scalar variables.
    bool validateImage() const {
        if (!valid() || depth_ > layout_.maximumDepth) return false;
        for (uint32_t index = 0; index < depth_; ++index) {
            const size_t base = size_t(index) * layout_.strideWords();
            const auto proc = words_[base + CallFrameAbi::kProcedureIdWord];
            const auto ret = words_[base + CallFrameAbi::kReturnPcWord];
            const auto pc = words_[base + CallFrameAbi::kResumePcWord];
            if (words_[base + CallFrameAbi::kArgumentCountWord] >
                    layout_.argumentCapacity ||
                words_[base + CallFrameAbi::kLocalCountWord] >
                    layout_.localCapacity ||
                pc == CallFrameAbi::kNoPc ||
                (index == 0 ? (proc != CallFrameAbi::kRootProcedure ||
                               ret != CallFrameAbi::kNoPc)
                            : (proc == CallFrameAbi::kRootProcedure ||
                               ret == CallFrameAbi::kNoPc)))
                return false;
        }
        return true;
    }

    // Allows a compiled runner to address any staged word with the same
    // constants on native LLVM and WebAssembly.
    std::optional<uint32_t> wordOffset(uint32_t frame,
                                       uint32_t word) const {
        if (!valid() || frame >= depth_ ||
            word >= layout_.strideWords()) return std::nullopt;
        return frame * layout_.strideWords() + word;
    }

private:
    friend class ScratchCallTransaction;
    size_t frameBase() const {
        return size_t(depth_ - 1) * layout_.strideWords();
    }
    CallFrameLayout layout_;
    std::vector<uint64_t> words_;
    uint32_t depth_ = 0;
    uint64_t revision_ = 0;
};

// A speculative JIT slice mutates its own call stack. On validation error,
// yield failure, deopt or WASM trap, the original stack is untouched.
// The runtime must still validate staged Scratch variable/GC state before
// publishing a successful slice; call-stack commit alone is not enough.
class ScratchCallTransaction {
public:
    explicit ScratchCallTransaction(ScratchCallStack& owner)
        : owner_(&owner), staged_(owner), baseRevision_(owner.revision()) {}
    ScratchCallStack& staged() { return staged_; }
    bool commit() {
        if (!owner_ || owner_->revision() != baseRevision_ ||
            owner_->layout() != staged_.layout() ||
            !staged_.validateImage())
            return false;
        owner_->words_.swap(staged_.words_);
        owner_->depth_ = staged_.depth_;
        ++owner_->revision_;
        owner_ = nullptr; // a transaction is single-use
        return true;
    }
    void discard() { owner_ = nullptr; }

private:
    ScratchCallStack* owner_;
    ScratchCallStack staged_;
    uint64_t baseRevision_;
};

} // namespace xyo::ir
