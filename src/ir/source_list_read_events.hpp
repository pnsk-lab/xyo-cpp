// IR-001: runtime Scratch list-read events with GC-rooted observations.
// Kept free of LLVM/compiler-only headers for no-JIT binaries.
#pragma once
#include "../core/value.hpp"
#include "../core/sstr.hpp"
#include "gc_value_handles.hpp"
#include "source_reporter_immediate.hpp"
#include "source_list_content_epoch.hpp"
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace xyo::ir {

struct SourceListReadEvent {
    uint64_t reporterPc = 0;
    std::string listId;
    uint64_t sequence = 0;
    // A boxed immediate numeric INDEX, recorded without re-evaluation.
    std::optional<uint64_t> indexBits;
    // Exact 0-based numeric position for simple, finite ordinary index.
    // Never guess a randomized/special string index.
    std::optional<uint32_t> numericPosition;
    uint32_t lengthAfterRead = 0;
    uint64_t storageEpochAfterRead = 0; // NOT a content mutation proof
    // The process-wide list write epoch sampled AFTER read observation.
    // It cannot retroactively make the original read atomic.
    uint64_t contentEpochAtReadStart = 0;
    uint64_t contentEpochAfterObserver = 0;
    std::optional<uint64_t> resultImmediate;
    RootedValueHandle resultRoot;
    bool rejected = false;
};

struct SourceListReadEvents {
    static constexpr size_t kMaxEvents = 4096;
    uint64_t nextSequence = 1;
    bool overflow = false;
    std::vector<SourceListReadEvent> pending;
    std::shared_ptr<GcValueHandles> roots;

    bool record(uint64_t reporterPc, std::string listId,
                Value index, Value result,
                uint32_t length, uint64_t storageEpoch,
                std::optional<uint64_t> beforeReadEpoch = std::nullopt,
                uint64_t observedSequence = 0,
                Gc& gc = Gc::get()) {
        if (!reporterPc || listId.empty() || overflow || !nextSequence ||
            nextSequence == std::numeric_limits<uint64_t>::max() ||
            pending.size() >= kMaxEvents ||
            observedSequence == std::numeric_limits<uint64_t>::max()) {
            overflow = true;
            return false;
        }
        SourceListReadEvent e;
        e.reporterPc = reporterPc;
        e.listId = std::move(listId);
        e.sequence = observedSequence ? observedSequence : nextSequence++;
        e.lengthAfterRead = length;
        e.storageEpochAfterRead = storageEpoch;
        // Native generated code samples before the ACTUAL list read.
        // Synthetic unit records may omit the token and use record entry.
        e.contentEpochAtReadStart = beforeReadEpoch.value_or(
            sourceListContentEpoch());
        if (index.isNumber() &&
            pointerFreeScratchImmediate(index.bits)) {
            e.indexBits = index.bits;
            const double v = index.asNumberUnchecked();
            if (std::isfinite(v) && v >= 1 &&
                v < 2147483648.0) {
                const uint32_t oneBased = uint32_t(v);
                if (oneBased >= 1 && oneBased <= length)
                    e.numericPosition = oneBased - 1;
            }
        }
        // Scratch numeric shadow INDEX may evaluate to a boxed string
        // ("1", "12", etc.). The *original* listGet already applied the
        // conversion, possibly consuming RNG for "random"/"any".
        // Only read pure decimal digits; never call listIndex twice.
        if (!e.numericPosition && index.isString() &&
            !index.isEmptyString()) {
            const uintptr_t ptr =
                reinterpret_cast<uintptr_t>(index.asStrPtr());
            if (ptr >= sizeof(GcHeader)) {
                auto* h = reinterpret_cast<GcHeader*>(
                    ptr - sizeof(GcHeader));
                if (gc.isObject(h) && h->type == GcType::Str) {
                    SStr* str = index.asStrPtr();
                    if (str->len >= 1 && str->len <= 10) {
                        uint64_t ordinal = 0;
                        bool decimal = true;
                        for (uint32_t k = 0; k < str->len; ++k) {
                            const char16_t c = str->data()[k];
                            if (c < u'0' || c > u'9') {
                                decimal = false;
                                break;
                            }
                            ordinal = ordinal * 10 + uint64_t(c - u'0');
                        }
                        if (decimal && ordinal >= 1 &&
                            ordinal <= length)
                            e.numericPosition = uint32_t(ordinal - 1);
                    }
                }
            }
        }
        if (pointerFreeScratchImmediate(result.bits)) {
            e.resultImmediate = result.bits;
        } else if (result.isString() && !result.isEmptyString()) {
            const uintptr_t ptr =
                reinterpret_cast<uintptr_t>(result.asStrPtr());
            if (ptr < sizeof(GcHeader)) {
                e.rejected = true;
            } else {
                auto* h = reinterpret_cast<GcHeader*>(
                    ptr - sizeof(GcHeader));
                // Exact membership BEFORE dereference.
                if (!gc.isObject(h) || h->type != GcType::Str) {
                    e.rejected = true;
                } else {
                    if (!roots)
                        roots = std::make_shared<GcValueHandles>(gc);
                    const auto handle = roots->retain(result);
                    if (!handle) e.rejected = true;
                    else e.resultRoot = *handle;
                }
            }
        } else {
            e.rejected = true;
        }
        e.contentEpochAfterObserver = sourceListContentEpoch();
        pending.push_back(std::move(e));
        return !pending.back().rejected;
    }

    // Each native checkpoint owns a bounded observation window.
    // Clearing never leaves GC pointers behind; root handles are owned
    // by the independent snapshot copy if one was published.
    void clear() {
        pending.clear();
        roots.reset();
        overflow = false;
    }
};

} // namespace xyo::ir
