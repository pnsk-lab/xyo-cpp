// RT-001: opt-in GC protection for compiler-proven JIT-generated
// boxed Scratch strings at an actual source-certified LLVM checkpoint.
// A trusted producer classification alone is NEVER a GC pointer:
/// its live word must also name an exactly allocated GcType::Str object.
// The detached Thread snapshot owns exact GC handles until handoff completes.
#pragma once

#include "../engine/thread.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace xyo::rt {

struct GeneratedSsaRootAudit {
    uint32_t provenStringSlots = 0;
    uint32_t rootedHeapStrings = 0;
    uint32_t immediateStrings = 0;
};

// Capture runs synchronously on the native Fiber, AFTER the lexical source
// checkpoint and BEFORE the scheduler may retire that Fiber. The caller
// already validated the compiler's readonly descriptors and source marker.
// There must be no concurrent GC or worker allocation during this phase.
inline bool retainGeneratedSsaRoots(NativeRtSsaCapture& saved,
                                    Gc& gc = Gc::get()) {
    const size_t count = saved.words.size();
    if (!count || saved.descriptors.size() != count ||
        saved.generatedStrings.size() != count ||
        saved.compilerGeneratedStrings != saved.generatedStrings ||
        saved.aliasArguments.size() != count ||
        saved.generatedStringRoots || !saved.generatedStringHandles.empty())
        return false;

    std::unique_ptr<ir::GcValueHandles> roots;
    std::vector<ir::RootedValueHandle> handles(count);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t proof = saved.generatedStrings[i];
        if (proof > 1) return false;
        if (!proof) continue;

        const uint32_t kind = uint32_t(saved.descriptors[i] >> 32);
        const uint32_t id = uint32_t(saved.descriptors[i]);
        // Proof=1 is ONLY emitted for native i64 Call/PHI/SELECT
        // instruction SSA. Never treat numeric arguments or floating
        // words as generated Scratch heap values.
        if (kind != 64 || (id & 0x80000000u) ||
            saved.aliasArguments[i] != 0)
            return false;

        const Value value{saved.words[i]};
        if (!value.isString()) return false; // Includes undefined.
        if (value.isEmptyString()) continue;

        const auto address =
            reinterpret_cast<uintptr_t>(value.asStrPtr());
        if (address < sizeof(GcHeader)) return false;
        auto* header = reinterpret_cast<GcHeader*>(
            address - sizeof(GcHeader));
        // Exact membership check BEFORE dereferencing the header. A
        // NaN-box tag by itself is NEVER a valid GC root certificate.
        if (!gc.isObject(header) || header->type != GcType::Str)
            return false;

        if (!roots) roots = std::make_unique<ir::GcValueHandles>(gc);
        const auto handle = roots->retain(value);
        if (!handle) return false;
        handles[i] = *handle;
    }
    // Register new handles atomically: on any failed slot, the local
    // provider unregisters and the destination snapshot stays empty.
    saved.generatedStringHandles = std::move(handles);
    saved.generatedStringRoots = std::move(roots);
    return true;
}

// The verifier NEVER roots a new arbitrary word. It only compares a
// saved raw i64 SSA string value against the independently retained
// GC handle produced from a known compiler-certified string producer.
// The full compiler mask cannot be replaced with a rehashed runtime copy.
inline std::optional<GeneratedSsaRootAudit> verifyGeneratedSsaRoots(
    const NativeRtSsaCapture& saved) {
    const size_t count = saved.words.size();
    if (!count || saved.descriptors.size() != count ||
        saved.generatedStrings.size() != count ||
        saved.compilerGeneratedStrings.size() != count ||
        saved.generatedStringHandles.size() != count ||
        saved.compilerGeneratedStrings != saved.generatedStrings ||
        saved.aliasArguments.size() != count ||
        saved.fingerprint != saved.computeFingerprint())
        return std::nullopt;

    GeneratedSsaRootAudit result;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t proof = saved.generatedStrings[i];
        if (proof > 1) return std::nullopt;
        const auto handle = saved.generatedStringHandles[i];
        if (!proof) {
            if (handle) return std::nullopt;
            continue;
        }
        const uint32_t kind = uint32_t(saved.descriptors[i] >> 32);
        const uint32_t id = uint32_t(saved.descriptors[i]);
        if (kind != 64 || (id & 0x80000000u) ||
            saved.aliasArguments[i]) return std::nullopt;
        ++result.provenStringSlots;
        const Value value{saved.words[i]};
        if (!value.isString()) return std::nullopt;
        if (value.isEmptyString()) {
            if (handle) return std::nullopt;
            ++result.immediateStrings;
            continue;
        }
        if (!handle || !saved.generatedStringRoots)
            return std::nullopt;
        const auto rooted = saved.generatedStringRoots->resolve(handle);
        if (!rooted || rooted->bits != value.bits)
            return std::nullopt;
        ++result.rootedHeapStrings;
    }
    if (saved.generatedStringRoots &&
        saved.generatedStringRoots->size() != result.rootedHeapStrings)
        return std::nullopt;
    return result;
}

} // namespace xyo::rt
