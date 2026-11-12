// IR-001: validate RT-owned detached diagnostic words against an
// independent compiler-owned typed manifest. No scheduler writes.
#pragma once

#include "source_typed_ssa.hpp"
#include <cstdint>
#include <optional>

namespace xyo::ir {

// Works with NativeRtSsaCapture without importing Thread or its GC/runtime
// headers into the common IR layer. Both the source identity and site ID
// must match a separately supplied compiler manifest.
// FNV fingerprint detects accidental corruption, NOT malicious forgery.
template<class DetachedCapture>
std::optional<SourceTypedSsaSnapshot> verifyDetachedTypedSsa(
    const DetachedCapture& state,
    const SourceTypedSsaManifest& expected) {
    if (!expected.valid() ||
        state.sourceRevision != expected.revision ||
        state.digestLo != expected.digestLo ||
        state.digestHi != expected.digestHi ||
        state.procedureId != expected.procedureId ||
        state.sourcePc != expected.resumePc ||
        state.siteId != expected.siteId ||
        state.fingerprint != state.computeFingerprint() ||
        state.descriptors != expected.descriptors ||
        uint64_t(state.unresolvedPointers) +
            uint64_t(state.unresolvedOther) != state.unsupported ||
        state.distinctStackAllocas > state.stackDerivedPointers ||
        state.stackDerivedPointers > state.unresolvedPointers ||
        (state.numericComplete && state.unsupported != 0))
        return std::nullopt;

    uint64_t pointers = 0;
    for (uint32_t count : state.pointerOrigins) pointers += count;
    if (pointers != state.unresolvedPointers)
        return std::nullopt;
    return SourceTypedSsaSnapshot::capture(expected, state.words);
}

} // namespace xyo::ir
