// RT-001: exact GC-owned JIT-created String SSA, without raw-pointer guesses.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/jit/rt_generated_ssa_roots.hpp"
#include "../../src/core/sstr.hpp"
#include <cassert>
#include <cstdio>

using namespace xyo;

static NativeRtSsaCapture make(uint64_t bits) {
    NativeRtSsaCapture state;
    state.descriptors = {(uint64_t(64) << 32) | 5};
    state.words = {bits};
    state.aliasArguments = {0};
    state.compilerAliasArguments = {0};
    state.generatedStrings = {1};
    state.compilerGeneratedStrings = {1};
    state.fingerprint = state.computeFingerprint();
    return state;
}

int main() {
    Gc& gc = Gc::get();
    auto value = Value::string(SStr::create(
        u"native join value kept through Fiber retirement 𠮷\U0001f9ea"));
    auto state = make(value.bits);
    assert(rt::retainGeneratedSsaRoots(state));
    assert(state.generatedStringRoots);
    assert(state.generatedStringRoots->size() == 1);
    auto audit = rt::verifyGeneratedSsaRoots(state);
    assert(audit && audit->provenStringSlots == 1 &&
        audit->rootedHeapStrings == 1 && audit->immediateStrings == 0);

    gc.collect();
    audit = rt::verifyGeneratedSsaRoots(state);
    assert(audit && audit->rootedHeapStrings == 1);

    // Diagnostic snapshots were previously copyable. Keeping a
    // frozen copy must NOT drop its shared exact root provider when
    // the original Thread-owned snapshot is reset/replaced.
    auto copied = state;
    state.generatedStringRoots.reset();
    state.generatedStringHandles.clear();
    gc.collect();
    audit = rt::verifyGeneratedSsaRoots(copied);
    assert(audit && audit->rootedHeapStrings == 1);
    state = std::move(copied);

    // A forged diagnostic i64 word with a repaired FNV is still
    // refused: source-owned exact GC handle remains authoritative.
    state.words[0] ^= uint64_t{1};
    state.fingerprint = state.computeFingerprint();
    assert(!rt::verifyGeneratedSsaRoots(state));
    state.words[0] ^= uint64_t{1};
    state.fingerprint = state.computeFingerprint();

    state.generatedStrings[0] = 0;
    state.fingerprint = state.computeFingerprint();
    assert(!rt::verifyGeneratedSsaRoots(state));
    state.generatedStrings[0] = 1;
    state.fingerprint = state.computeFingerprint();

    // A detached snapshot cannot fabricate its own new root: exact
    // source handle identity is required after capture.
    const auto handle = state.generatedStringHandles[0];
    assert(state.generatedStringRoots->release(handle));
    assert(!rt::verifyGeneratedSsaRoots(state));

    // Wrong tag, dangling fake string pointer, and untrusted source
    // kind all reject *before* any unsafe dereference or root mutation.
    auto numeric = make(Value::num(42).bits);
    assert(!rt::retainGeneratedSsaRoots(numeric));
    auto forged = make(val::kTagStr | 0x123450);
    assert(!rt::retainGeneratedSsaRoots(forged));

    auto wrongKind = make(value.bits);
    wrongKind.descriptors[0] =
        uint64_t(0x200 | 64) << 32 | 0x80000001u;
    wrongKind.fingerprint = wrongKind.computeFingerprint();
    assert(!rt::retainGeneratedSsaRoots(wrongKind));

    auto noProof = make(value.bits);
    noProof.generatedStrings[0] = 0;
    noProof.fingerprint = noProof.computeFingerprint();
    assert(!rt::retainGeneratedSsaRoots(noProof));

    auto wrongOwner = make(value.bits);
    wrongOwner.aliasArguments[0] = 1;
    wrongOwner.fingerprint = wrongOwner.computeFingerprint();
    assert(!rt::retainGeneratedSsaRoots(wrongOwner));

    // A string literal's empty sentinel is an immediate, not heap.
    auto empty = make(Value::emptyStr().bits);
    assert(rt::retainGeneratedSsaRoots(empty));
    audit = rt::verifyGeneratedSsaRoots(empty);
    assert(audit && audit->provenStringSlots == 1 &&
        audit->rootedHeapStrings == 0 && audit->immediateStrings == 1);
    assert(!empty.generatedStringRoots);

    // Legacy/native CallFrame argument modes do not secretly create
    // GC ownership for unrelated numeric or string SSA slots.
    auto unproven = make(value.bits);
    unproven.generatedStrings[0] = 0;
    unproven.compilerGeneratedStrings[0] = 0;
    unproven.fingerprint = unproven.computeFingerprint();
    assert(rt::retainGeneratedSsaRoots(unproven));
    audit = rt::verifyGeneratedSsaRoots(unproven);
    assert(audit && audit->rootedHeapStrings == 0 &&
        audit->provenStringSlots == 0 &&
        !unproven.generatedStringRoots);

    std::puts("PASS RT-001: generated native SSA exact SStr GC handle, "
              "full GC, forged word/source/handle, fake pointer and "
              "type/owner/sentinel guards");
}
