// GC-rooted, backend-neutral argument sidecar for resumable Scratch frames.
//
// The portable CallFrameAbi numeric arena does not itself scan pointers for
// GC. Instead, a scheduler may keep one RootedCallFrameValues instance next
// to its ScratchCallStack and use stable handles for heap-string arguments.
// Values remain rooted through yield and release in strict frame-pop order.
//
// This sidecar intentionally does NOT take over the scheduler's resume PC,
// frame generation, or transactional publication rules.
#pragma once

#include "execution_abi.hpp"
#include "gc_value_handles.hpp"
#include "scratch_call_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <algorithm>
#include <span>
#include <utility>
#include <vector>

namespace xyo::ir {

class RootedCallFrameValues final {
public:
    explicit RootedCallFrameValues(Gc& gc = Gc::get()) : roots_(gc) {}
    RootedCallFrameValues(const RootedCallFrameValues&) = delete;
    RootedCallFrameValues& operator=(const RootedCallFrameValues&) = delete;

    ~RootedCallFrameValues() { clear(); }

    // Call only while the interpreter/LLVM source arguments are still
    // rooted. No raw SStr pointer is copied into the portable frame sidecar.
    // Failed push is all-or-nothing, including GC root registration.
    bool push(std::span<const uint64_t> args) {
        if (frames_.size() >= CallFrameAbi::kMaxDepth ||
            args.size() > CallFrameAbi::kMaxArguments)
            return false;
        Frame frame;
        frame.reserve(args.size());
        for (uint64_t bits : args) {
            const Value value{bits};
            if (value.isString() && !value.isEmptyString()) {
                const auto handle = roots_.retain(value);
                if (!handle) {
                    releaseFrame(frame);
                    return false;
                }
                frame.push_back({0, *handle});
            } else {
                frame.push_back({bits, {}});
            }
        }
        frames_.push_back(std::move(frame));
        return true;
    }

    bool pop() {
        if (frames_.empty()) return false;
        releaseFrame(frames_.back());
        frames_.pop_back();
        return true;
    }

    void clear() {
        while (pop()) {}
    }

    // The returned Value is borrowed: do not retain its raw bits in an
    // untraced continuation or use it after a subsequent scheduler yield.
    std::optional<Value> argument(std::size_t frameIndex,
                                  std::size_t argumentIndex) const {
        if (frameIndex >= frames_.size() ||
            argumentIndex >= frames_[frameIndex].size())
            return std::nullopt;
        const auto& entry = frames_[frameIndex][argumentIndex];
        return entry.heap ? roots_.resolve(entry.heap) :
                            std::optional<Value>{Value{entry.immediateBits}};
    }

    // Replace a frame argument without ever leaving its string value
    // unrooted. Frame updates are only permitted at scheduler-safe points.
    bool update(std::size_t frameIndex, std::size_t argumentIndex,
                uint64_t bits) {
        if (frameIndex >= frames_.size() ||
            argumentIndex >= frames_[frameIndex].size())
            return false;
        Slot next{bits, {}};
        const Value value{bits};
        if (value.isString() && !value.isEmptyString()) {
            const auto handle = roots_.retain(value);
            if (!handle) return false;
            next = {0, *handle};
        }
        Slot& prior = frames_[frameIndex][argumentIndex];
        if (prior.heap) roots_.release(prior.heap);
        prior = next;
        return true;
    }

    std::size_t depth() const { return frames_.size(); }
    std::size_t liveHeapRoots() const { return roots_.size(); }

private:
    struct Slot {
        uint64_t immediateBits;
        RootedValueHandle heap;
    };
    using Frame = std::vector<Slot>;

    void releaseFrame(const Frame& frame) {
        for (const auto& slot : frame)
            if (slot.heap) roots_.release(slot.heap);
    }

