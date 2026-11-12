// RT-001 pure ABI verifier edge cases. Real GcValueHandles/collection
// are tested by native_simple_resume.py on actual rooted Scratch frames.
// This mock models only the read-only resolved CallFrame view.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "../../src/jit/rt_rooted_ssa_arguments.hpp"
#include "../../src/engine/thread.hpp"

#include <cassert>
#include <cstdint>
#include <optional>
#include <vector>
#include <cstdio>

using namespace xyo;

struct RootedFramesView {
    bool healthy = true;
    uint32_t rootedStrings = 1;
    std::vector<std::vector<Value>> sourceFrames;

    bool valid() const { return healthy; }
    size_t depth() const { return sourceFrames.size(); }
    size_t rootedArguments() const { return rootedStrings; }

    std::optional<Value> argument(
        uint32_t frameIndex, uint32_t argumentIndex) const {
        if (frameIndex >= sourceFrames.size() ||
            argumentIndex >= sourceFrames[frameIndex].size())
            return std::nullopt;
        return sourceFrames[frameIndex][argumentIndex];
    }
};

uint64_t tagged(uint32_t id, uint32_t family, uint32_t width) {
    return uint64_t(id) |
           (uint64_t(family | width) << 32);
}

int main() {
    // An opaque string tag must NOT be dereferenced by this verifier.
    // The production CallFrame resolves the real, independently rooted
    // SStr; this unit only supplies the same bits with a mock.
    const Value stringArg{val::kTagStr | uint64_t{0x12345670}};
    const Value numArg = Value::num(42.0);
    RootedFramesView rootView{true, 1,
                              {{}, {stringArg, numArg}}};
    NativeRtSsaCapture capture;
    capture.descriptors = {
        tagged(10, 0, 64),
        tagged(0x80000001u, 0x200, 64),
        tagged(0x80000002u, 0x200, 64)
    };
    capture.words = {123, stringArg.bits, numArg.bits};
    capture.fingerprint = capture.computeFingerprint();

    const auto good = rt::verifyRootedSsaArguments(
        capture, rootView, 2, 17, 2);
    assert(good && good->matchedAllObservedArguments);
    assert(good->matchedArguments == 2);
    assert(good->rootedStringArguments == 1);
    assert(good->unverifiedInstructionWords == 1);

    auto changed = capture;
    changed.words[1] ^= 1;
    changed.fingerprint = changed.computeFingerprint();
    assert(!rt::verifyRootedSsaArguments(
        changed, rootView, 2, 17, 2));
    changed = capture;
    changed.words[2] ^= 1;
    changed.fingerprint = changed.computeFingerprint();
    assert(!rt::verifyRootedSsaArguments(
        changed, rootView, 2, 17, 2));

    changed = capture;
    changed.descriptors[2] = tagged(0x80000003u, 0x200, 64);
    changed.fingerprint = changed.computeFingerprint();
    assert(!rt::verifyRootedSsaArguments(
        changed, rootView, 2, 17, 2));
    changed = capture;
    changed.descriptors[1] = tagged(0x80000000u, 0x200, 64);
    changed.fingerprint = changed.computeFingerprint();
    assert(!rt::verifyRootedSsaArguments(
        changed, rootView, 2, 17, 2));
    changed = capture;
    changed.descriptors[1] = tagged(0x80000001u, 0x300, 64);
    changed.fingerprint = changed.computeFingerprint();
    assert(!rt::verifyRootedSsaArguments(
        changed, rootView, 2, 17, 2));
    changed = capture;
    changed.descriptors[1] = tagged(0x80000001u, 0x200, 32);
    changed.fingerprint = changed.computeFingerprint();
    assert(!rt::verifyRootedSsaArguments(
        changed, rootView, 2, 17, 2));
    assert(!rt::verifyRootedSsaArguments(
        capture, rootView, 1, 17, 2));
    assert(!rt::verifyRootedSsaArguments(
        capture, rootView, 2, 0, 2));
    assert(!rt::verifyRootedSsaArguments(
        capture, rootView, 2, 17, 1));

    auto withNoRoots = rootView;
    withNoRoots.rootedStrings = 0;
    assert(!rt::verifyRootedSsaArguments(
        capture, withNoRoots, 2, 17, 2));
    auto broken = rootView;
    broken.healthy = false;
    assert(!rt::verifyRootedSsaArguments(
        capture, broken, 2, 17, 2));
    changed = capture;
    changed.fingerprint ^= 1;
    assert(!rt::verifyRootedSsaArguments(
        changed, rootView, 2, 17, 2));

    NativeRtSsaCapture rootCapture;
    rootCapture.descriptors = {tagged(10, 0x100, 64)};
    rootCapture.words = {0x3ff0000000000000ULL};
    rootCapture.fingerprint = rootCapture.computeFingerprint();
    RootedFramesView rootOnly{true, 0, {{}}};
    const auto noArguments = rt::verifyRootedSsaArguments(
        rootCapture, rootOnly, 1, 0, 0);
    assert(noArguments && noArguments->matchedArguments == 0);
    rootCapture.descriptors.push_back(
        tagged(0x80000001u, 0x200, 64));
    rootCapture.words.push_back(numArg.bits);
    rootCapture.fingerprint = rootCapture.computeFingerprint();
    assert(!rt::verifyRootedSsaArguments(
        rootCapture, rootOnly, 1, 0, 0));

    // A nonargument i64 SSA PHI/SELECT can inherit a rooted
    // Scratch string only when the *compiler* proves exact same
    // argument bits on all incoming paths. Original vs host copy
    // provenance and the runtime value are verified independently.
    auto aliases = capture;
    aliases.descriptors.insert(
        aliases.descriptors.begin(), tagged(9, 0, 64));
    aliases.words.insert(aliases.words.begin(), stringArg.bits);
    aliases.compilerAliasArguments = {1, 0, 1, 2};
    aliases.aliasArguments = aliases.compilerAliasArguments;
    aliases.fingerprint = aliases.computeFingerprint();
    const auto aliasOk = rt::verifyRootedSsaArguments(
        aliases, rootView, 2, 17, 2);
    assert(aliasOk && aliasOk->matchedArguments == 2);
    assert(aliasOk->matchedAliasedInstructions == 1);
    assert(aliasOk->rootedAliasedStrings == 1);
    assert(aliasOk->rootedStringArguments == 1);
    assert(aliasOk->unverifiedInstructionWords == 1);

    assert(rt::verifyRootedSsaArguments(
        aliases, rootView, 2, 17, 2, true));
    auto noAliases = aliases;
    noAliases.aliasArguments.clear();
    noAliases.compilerAliasArguments.clear();
    noAliases.fingerprint = noAliases.computeFingerprint();
    // Old purely direct ABI tests can use the permissive legacy mode,
    // while the production gate MUST reject a missing alias manifest.
    assert(rt::verifyRootedSsaArguments(
        noAliases, rootView, 2, 17, 2));
    assert(!rt::verifyRootedSsaArguments(
        noAliases, rootView, 2, 17, 2, true));

    auto forgedAlias = aliases;
    forgedAlias.words.front() ^= uint64_t{1};
    forgedAlias.fingerprint = forgedAlias.computeFingerprint();
    assert(!rt::verifyRootedSsaArguments(
        forgedAlias, rootView, 2, 17, 2));
    forgedAlias = aliases;
    forgedAlias.aliasArguments.front() = 2;
    forgedAlias.fingerprint = forgedAlias.computeFingerprint();
    assert(!rt::verifyRootedSsaArguments(
        forgedAlias, rootView, 2, 17, 2));
    forgedAlias = aliases;
    forgedAlias.compilerAliasArguments.front() = 2;
    assert(!rt::verifyRootedSsaArguments(
        forgedAlias, rootView, 2, 17, 2));
    forgedAlias = aliases;
    forgedAlias.aliasArguments.pop_back();
    forgedAlias.fingerprint = forgedAlias.computeFingerprint();
    assert(!rt::verifyRootedSsaArguments(
        forgedAlias, rootView, 2, 17, 2));
    forgedAlias = aliases;
    forgedAlias.aliasArguments.front() = 0;
    forgedAlias.compilerAliasArguments.front() = 0;
    forgedAlias.fingerprint = forgedAlias.computeFingerprint();
    const auto deliberatelyUnproven = rt::verifyRootedSsaArguments(
        forgedAlias, rootView, 2, 17, 2);
    assert(deliberatelyUnproven &&
           deliberatelyUnproven->matchedAliasedInstructions == 0 &&
           deliberatelyUnproven->unverifiedInstructionWords == 2);

    std::puts("PASS RT-001: rooted SSA ABI args and bit-preserving "
              "nonargument aliases, forged word/map fail closed");
}
