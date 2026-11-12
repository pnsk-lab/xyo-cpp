// AOT v3 lazy BlockCached descriptor must resolve once on first use,
// stay pointer-stable across threads, and remain valid across source edits/GC.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/jit/aot_lazy_cache.hpp"
#include "../../src/jit/aot_emit.hpp"
#include "../../src/engine/runtime.hpp"
#include "../../src/core/gc.hpp"
#include "../../src/core/sstr.hpp"
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>

int main() {
    using namespace xyo;
    // Capture every use, not just new symbols. Two scripts may reuse the
    // same BlockCached descriptor without losing either worker prewarm list.
    AotEmit emit;
    emit.packedSlots = true;
    emit.lazyCached = true;
    emit.beginScript();
    const auto slot0 = emit.slotIndex(AotSlotKind::BlockCached, "0\\x01call");
    assert(emit.slotIndex(AotSlotKind::BlockCached, "0\\x01call") == slot0);
    const auto slot1 = emit.slotIndex(AotSlotKind::GpuProgram, "0\\x01loop");
    assert(emit.scriptCachedSlots.size() == 2);
    assert(emit.scriptUsesGpu);
    emit.endScript();
    emit.beginScript();
    assert(emit.slotIndex(AotSlotKind::BlockCached, "0\\x01call") == slot0);
    assert(emit.scriptCachedSlots.size() == 1);
    assert(!emit.scriptUsesGpu);
    emit.endScript();
    assert(slot1 != slot0);
    Runtime rt;
    // START-016: field edits that commit through Blocks must always
    // change the AOT source revision and rebuild immutable cached ops.
    Blocks editedBlocks(&rt);
    Block num;
    num.id = "editor-shadow";
    num.opcode = "math_number";
    num.shadow = true;
    num.fields["NUM"].value = Value::num(7);
    editedBlocks.createBlock(std::move(num));
    const uint64_t beforeMissing = editedBlocks.revision();
    assert(!editedBlocks.setBlockField(
        "missing", "NUM", BlockField{Value::num(9)}));
    assert(editedBlocks.revision() == beforeMissing);
    const BlockCached* originalShadow =
        editedBlocks.getCachedExecute("editor-shadow");
    assert(originalShadow != nullptr);
    assert(originalShadow->fields.at("NUM").value == Value::num(7));
    AotLazyCachedSlot editedSource(
        &editedBlocks, "editor-shadow", editedBlocks.revision());
    assert(xyo_rt_aot_cached(&editedSource) == originalShadow);
    const uint64_t previous = editedBlocks.revision();
    assert(editedBlocks.setBlockField(
        "editor-shadow", "NUM", BlockField{Value::num(12)}));
    assert(editedBlocks.revision() != previous);
    const BlockCached* updatedShadow =
        editedBlocks.getCachedExecute("editor-shadow");
    assert(updatedShadow && updatedShadow != originalShadow);
    assert(updatedShadow->fields.at("NUM").value == Value::num(12));
    assert(originalShadow->fields.at("NUM").value == Value::num(7));
    assert(xyo_rt_aot_cached(&editedSource) == nullptr);

    auto* target = new Target(&rt);
    target->isStage = true;
    rt.addTarget(target);
    Blocks& blocks = *target->blocks;

    Block literal;
    literal.id = "text";
    literal.opcode = "text";
    literal.fields["TEXT"].value =
        Value::string(SStr::create(u"GCルート維持🧪"));
    blocks.createBlock(std::move(literal));

    Block caller;
    caller.id = "call";
    caller.opcode = "procedures_call";
    caller.inputs.set("VALUE", {"text", "text"});
    Mutation mutation;
    mutation.tagName = "mutation";
    mutation.attrs["proccode"] = "f %s";
    mutation.attrs["argumentids"] = "[\"a\"]";
    mutation.attrs["argumentnames"] = "[\"a\"]";
    mutation.attrs["argumentdefaults"] = "[\"🧪\"]";
    caller.mutation = std::move(mutation);
    blocks.createBlock(std::move(caller));

    AotLazyCachedSlot descriptor(&blocks, "call", blocks.revision());
    assert(descriptor.resolved.load() == nullptr);
    assert(blocks.cache().executeCached.empty());
    const BlockCached* const original = xyo_rt_aot_cached(&descriptor);
    assert(original && original->mutation);
    assert(original->mutation->attr("proccode") == "f %s");
    assert(!original->ops.empty());
    assert(descriptor.resolved.load() == original);
    assert(xyo_rt_aot_cached(&descriptor) == original);

    std::atomic<bool> consistent{true};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i)
        readers.emplace_back([&] {
            for (int n = 0; n < 10000; ++n)
                if (xyo_rt_aot_cached(&descriptor) != original)
                    consistent = false;
        });
    for (auto& reader : readers) reader.join();
    assert(consistent);

    const uint64_t originalRevision = blocks.revision();
    blocks.resetCache();
    assert(blocks.revision() != originalRevision);
    assert(xyo_rt_aot_cached(&descriptor) == nullptr);
    const BlockCached* fresh = blocks.getCachedExecute("call");
    assert(fresh && fresh != original);
    assert(xyo_rt_aot_cached(&descriptor) == nullptr);
    Gc::get().collect();
    assert(original->mutation->attr("proccode") == "f %s");
    assert(original->argValues.mutation == original->mutation);

    AotLazyCachedSlot literalDescriptor(&blocks, "text", blocks.revision());
    const BlockCached* text = xyo_rt_aot_cached(&literalDescriptor);
    assert(text && text->fields.at("TEXT").value.isString());
    assert(text->fields.at("TEXT").value.asStrPtr()->view() ==
           u"GCルート維持🧪");

    // A descriptor not yet resolved when its source disappears must not
    // bind an edited replacement with the same identifier. The scheduler
    // must reject the corresponding compiled script before entering LLVM.
    AotLazyCachedSlot unresolvedAtEdit(&blocks, "call", blocks.revision());
    assert(unresolvedAtEdit.resolved.load() == nullptr);
    blocks.deleteBlock("call");
    assert(blocks.getBlock("call") == nullptr);
    assert(xyo_rt_aot_cached(&unresolvedAtEdit) == nullptr);
    Block replacement;
    replacement.id = "call";
    replacement.opcode = "control_wait";
    blocks.createBlock(std::move(replacement));
    assert(xyo_rt_aot_cached(&unresolvedAtEdit) == nullptr);
    assert(unresolvedAtEdit.resolved.load() == nullptr);
    Gc::get().collect();
    assert(xyo_rt_aot_cached(&descriptor) == nullptr);
    assert(original->mutation->attr("proccode") == "f %s");
    assert(text->fields.at("TEXT").value.asStrPtr()->view() ==
           u"GCルート維持🧪");
    return 0;
}