    GcValueHandles roots_;
    std::vector<Frame> frames_;
};


// Scheduler-facing composite of the existing portable ScratchCallStack and
// the rooted argument sidecar. This eliminates the mismatch window between
// two independently managed stacks and prevents raw SStr pointers from ever
// entering the executable's numeric/SSA frame arena.
//
// RootedScratchCallStack is an opt-in ABI adapter. It does NOT mean general
// heap-value SSA/local slots, stack snapshot/transaction or deopt recovery
// can be used with arbitrary effects. Only arguments are rooted here.
class RootedScratchCallStack final {
public:
    explicit RootedScratchCallStack(CallFrameLayout layout,
                                    Gc& gc = Gc::get())
        : frames_(layout), values_(gc) {}

    RootedScratchCallStack(const RootedScratchCallStack&) = delete;
    RootedScratchCallStack& operator=(const RootedScratchCallStack&) = delete;

    bool valid() const {
        return frames_.valid() && frames_.depth() == values_.depth();
    }
    uint32_t depth() const { return frames_.depth(); }
    uint64_t revision() const { return frames_.revision(); }
    std::size_t rootedArguments() const { return values_.liveHeapRoots(); }
    const ScratchCallStack& frames() const { return frames_; }

    // All validation occurs before root acquisition. A failed frame push
    // releases the newly acquired roots and leaves the original frame
    // state untouched. A GC may collect between successive scheduler
    // operations but cannot collect the just-pushed heap strings.
    bool push(uint64_t procedureId, uint64_t returnPc, uint64_t entryPc,
              std::span<const uint64_t> arguments, uint32_t locals) {
        const auto& layout = frames_.layout();
        if (!valid() || frames_.depth() >= layout.maximumDepth ||
            arguments.size() > layout.argumentCapacity ||
            locals > layout.localCapacity ||
            entryPc == CallFrameAbi::kNoPc ||
            (frames_.depth() == 0 &&
             (procedureId != CallFrameAbi::kRootProcedure ||
              returnPc != CallFrameAbi::kNoPc)) ||
            (frames_.depth() != 0 &&
             (procedureId == CallFrameAbi::kRootProcedure ||
              returnPc == CallFrameAbi::kNoPc)))
            return false;

        if (!values_.push(arguments)) return false;

        // Any nonnumeric tagged Value is represented only by the rooted
        // sidecar. Its numeric SSA word is intentionally unusable by the
        // compiler until type-aware code explicitly resolves the sidecar.
        std::vector<uint64_t> numeric(arguments.begin(), arguments.end());
        for (uint64_t& bits : numeric)
            if (!Value{bits}.isNumber()) bits = val::kUndefined;

        if (!frames_.push(procedureId, returnPc, entryPc, numeric, locals)) {
            values_.pop();
            return false;
        }
        return valid();
    }

    // Resolved arguments are borrowed values. A compiled backend must not
    // cache their raw pointers across a scheduler yield or GC boundary.
    std::optional<Value> argument(uint32_t frameIndex,
                                  uint32_t argumentIndex) const {
        if (!valid() || frameIndex >= frames_.depth()) return std::nullopt;
        // RootedCallFrameValues checks the argument arity itself.
        return values_.argument(frameIndex, argumentIndex);
    }

    // The return PC and logical stack frame are committed in lockstep with
    // release of their roots. The call owner must prevent arbitrary
    // mutation of the frame stack through the const-only frames() view.
    std::optional<uint64_t> pop() {
        if (!valid() || frames_.empty()) return std::nullopt;
        const auto returnPc = frames_.pop();
        if (!returnPc || !values_.pop()) return std::nullopt;
        return returnPc;
    }

    bool setResumePc(uint64_t pc) {
        return valid() && frames_.setResumePc(pc);
    }

    bool setNumericLocal(uint32_t index, uint64_t bits) {
        return valid() && frames_.setLocal(index, bits);
    }

    // Execute on scheduler deopt/disposal, never as a resumable checkpoint.
    void clear() {
        while (pop()) {}
    }

private:
    ScratchCallStack frames_;
    RootedCallFrameValues values_;
};

} // namespace xyo::ir
