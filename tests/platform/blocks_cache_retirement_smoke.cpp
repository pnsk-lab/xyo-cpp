// A BlockCached remains valid while suspended native/interpreter frames
// still retain it, even after a real source cache invalidation and full GC.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/engine/runtime.hpp"
#include "../../src/core/gc.hpp"
#include "../../src/core/sstr.hpp"
#include <cassert>
#include <string_view>

int main() {
    using namespace xyo;
    Runtime rt;
    auto* target = new Target(&rt);
    target->isStage = true;
    rt.addTarget(target);
    Blocks& blocks = *target->blocks;

    Block literal;
    literal.id = "text";
    literal.opcode = "text";
    literal.fields["TEXT"].value =
        Value::string(SStr::create(u"保持する文字列𠮷🧪"));
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

    BlockCached* const old = blocks.getCachedExecute("call");
    assert(old && old->mutation && old->argValues.mutation);
    assert(old->mutation == old->argValues.mutation);
    assert(old->ownedMutation.has_value());
    assert(old->mutation->attr("proccode") == "f %s");
    assert(!old->ops.empty());
    BlockCached* const oldText = blocks.getCachedExecute("text");
    assert(oldText && oldText->fields["TEXT"].value.isString());
    assert(oldText->fields["TEXT"].value.asStrPtr()->view() ==
           u"保持する文字列𠮷🧪");

    blocks.resetCache();
    BlockCached* const fresh = blocks.getCachedExecute("call");
    assert(fresh && fresh != old);
    assert(!blocks.cache().retiredExecuteCached.empty());
    // Repeated edits must never reclaim any still-embedded pointer or
    // discard its string roots, even after several fresh code generations.
    for (int generation = 0; generation < 8; ++generation) {
        blocks.resetCache();
        BlockCached* next = blocks.getCachedExecute("call");
        assert(next && next != old && next != fresh);
        Gc::get().collect();
        assert(old->argValues.mutation->attr("proccode") == "f %s");
        assert(oldText->fields["TEXT"].value.asStrPtr()->view() ==
               u"保持する文字列𠮷🧪");
    }
    assert(blocks.cache().retiredExecuteCached.size() >= 9);
    // The original mutation is now gone; cached callers must own a copy.
    blocks.deleteBlock("call");
    assert(blocks.getBlock("call") == nullptr);
    assert(blocks.getBlock("text") == nullptr);
    Gc::get().collect();
    assert(old->mutation->attr("proccode") == "f %s");
    assert(old->argValues.mutation == old->mutation);
    assert(oldText->fields["TEXT"].value.asStrPtr()->view() ==
           u"保持する文字列𠮷🧪");
    assert(old->ops.size() >= 1);
    for (const BlockCached* op : old->ops) {
        assert(op);
        assert(op->id == "call" || op->id == "text");
    }
    return 0;
}
