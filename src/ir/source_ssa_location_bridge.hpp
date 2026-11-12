// IR-001: read-only SSA -> source procedure-argument provenance bridge.
// Compiler-generated LLVM metadata is compared with the actual GC-rooted
// Scratch CallFrame. This is NOT a source-variable/local recovery plan:
// arbitrary LLVM temporaries remain unmapped, and no deopt is enabled.
#pragma once

#include "source_ssa_gc_audit.hpp"
#include "../jit/rt_rooted_ssa_arguments.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace xyo::ir {

enum class SourceSsaLocationKind : uint8_t {
    Unmapped = 0,
    ProcedureArgument = 1, // one-based original Scratch procedure arg
    NativeGeneratedString = 2, // exact RT-owned GC root, NO source local
};

struct SourceSsaLocation {
    uint64_t descriptor = 0;
    SourceSsaLocationKind kind = SourceSsaLocationKind::Unmapped;
    uint32_t argument = 0;
};

struct SourceSsaLocationAudit {
    std::vector<SourceSsaLocation> locations;
    uint32_t mappedArguments = 0;
    uint32_t rootedArgumentStrings = 0;
    uint32_t nativeGeneratedStrings = 0;
    uint32_t unmappedWords = 0;
    uint32_t unrootedWords = 0;
    uint32_t unresolvedNativePointers = 0;
    uint32_t unresolvedOther = 0;
    uint64_t rootedFrameRevision = 0;
    // A live Scratch variable/local is not identified by any of the
    // current compiler SSA alias or generated-string masks.
    bool sourceLocalsComplete = false;
    bool executable() const { return false; }
};

// The independently rooted ScratchCallStack must originate from the
// matching source image and still be alive at this safe point.
// Does not borrow or export raw object pointers, modify GC root tables,
// or write to the original interpreter/LLVM state.
//
// This refuses an unsupported GC state of a KNOWN compiler-produced
// string, but permits other unmapped SSA only as diagnostic gaps.
template<class Detached, class RootedFrames>
std::optional<SourceSsaLocationAudit> auditSourceSsaLocations(
    const Detached& saved,
    const SourceTypedSsaManifest& compilerManifest,
    const RootedFrames& rootedFrames) {
    const auto typed = verifyDetachedTypedSsa(saved, compilerManifest);
    if (!typed || !rootedFrames.valid() || !rootedFrames.depth())
        return std::nullopt;
    const auto sourceFrame = rootedFrames.frames().top();
    if (!sourceFrame ||
        sourceFrame->procedureId != compilerManifest.procedureId ||
        sourceFrame->resumePc != compilerManifest.resumePc)
        return std::nullopt;

    const bool rootScript = compilerManifest.procedureId == 0;
    if ((rootScript && (rootedFrames.depth() != 1 ||
                        sourceFrame->arguments != 0)) ||
        (!rootScript && rootedFrames.depth() <= 1))
        return std::nullopt;

    const auto gc = auditDetachedSsaGcRoots(saved, compilerManifest);
    if (!gc.knownGeneratedRootsConsistent())
        return std::nullopt;
    // Independently verify all direct arguments and compiler-proven
    // PHI/SELECT aliases against the real rooted CallFrame. This is
    // RT-001's existing stricter verifier, not another approximation.
    const auto rooted = xyo::rt::verifyRootedSsaArguments(
        saved, rootedFrames, rootedFrames.depth(),
        compilerManifest.procedureId, sourceFrame->arguments, true);
    if (!rooted || !rooted->matchedAllObservedArguments)
        return std::nullopt;

    SourceSsaLocationAudit result;
    result.rootedFrameRevision = rootedFrames.frames().revision();
    result.rootedArgumentStrings = rooted->rootedStringArguments +
                                   rooted->rootedAliasedStrings;
    result.nativeGeneratedStrings = gc.certifiedGeneratedStrings;
    result.unrootedWords = gc.unrootedBoxedStrings;
    result.unresolvedNativePointers = saved.unresolvedPointers;
    result.unresolvedOther = saved.unresolvedOther;
    result.locations.reserve(saved.descriptors.size());

    for (size_t i = 0; i < saved.descriptors.size(); ++i) {
        const uint64_t desc = saved.descriptors[i];
        const uint32_t id = uint32_t(desc);
        const uint32_t kindWidth = uint32_t(desc >> 32);
        const uint32_t family = kindWidth & ~uint32_t{255};
        const uint32_t width = kindWidth & 255u;
        const uint32_t alias = saved.compilerAliasArguments[i];
        SourceSsaLocation location{desc};
        if (saved.compilerGeneratedStrings[i]) {
            location.kind = SourceSsaLocationKind::NativeGeneratedString;
        } else if (alias) {
            // Scratch procedure args are always boxed i64. No float
            // argument, raw pointer, or arithmetic transformation can
            // accidentally inherit a source argument mapping.
            const bool instruction =
                family == 0 && width == 64 && !(id & 0x80000000u);
            const bool originalArgument =
                family == 0x200 && width == 64 &&
                (id & 0x80000000u) &&
                (id & 0x7fffffffu) == alias;
            if (!instruction && !originalArgument)
                return std::nullopt;
            location.kind = SourceSsaLocationKind::ProcedureArgument;
            location.argument = alias; // one-based index in leaf frame
            ++result.mappedArguments;
        } else if (family == 0x200) {
            // Native i64 Scratch arguments MUST be represented by
            // exact compiler source-argument ancestry.
            return std::nullopt;
        } else {
            ++result.unmappedWords;
        }
        result.locations.push_back(location);
    }
    // Even all-known arguments and generated temporaries do not
    // describe which expression values / loop locals a resumed
    // interpreter would need. Never infer sourceLocal completeness.
    return result;
}

} // namespace xyo::ir
