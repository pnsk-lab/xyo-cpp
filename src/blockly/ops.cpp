// xyo-cpp: ブロックグラフの基本操作の実装。
#include "ops.hpp"

#include "layout.hpp"
#include "model/blocks.hpp"

namespace xyo {
namespace blockly {

void insertAfter(Blocks* blocks, const std::string& head, const std::string& tail,
                 const std::string& anchor) {
    if (!blocks) return;
    const Block* found = blocks->getBlock(anchor);
    if (!found) return;
    Blocks::StructuralEdit edit(*blocks);
    Block* a = edit.getBlock(anchor);
    std::string oldNext = a->next;
    a->next = head;
    if (Block* h = edit.getBlock(head)) h->parent = anchor;
    if (Block* tl = edit.getBlock(tail)) tl->next = oldNext;
    if (!oldNext.empty())
        if (Block* on = edit.getBlock(oldNext)) on->parent = tail;
}

void insertBefore(Blocks* blocks, const std::string& head, const std::string& tail,
                  const std::string& anchor) {
    if (!blocks) return;
    const Block* found = blocks->getBlock(anchor);
    if (!found) return;
    Blocks::StructuralEdit edit(*blocks);
    Block* a = edit.getBlock(anchor);
    std::string prev = prevOf(blocks, anchor);
    bool aTop = a->topLevel;
    if (!prev.empty()) {
        if (Block* p = edit.getBlock(prev)) p->next = head;
        if (Block* h = edit.getBlock(head)) h->parent = prev;
    } else {
        // The first statement inside a C-block has no .next predecessor.
        // Rewire its parent's input instead of creating a second script root.
        const std::string parentId = a->parent;
        bool inSubstack = false;
        if (Block* parent = edit.getBlock(parentId)) {
            for (auto& [name, input] : parent->inputs.items) {
                if ((name == "SUBSTACK" || name == "SUBSTACK2") &&
                    input.block == anchor) {
                    input.block = head;
                    inSubstack = true;
                    break;
                }
            }
        }
        if (inSubstack) {
            blocks->deleteScript(head);
            if (Block* h = edit.getBlock(head)) {
                h->topLevel = false;
                h->parent = parentId;
            }
        } else {
            if (aTop) {
                blocks->deleteScript(anchor);
                a->topLevel = false;
            }
            if (Block* h = edit.getBlock(head)) {
                h->topLevel = true;
                h->parent.clear();
                blocks->addScript(head);
            }
        }
    }
    if (Block* tl = edit.getBlock(tail)) tl->next = anchor;
    if (Block* a2 = edit.getBlock(anchor)) a2->parent = tail;
}

void attachToValue(Blocks* blocks, const std::string& head, const std::string& target,
                   const std::string& inputName) {
    if (!blocks) return;
    const Block* tgt = blocks->getBlock(target);
    if (!tgt) return;
    const BlockInput* in = tgt->inputs.find(inputName);
    if (!in) return;
    Blocks::StructuralEdit edit(*blocks);
    Block* mutableTarget = edit.getBlock(target);
    BlockInput* mutableInput = mutableTarget->inputs.find(inputName);
    if (!mutableInput) return;
    mutableInput->block = head;
    if (Block* h = edit.getBlock(head)) h->parent = target;
}

void attachToSubstack(Blocks* blocks, const std::string& head, const std::string& tail,
                      const std::string& target, int branch) {
    if (!blocks) return;
    const Block* tgt = blocks->getBlock(target);
    if (!tgt) return;
    const char* nm = branch == 0 ? "SUBSTACK" : "SUBSTACK2";
    const BlockInput* in = tgt->inputs.find(nm);
    if (!in) return;
    Blocks::StructuralEdit edit(*blocks);
    Block* mutableTarget = edit.getBlock(target);
    BlockInput* mutableInput = mutableTarget->inputs.find(nm);
    if (!mutableInput) return;
    std::string oldHead = mutableInput->block;
    mutableInput->block = head;
    if (Block* h = edit.getBlock(head)) h->parent = target;
    if (Block* tl = edit.getBlock(tail)) tl->next = oldHead;
    if (!oldHead.empty())
        if (Block* oh = edit.getBlock(oldHead)) oh->parent = tail;
}

void makeTopLevel(Blocks* blocks, const std::string& head) {
    if (!blocks) return;
    const Block* found = blocks->getBlock(head);
    if (!found) return;
    Blocks::StructuralEdit edit(*blocks);
    Block* h = edit.getBlock(head);
    h->topLevel = true;
    h->parent.clear();
    blocks->addScript(head);
}

} // namespace blockly
} // namespace xyo
