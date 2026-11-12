// START-017: nested structural source edit transaction / AOT lazy guard.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "engine/runtime.hpp"
#include "jit/aot_lazy_cache.hpp"
#include "model/blocks.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>
#include <unordered_map>

using xyo::Blocks;
using xyo::Block;
// Reads through Blocks::allBlocks() cannot mutate any Block payload.
static_assert(std::is_same_v<
    decltype(std::declval<Blocks&>().allBlocks()),
    const std::unordered_map<std::string, Block>&>);
static_assert(std::is_same_v<
    decltype(std::declval<Blocks::StructuralEdit&>().allBlocks()),
    std::unordered_map<std::string, Block>&>);
// START-019: writable single-block pointers come from a versioned scope.
static_assert(std::is_same_v<
    decltype(std::declval<Blocks::StructuralEdit&>().getBlock(
        std::declval<const std::string&>())),
    Block*>);
static_assert(std::is_same_v<
    decltype(std::declval<const Blocks&>().getBlock(
        std::declval<const std::string&>())),
    const Block*>);

int main() {
    using namespace xyo;
    Runtime rt;
    Blocks blocks(&rt);

    Block hat;
    hat.id = "root";
    hat.opcode = "event_whenflagclicked";
    hat.topLevel = true;
    blocks.createBlock(std::move(hat));

    const BlockCached* oldCache = blocks.getCachedExecute("root");
    assert(oldCache && oldCache->nextBlockId.empty());
    AotLazyCachedSlot oldDescriptor(&blocks, "root", blocks.revision());
    assert(xyo_rt_aot_cached(&oldDescriptor) == oldCache);
    const uint64_t before = blocks.revision();

    // A bulk paste builds an interconnected chain, including nested
    // createBlock() operations. No intermediate cache invalidation or AOT
    // revision publication is permitted before the outer transaction ends.
    {
        Blocks::StructuralEdit edit(blocks);
        assert(blocks.revision() == before);
        {
            Blocks::StructuralEdit nested(blocks);
            std::string previous = "root";
            for (int i = 0; i < 64; ++i) {
                Block child;
                child.id = "child-" + std::to_string(i);
                child.opcode = "looks_say";
                child.parent = previous;
                child.fields["MESSAGE"] = BlockField{Value::num(i)};
                blocks.createBlock(std::move(child));
                Block* parent = nested.getBlock(previous);
                assert(parent);
                parent->next = "child-" + std::to_string(i);
                previous = "child-" + std::to_string(i);
                assert(blocks.revision() == before);
            }
            assert(blocks.setBlockField(
                "child-0", "MESSAGE", BlockField{Value::num(777)}));
            assert(blocks.revision() == before);
        }
        assert(blocks.revision() == before);
        assert(blocks.getBlock("root")->next == "child-0");
    }
    assert(blocks.revision() == before + 1);
    const BlockCached* updatedCache = blocks.getCachedExecute("root");
    assert(updatedCache && updatedCache != oldCache);
    assert(updatedCache->nextBlockId == "child-0");
    assert(oldCache->nextBlockId.empty());
    assert(blocks.getBlock("child-0")->fields.at("MESSAGE").value ==
           Value::num(777));
    assert(xyo_rt_aot_cached(&oldDescriptor) == nullptr);

    // Recursive deleteBlock() internally calls resetCache once per child.
    // The transaction collapses these invalidations to one source commit
    // while still removing the entire 64-node chain.
    const uint64_t beforeDelete = blocks.revision();
    {
        Blocks::StructuralEdit deleteEdit(blocks);
        deleteEdit.getBlock("root")->next.clear();
        blocks.deleteBlock("child-0");
        assert(blocks.revision() == beforeDelete);
        assert(blocks.getBlock("child-63") == nullptr);
    }
    assert(blocks.revision() == beforeDelete + 1);
    assert(blocks.getBlock("root")->next.empty());
    assert(blocks.getBlock("child-0") == nullptr);
    assert(xyo_rt_aot_cached(&oldDescriptor) == nullptr);

    // Mutable block iteration is only obtained from a transaction.
    const uint64_t beforeListEdit = blocks.revision();
    {
        Blocks::StructuralEdit edit(blocks);
        for (auto& [id, block] : edit.allBlocks()) {
            if (id == "root") block.opcode = "event_whenkeypressed";
        }
        assert(blocks.revision() == beforeListEdit);
    }
    assert(blocks.revision() == beforeListEdit + 1);
    assert(blocks.getBlock("root")->opcode == "event_whenkeypressed");

    // Pure workspace layout changes do not change Scratch semantics and
    // must not abort a live AOT Fiber just to move a script on the screen.
    const uint64_t beforePosition = blocks.revision();
    const BlockCached* beforePositionCache =
        blocks.getCachedExecute("root");
    assert(blocks.setBlockPosition("root", 123, 456));
    assert(!blocks.setBlockPosition("missing", 2, 3));
    assert(blocks.revision() == beforePosition);
    assert(blocks.getCachedExecute("root") == beforePositionCache);
    assert(blocks.getBlock("root")->x == 123);
    assert(blocks.getBlock("root")->y == 456);

    // A direct in-place opcode mutation is now safe WHEN wrapped in a
    // structural transaction. The public raw Block* API outside a scope
    // remains an independent, documented source of silent edits.
    const uint64_t beforeOpcode = blocks.revision();
    {
        Blocks::StructuralEdit edit(blocks);
        edit.getBlock("root")->opcode = "event_whenbroadcastreceived";
    }
    assert(blocks.revision() == beforeOpcode + 1);
    const BlockCached* finalCache = blocks.getCachedExecute("root");
    assert(finalCache && finalCache->opcode == "event_whenbroadcastreceived");
    assert(updatedCache->opcode == "event_whenflagclicked");

    std::puts("PASS START-017 nested commit, 64-block paste/delete, "
              "immutable cache and stale AOT slot");
    return 0;
}
