// RT-001: invalid boxed string must unregister transient root owner
// before entering an abortable native async reporter.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/jit/jit.hpp"
#include "../../src/engine/thread.hpp"
#include "../../src/core/gc.hpp"
#include "../../src/core/sstr.hpp"
#include <cassert>
#include <cstdio>

extern "C" uint64_t xyo_rt_async_reporter_rooted(
    xyo::JitCtx*, const char*, const uint64_t*, uint32_t);

int main() {
    using namespace xyo;
    Thread thread("invalid_native_async_root");
    thread.isKilled = true; // Reporter exits without stepping a Sequencer.
    JitCtx ctx{};
    ctx.thread = &thread;
    auto& gc = Gc::get();
    assert(xyo_rt_async_reporter_rooted(
        nullptr, "invalid_native_async_root", nullptr, 1)
           == val::kUndefined);
    assert(xyo_rt_async_reporter_rooted(
        &ctx, nullptr, nullptr, 1) == val::kUndefined);
    const uint64_t invalid[] = {
        val::kTagStr | 1u,
        val::kTagStr | 0x123450u
    };
    for (const uint64_t bits : invalid) {
        assert(xyo_rt_async_reporter_rooted(
            &ctx, "invalid_native_async_root", &bits, 1)
               == val::kUndefined);
        assert(thread.nativeAsyncVariableRoots.empty());
        // Would use-after-free if the rejected GC provider remained
        // registered but existed only on the retired native stack.
        const auto before = gc.collectCount();
        gc.collect();
        assert(gc.collectCount() > before);
        thread.stack.clear();
    }
    // Partial success then failure: a real GC-owned string is retained
    // before an invalid payload rejects the entire batch. The tentative
    // provider must be destroyed BEFORE a potentially abortable yield.
    const uint64_t partiallyValid[] = {
        Value::string(SStr::create(u"valid-first")).bits,
        val::kTagStr | 1u
    };
    assert(xyo_rt_async_reporter_rooted(
        &ctx, "invalid_native_async_root", partiallyValid, 2)
           == val::kUndefined);
    assert(thread.nativeAsyncVariableRoots.empty());
    const auto beforeMixed = gc.collectCount();
    gc.collect();
    assert(gc.collectCount() > beforeMixed);
    thread.stack.clear();
    std::puts("PASS RT-001 invalid i64 string tags reject unowned roots "
              "and subsequent full GC is safe; gc_safe=0");
}
