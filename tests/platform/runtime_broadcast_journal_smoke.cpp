// Runtime's *actual* opt-in broadcast delivery path, including reentrant
// nested non-waiting broadcasts and collections during the callback.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/engine/runtime.hpp"
#include "../../src/ir/commit_once_effects.hpp"
#include "../../src/core/gc.hpp"
#include "../../src/core/sstr.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace xyo;

namespace {
struct Context {
    Runtime& runtime;
    std::vector<std::string> order;
};

void nested(void* state, std::u16string_view name) noexcept {
    auto& context = *static_cast<Context*>(state);
    Gc::get().collect(); // nested UTF-16 must remain valid after collection
    assert(name == u"nested 世界");
    context.order.push_back("nested");
}

void outer(void* state, std::u16string_view name) noexcept {
    auto& context = *static_cast<Context*>(state);
    assert(name == u"outer 🌟");
    context.order.push_back("outer/before");
    Gc::get().collect(); // journal must protect the outer UTF-16 payload
    // Pass a view borrowed from a GC-managed Scratch string: a nested
    // callback that collects must not observe freed or corrupted UTF-16.
    SStr* heapName = SStr::create(u"nested 世界");
    context.runtime.journalBroadcast(heapName->view(), nested, &context);
    context.order.push_back("outer/after");
}
}

struct EffectContext {
    Runtime& runtime;
    unsigned effects = 0;
};

void nestedEffect(void* state, Value message) noexcept {
    auto& context = *static_cast<EffectContext*>(state);
    Gc::get().collect(); // nested exact root is live during collection
    assert(message.isString());
    assert(message.asStrPtr()->view() == u"GC保持 🌠");
    ++context.effects;
}

void outerEffect(void* state, Value message) noexcept {
    auto& context = *static_cast<EffectContext*>(state);
    assert(message.isNumber());
    context.runtime.journalObservable(ir::ObservableOp::Say,
        Value::string(SStr::create(u"GC保持 🌠")), nestedEffect, &context);
    ++context.effects;
}

int main() {
#if defined(_WIN32)
    _putenv_s("XYO_EFFECT_JOURNAL", "1");
#else
    setenv("XYO_EFFECT_JOURNAL", "1", 1);
#endif
    Runtime runtime;
    assert(runtime.observableJournal);
    Context context{runtime, {}};
    runtime.journalBroadcast(u"outer 🌟", outer, &context);
    assert((context.order == std::vector<std::string>{
        "outer/before", "nested", "outer/after"}));
    assert(runtime.observableCommittedBroadcasts == 1);
    assert(runtime.observableImmediateBroadcasts == 1);
    assert(runtime.observableJournal->pendingCount() == 0);
    assert(runtime.observableJournal->rootedPayloads() == 0);

    // A later independent broadcast should receive a fresh monotonic ID,
    // not be confused with the previous committed/reentrant event.
    runtime.journalBroadcast(u"nested 世界", nested, &context);
    assert(context.order.back() == "nested");
    assert(runtime.observableCommittedBroadcasts == 2);
    assert(runtime.observableImmediateBroadcasts == 1);
    assert(runtime.observableSliceId == 2);
    assert(runtime.observableJournal->pendingCount() == 0);
    assert(runtime.observableJournal->rootedPayloads() == 0);

    EffectContext effects{runtime};
    runtime.journalObservable(ir::ObservableOp::Say,
        Value::num(5), outerEffect, &effects);
    assert(effects.effects == 2);
    assert(runtime.observableCommittedEffects == 1);
    assert(runtime.observableImmediateEffects == 1);
    assert(runtime.observableJournal->pendingCount() == 0);
    assert(runtime.observableJournal->rootedPayloads() == 0);
    std::cout << "PASS Runtime journal: real nested/reentrant broadcast, "
                 "forced GC, ordered single dispatch" << '\n';
}
