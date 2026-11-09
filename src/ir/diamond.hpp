// A backend-neutral lowering contract for a structured conditional diamond.
//
// LLVM materializes basic blocks and a Phi at merge. WebAssembly emits a
// structured if/else/end, and writes the chosen value to a local when needed.
// The graph is kept deliberately separate from Scratch's effectful lowering.
#pragma once

#include "verifier.hpp"
#include <utility>

namespace xyo::ir {

// IDs are logical block identities, not LLVM BasicBlock pointers or WASM labels.
struct Diamond {
    ControlFlow cfg;
    BlockId entry;
    BlockId yes;
    BlockId no;
    BlockId merge;

    explicit Diamond(bool withPhi = false, Type phiType = Type::I32)
        : entry(cfg.createBlock()),
          yes(cfg.createBlock()),
          no(cfg.createBlock()),
          merge(cfg.createBlock()) {
        cfg.block(entry).terminator = {
            TerminatorKind::Branch, yes, no, 0, 0};
        cfg.block(yes).terminator = {TerminatorKind::Jump, merge};
        cfg.block(no).terminator = {TerminatorKind::Jump, merge};
        cfg.block(merge).terminator = {TerminatorKind::Return};
        if (withPhi) cfg.block(merge).phis.push_back(
            {phiType, {{yes, 0}, {no, 1}}});
    }

    bool validate() const { return static_cast<bool>(verifyControlFlow(cfg)); }
};

// Callback contract (values can be llvm::Value*, a local ID, etc.):
//   branch(cond, yesId, noId, mergeId)
//   otherwise(noId)                  [switch to else path]
//   finish(mergeId)                  [join the paths]
//   phi(mergeId, yesId, yesValue, noId, noValue)
// The backend is responsible for recording actual predecessor blocks for Phi
// entries (a branch emitter can split blocks while lowering an expression).
template <typename Backend, typename Cond, typename TrueFn, typename FalseFn>
auto lowerDiamondValue(const Diamond& diamond, Backend& backend,
                       Cond condition, TrueFn&& onTrue, FalseFn&& onFalse) {
    assert(diamond.validate() && diamond.cfg.block(diamond.merge).phis.size() == 1);
    backend.branch(condition, diamond.yes, diamond.no, diamond.merge);
    auto yesValue = std::forward<TrueFn>(onTrue)();
    backend.otherwise(diamond.no);
    auto noValue = std::forward<FalseFn>(onFalse)();
    backend.finish(diamond.merge);
    return backend.phi(diamond.merge, diamond.yes, yesValue,
                       diamond.no, noValue);
}

// Void variant used for effectful statement bodies. Branch callbacks must
// preserve Scratch statement ordering and ensure any bailout is transactional.
template <typename Backend, typename Cond, typename TrueFn, typename FalseFn>
void lowerDiamondVoid(const Diamond& diamond, Backend& backend,
                      Cond condition, TrueFn&& onTrue, FalseFn&& onFalse) {
    assert(diamond.validate() && diamond.cfg.block(diamond.merge).phis.empty());
    backend.branch(condition, diamond.yes, diamond.no, diamond.merge);
    std::forward<TrueFn>(onTrue)();
    backend.otherwise(diamond.no);
    std::forward<FalseFn>(onFalse)();
    backend.finish(diamond.merge);
}

} // namespace xyo::ir
