// RT-001: compare LLVM argument SSA with the already GC-rooted Scratch
// procedure CallFrame. No arbitrary SSA value is added as a GC root.
// This diagnostic does not authorize executable generic deoptimization.
#pragma once

#include "../core/value.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>

namespace xyo::rt {

struct RootedSsaArgumentAudit {
    uint32_t matchedArguments = 0;
    uint32_t rootedStringArguments = 0;
    uint32_t matchedAliasedInstructions = 0;
    uint32_t rootedAliasedStrings = 0;
    uint32_t unverifiedInstructionWords = 0;
    bool matchedAllObservedArguments = false;
};

// RootedFrames offers valid(), depth(), rootedArguments(), argument().
// Only after a separate source/typed-manifest check, source CallFrame
// verification and construction of a separately rooted source export.
template<class Detached, class RootedFrames>
std::optional<RootedSsaArgumentAudit> verifyRootedSsaArguments(
    const Detached& saved, const RootedFrames& roots,
    uint32_t frameCount, uint64_t procedureId, uint32_t argumentCount,
    bool requireCompiledAliases = false) {
    if (!roots.valid() || !frameCount || roots.depth() != frameCount ||
        saved.words.size() != saved.descriptors.size() ||
        saved.fingerprint != saved.computeFingerprint())
        return std::nullopt;

    // Original compiler ancestry and scheduler-copy provenance must
    // match exactly. A locally rehashed snapshot is not authorization
    // to rename its original SSA->argument owner.
    const bool hasAliases = !saved.aliasArguments.empty() ||
        !saved.compilerAliasArguments.empty();
    // The RT production gate must never silently downgrade to
    // argument-only validation if *both* compiler and runtime alias
    // manifests were dropped (with a repaired local FNV).
    if (requireCompiledAliases && !hasAliases)
        return std::nullopt;
    if (hasAliases &&
        (saved.aliasArguments.size() != saved.words.size() ||
         saved.compilerAliasArguments.size() != saved.words.size() ||
         saved.aliasArguments != saved.compilerAliasArguments))
        return std::nullopt;

    const bool isRootScript = procedureId == 0;
    if ((isRootScript && (frameCount != 1 || argumentCount != 0)) ||
        (!isRootScript && frameCount <= 1))
        return std::nullopt;

    RootedSsaArgumentAudit result;
    for (size_t i = 0; i < saved.descriptors.size(); ++i) {
        const uint64_t descriptor = saved.descriptors[i];
        const uint32_t id = uint32_t(descriptor);
        const uint32_t family = uint32_t(descriptor >> 32) & ~uint32_t{255};
        const uint32_t width = uint32_t(descriptor >> 32) & 255u;
        const uint32_t alias =
            hasAliases ? saved.aliasArguments[i] : 0;
        const bool nativeArgument = family == 0x200 || family == 0x300;
        if (!nativeArgument && !alias) {
            ++result.unverifiedInstructionWords;
            continue;
        }

        // xyo-cpp native Scratch procedure ABI:
        //   function(JitCtx* nativeContext, i64 boxedScratchArg1, ...)
        // LLVM argument 0 is a pointer, NOT a Scratch Value.
        // Compiler-proven nonargument aliases require kind 0/i64
        // (an unchanged PHI/SELECT value of one original argument).
        if (isRootScript || width != 64 ||
            (nativeArgument && family != 0x200) ||
            (!nativeArgument && (family != 0 ||
                (id & 0x80000000u))))
            return std::nullopt;
        const uint32_t llvmArgument =
            nativeArgument ? (id & 0x7fffffffu) : alias;
        if (!llvmArgument || llvmArgument > argumentCount ||
            (nativeArgument && !(id & 0x80000000u)) ||
            (alias && alias != llvmArgument) ||
            (hasAliases && nativeArgument && alias == 0))
            return std::nullopt;
        const auto rooted = roots.argument(
            frameCount - 1, llvmArgument - 1);
        if (!rooted || rooted->bits != saved.words[i])
            return std::nullopt;
        if (rooted->isString() && !rooted->isEmptyString()) {
            // Resolve from the independently rooted Scratch CallFrame,
            // not by dereferencing a guessed LLVM SSA pointer.
            if (!roots.rootedArguments()) return std::nullopt;
            if (nativeArgument) ++result.rootedStringArguments;
            else ++result.rootedAliasedStrings;
        }
        if (nativeArgument) ++result.matchedArguments;
        else ++result.matchedAliasedInstructions;
    }
    result.matchedAllObservedArguments = true;
    return result;
}

} // namespace xyo::rt
