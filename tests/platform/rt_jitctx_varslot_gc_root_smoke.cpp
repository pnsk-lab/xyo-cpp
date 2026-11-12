// RT-001: a real Runtime/Target JitCtx variable-slot ownership audit.
// The pointer slots are NOT GC roots themselves. Runtime::markRoots
// owns Variable::value and Variable::list via Target::variables, while
// structural target edits invalidate cached Variable* ownership.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/gc.hpp"
#include "core/sstr.hpp"
#include "engine/runtime.hpp"
#include "engine/thread.hpp"
#include "jit/jit.hpp"
#include "model/target.hpp"

#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <string>

int main() {
    using namespace xyo;
    auto& gc = Gc::get();
    gc.setThreshold(128u << 20);
    {
        Runtime runtime;
        auto* stage = new Target(&runtime);
        stage->isStage = true;
        runtime.addTarget(stage);

        Thread thread("rt001-varslot-root");
        thread.target = stage;
        JitCtx ctx{};
        ctx.thread = &thread;
        ctx.target = stage;
        ctx.runtime = &runtime;
        const bool fast = [] {
            const char* env = std::getenv(
                "XYO_RT_GUARD_NATIVE_VAR_SLOTS_FAST");
            return env && env[0] == '1';
        }();
        const bool indexed = fast && [] {
            const char* env = std::getenv(
                "XYO_RT_GUARD_NATIVE_VAR_SLOTS_INDEXED");
            return env && env[0] == '1';
        }();
        const char* varId = "rt001-variable";
        const char16_t* varName = u"GC rooted";

        // Actual native runtime helper: JitCtx::varSlotData points
        // into Thread::jitVarSlots, and the elements are Variable*.
        xyo_varslot_init(&ctx, 0, varId, varName, 0);
        if (fast) {
            // At least one hit with no intervening structural change.
            const uint64_t hits = thread.nativeVarCacheHits;
            assert(xyo_varslot_checked(&ctx, 0, varId, varName, 0));
            // Hash cache was populated by varslot_init; the indexed
            // cache lazily inserts its first entry on checked access.
            if (indexed)
                assert(xyo_varslot_checked(
                    &ctx, 0, varId, varName, 0));
            assert(thread.nativeVarCacheHits > hits);
            assert(thread.nativeVarCacheValid);
        }
        assert(ctx.varSlotData == thread.jitVarSlots.data());
        assert(thread.jitVarSlots.size() == 1);
        Variable* variable = thread.jitVarSlots[0];
        assert(variable != nullptr);
        auto it = stage->variables.find("rt001-variable");
        assert(it != stage->variables.end() &&
               &it->second == variable);

        // A real GC-managed SStr is rooted by Runtime::markRoots
        // through Target::variables, NOT by the cached Variable*.
        const std::u16string message(2048, u'G');
        SStr* ownedString = SStr::create(message);
        variable->value = Value::string(ownedString);
        GcHeader* header = reinterpret_cast<GcHeader*>(ownedString) - 1;
        assert(gc.isObject(header));
        // Test Runtime's root provider directly. Survival alone could
        // also have been caused by conservative native stack scanning.
        assert(header->mark == 0);
        runtime.markRoots(gc);
        assert(header->mark == 1);
        gc.drainPending();
        const uint64_t before = gc.collectCount();
        gc.collect();
        assert(gc.collectCount() == before + 1);
        assert(gc.isObject(header));
        assert(header->type == GcType::Str);
        assert(header->mark == 0);
        assert(variable->value.asStrPtr() == ownedString);

        // Unordered-map rehash does not move the map node. Its
        // STRUCTURAL generation nevertheless changes. A future GC
        // proof may not mistake a cached pointer for stable ownership.
        const auto originalStamp = stage->variableBindingStamp();
        for (unsigned i = 0; i < 80; ++i) {
            stage->lookupOrCreateVariable(
                "rt001-extra-" + std::to_string(i), u"extra");
        }
        assert(stage->variableBindingStamp() != originalStamp);
        assert(ctx.varSlotData == thread.jitVarSlots.data());
        assert(thread.jitVarSlots[0] == variable);
        assert(&stage->variables.find("rt001-variable")->second == variable);
        gc.collect();
        assert(gc.isObject(header));
        assert(variable->value.asStrPtr() == ownedString);

        // Deletion changes binding generation and destroys the map
        // node. Crucially the Thread's cached pointer IS NOT
        // automatically invalidated. Do not dereference it.
        const auto beforeErase = stage->variableBindingStamp();
        stage->deleteVariable("rt001-variable");
        assert(stage->variableBindingStamp() != beforeErase);
        assert(stage->variables.find("rt001-variable") ==
               stage->variables.end());
        assert(thread.jitVarSlots[0] == variable);
        bool stillOwned = false;
        for (const auto& [id, item] : stage->variables) {
            if (&item == variable) stillOwned = true;
        }
        assert(!stillOwned);

        // Even if the allocator reuses the same node address, the
        // new generation must reject an old source ownership proof.
        auto* replacement = stage->lookupOrCreateVariable(
            "rt001-variable", u"GC rooted");
        assert(replacement != nullptr);
        assert(stage->variableBindingStamp() != beforeErase);
        // A raw address comparison is NOT a substitute for the stamp:
        // replacement may or may not equal the retired pointer.
        // Rebind BEFORE ever dereferencing the cached raw pointer,
        // independent of whether the allocator reused its address.
        const uint64_t oldInvalidations =
            thread.nativeVarCacheInvalidations;
        Variable* checked = xyo_varslot_checked(
            &ctx, 0, varId, varName, 0);
        if (fast)
            assert(thread.nativeVarCacheInvalidations > oldInvalidations);
        assert(checked == replacement);
        assert(thread.jitVarSlots[0] == replacement);
        assert(ctx.varSlotData == thread.jitVarSlots.data());
        assert(xyo_jitVar(&ctx, varId, varName, 0) == replacement);
        if (fast)
            assert(thread.nativeVarCacheHits > 0 &&
                   thread.nativeVarCacheMisses > 0);

        const auto beforeReerase = stage->variableBindingStamp();
        stage->deleteVariable("rt001-variable");
        assert(stage->variableBindingStamp() != beforeReerase);
        // The newly checked helper must CREATE AND BIND the current
        // Scratch variable rather than borrowing a deleted node.
        Variable* recreated = xyo_varslot_checked(
            &ctx, 0, "rt001-variable", u"GC rooted", 0);
        assert(recreated ==
               stage->lookupVariableById("rt001-variable"));
        assert(thread.jitVarSlots[0] == recreated);
        assert(ctx.varSlotData == thread.jitVarSlots.data());

        // The list path has a different GC ownership type (SList*),
        // so ensure checked resolution also refreshes list slots.
        Variable* list = xyo_varslot_checked(
            &ctx, 1, "rt001-list", u"Checked list", 1);
        assert(list && list->list);
        assert(thread.jitVarSlots[1] == list);
        stage->deleteVariable("rt001-list");
        Variable* reboundList = xyo_varslot_checked(
            &ctx, 1, "rt001-list", u"Checked list", 1);
        assert(reboundList && reboundList->list);
        assert(reboundList ==
               stage->lookupVariableById("rt001-list"));
        assert(thread.jitVarSlots[1] == reboundList);

        // Same numeric slot with a DIFFERENT immutable source key:
        // a dense cache must validate the full (id, name, list-kind),
        // not just the slot index and owner-generation stamp.
        Variable* alpha = stage->lookupOrCreateVariable(
            "rt001-index-alpha", u"alpha");
        Variable* beta = stage->lookupOrCreateVariable(
            "rt001-index-beta", u"beta");
        assert(alpha && beta && alpha != beta);
        const auto sameStamp = stage->variableBindingStamp();
        assert(xyo_varslot_checked(
            &ctx, 9, "rt001-index-alpha", u"alpha", 0) == alpha);
        assert(xyo_varslot_checked(
            &ctx, 9, "rt001-index-beta", u"beta", 0) == beta);
        assert(stage->variableBindingStamp() == sameStamp);
        assert(thread.jitVarSlots[9] == beta);
        assert(xyo_varslot_checked(
            &ctx, 9, "rt001-index-alpha", u"alpha", 0) == alpha);
        assert(stage->variableBindingStamp() == sameStamp);
        assert(thread.jitVarSlots[9] == alpha);

        // A sprite can initially borrow a stage-global binding and
        // later shadow it with a *local* variable with the same ID.
        // Cross-target rebinding must not reuse the stage's cached
        // Variable* even if both nodes have stable map addresses.
        auto* sprite = new Target(&runtime);
        runtime.addTarget(sprite);
        Variable* global = stage->lookupOrCreateVariable(
            "rt001-shared", u"shared");
        assert(global);
        thread.target = sprite;
        ctx.target = sprite;
        assert(xyo_varslot_checked(&ctx, 2, "rt001-shared", u"shared", 0) ==
               global);
        const auto beforeShadow = sprite->variableBindingStamp();
        auto [localIt, inserted] = sprite->variables.emplace(
            "rt001-shared",
            Variable::create("rt001-shared", u"shared",
                             Variable::kScalarType, false));
        assert(inserted);
        Variable* local = &localIt->second;
        assert(sprite->variableBindingStamp() != beforeShadow);
        assert(xyo_varslot_checked(&ctx, 2, "rt001-shared", u"shared", 0) ==
               local);
        assert(thread.jitVarSlots[2] == local);
        sprite->deleteVariable("rt001-shared");
        assert(xyo_varslot_checked(&ctx, 2, "rt001-shared", u"shared", 0) ==
               global);
        assert(thread.jitVarSlots[2] == global);
        // Stage-side erase/re-create must invalidate a sprite's
        // cached stage-global Variable* even though the SPRITE map
        // itself did not change. Its full binding stamp tracks both.
        const auto beforeStageErase = sprite->variableBindingStamp();
        const auto oldStageInvalidations =
            thread.nativeVarCacheInvalidations;
        stage->deleteVariable("rt001-shared");
        assert(sprite->variableBindingStamp() != beforeStageErase);
        Variable* newGlobal = stage->lookupOrCreateVariable(
            "rt001-shared", u"shared");
        assert(newGlobal);
        assert(xyo_varslot_checked(
            &ctx, 2, "rt001-shared", u"shared", 0) == newGlobal);
        assert(thread.jitVarSlots[2] == newGlobal);
        if (fast)
            assert(thread.nativeVarCacheInvalidations >
                   oldStageInvalidations);
    }
    std::puts(
        "PASS RT-001 JitCtx varSlotData: Target Variable::value retained "
        "by Runtime GC; rehash preserves node; deletion invalidates "
        "cached raw pointer and target generation");
}
