// IR-001: exact execution-time scalar reporter observations.
// These are SOURCE EVALUATION events, not live LLVM SSA values.
// A removed/optimized-away LLVM temporary can still have an earlier
// rooted Scratch value. Never re-read current Variable::value here.
#pragma once

#include "../core/value.hpp"
#include "gc_value_handles.hpp"
#include "source_reporter_immediate.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace xyo::ir {

struct SourceScalarReadEvent {
    uint64_t reporterPc = 0;
    std::string variableId;
    uint64_t sequence = 0;
    std::optional<uint64_t> resultImmediate;
    RootedValueHandle resultRoot;
    bool rejected = false;
};

struct SourceScalarReadEvents {
    static constexpr size_t kMaxEvents = 4096;
    uint64_t nextSequence = 1;
    bool overflow = false;
    std::vector<SourceScalarReadEvent> pending;
    std::shared_ptr<GcValueHandles> roots;

    bool record(uint64_t reporterPc, std::string variableId, Value result,
                uint64_t observedSequence = 0, Gc& gc = Gc::get()) {
        if (!reporterPc || variableId.empty() || overflow ||
            !nextSequence ||
            nextSequence == std::numeric_limits<uint64_t>::max() ||
            pending.size() >= kMaxEvents ||
            observedSequence == std::numeric_limits<uint64_t>::max()) {
            overflow = true;
            return false;
        }
        SourceScalarReadEvent event;
        event.reporterPc = reporterPc;
        event.variableId = std::move(variableId);
        event.sequence = observedSequence ? observedSequence : nextSequence++;
        if (pointerFreeScratchImmediate(result.bits)) {
            event.resultImmediate = result.bits;
        } else if (result.isString() && !result.isEmptyString()) {
            const uintptr_t ptr =
                reinterpret_cast<uintptr_t>(result.asStrPtr());
            if (ptr < sizeof(GcHeader)) {
                event.rejected = true;
            } else {
                auto* header = reinterpret_cast<GcHeader*>(
                    ptr - sizeof(GcHeader));
                // Exact GC allocation membership BEFORE dereferencing.
                if (!gc.isObject(header) || header->type != GcType::Str) {
                    event.rejected = true;
                } else {
                    if (!roots)
                        roots = std::make_shared<GcValueHandles>(gc);
                    const auto handle = roots->retain(result);
                    if (!handle) event.rejected = true;
                    else event.resultRoot = *handle;
                }
            }
        } else {
            event.rejected = true;
        }
        pending.push_back(std::move(event));
        return !pending.back().rejected;
    }

    void clear() {
        pending.clear();
        roots.reset();
        overflow = false;
    }
};

inline std::optional<Value> resolveScalarReadValue(
        const SourceScalarReadEvent& event,
        const SourceScalarReadEvents& owner) {
    if (event.rejected) return std::nullopt;
    if (event.resultImmediate &&
        pointerFreeScratchImmediate(*event.resultImmediate))
        return Value{*event.resultImmediate};
    if (!event.resultRoot || !owner.roots) return std::nullopt;
    auto rooted = owner.roots->resolve(event.resultRoot);
    if (!rooted || !rooted->isString() ||
        rooted->isEmptyString()) return std::nullopt;
    return *rooted;
}

} // namespace xyo::ir
