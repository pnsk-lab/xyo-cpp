// xyo-cpp: ブロックグラフの基本操作の実装。
#include "ops.hpp"

#include "layout.hpp"
#include "model/blocks.hpp"

namespace xyo {
namespace blockly {

void insertAfter(Blocks* blocks, const std::string& head, const std::string& tail,
                 const std::string& anchor) {
    Block* a = blocks->getBlock(anchor);
    if (!a) return;
    std::string oldNext = a->next;
    a->next = head;
    if (Block* h = blocks->getBlock(head)) h->parent = anchor;
    if (Block* tl = blocks->getBlock(tail)) tl->next = oldNext;
    if (!oldNext.empty())
        if (Block* on = blocks->getBlock(oldNext)) on->parent = tail;
}

void insertBefore(Blocks* blocks, const std::string& head, const std::string& tail,
                  const std::string& anchor) {
    Block* a = blocks->getBlock(anchor);
    if (!a) return;
    std::string prev = prevOf(blocks, anchor);
    bool aTop = a->topLevel;
    if (!prev.empty()) {
        if (Block* p = blocks->getBlock(prev)) p->next = head;
        if (Block* h = blocks->getBlock(head)) h->parent = prev;
    } else {
        // The first statement inside a C-block has no .next predecessor.
        // Rewire its parent's input instead of creating a second script root.
        const std::string parentId = a->parent;
        bool inSubstack = false;
        if (Block* parent = blocks->getBlock(parentId)) {
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
            if (Block* h = blocks->getBlock(head)) {
                h->topLevel = false;
                h->parent = parentId;
            }
        } else {
            if (aTop) {
                blocks->deleteScript(anchor);
                a->topLevel = false;
            }
            if (Block* h = blocks->getBlock(head)) {
                h->topLevel = true;
                h->parent.clear();
                blocks->addScript(head);
            }
        }
    }
    if (Block* tl = blocks->getBlock(tail)) tl->next = anchor;
    if (Block* a2 = blocks->getBlock(anchor)) a2->parent = tail;
}

void attachToValue(Blocks* blocks, const std::string& head, const std::string& target,
                   const std::string& inputName) {
    Block* tgt = blocks->getBlock(target);
    if (!tgt) return;
    BlockInput* in = tgt->inputs.find(inputName);
    if (!in) return;
    in->block = head;
    if (Block* h = blocks->getBlock(head)) h->parent = target;
}

void attachToSubstack(Blocks* blocks, const std::string& head, const std::string& tail,
                      const std::string& target, int branch) {
    Block* tgt = blocks->getBlock(target);
    if (!tgt) return;
    const char* nm = branch == 0 ? "SUBSTACK" : "SUBSTACK2";
    BlockInput* in = tgt->inputs.find(nm);
    if (!in) return;
    std::string oldHead = in->block;
    in->block = head;
    if (Block* h = blocks->getBlock(head)) h->parent = target;
    if (Block* tl = blocks->getBlock(tail)) tl->next = oldHead;
    if (!oldHead.empty())
        if (Block* oh = blocks->getBlock(oldHead)) oh->parent = tail;
}

void makeTopLevel(Blocks* blocks, const std::string& head) {
    Block* h = blocks->getBlock(head);
    if (!h) return;
    h->topLevel = true;
    h->parent.clear();
    blocks->addScript(head);
}

} // namespace blockly
} // namespace xyo
