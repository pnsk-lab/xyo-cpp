// Backend-neutral single-latch loop with a backedge SSA Phi.
//
// Semantics:
//   preheader -> header
//   header: phi(initial from preheader, updated from header)
//           body; if (continueCondition) goto header; else goto exit
//
// A caller must prove at least one iteration is legal (e.g. count > 0).
// LLVM emits actual loop-header PHIs; WebAssembly implements those PHIs
// with locals that are updated before br_if. Runtime guards, yields and
// deoptimization are the responsibility of the calling Scratch lowerer.
#pragma once

#include "verifier.hpp"
#include <cassert>
#include <utility>

namespace xyo::ir {

struct BackedgeLoop {
    ControlFlow cfg;
    BlockId preheader = cfg.createBlock();
    BlockId header = cfg.createBlock();
    BlockId exit = cfg.createBlock();

    explicit BackedgeLoop(Type inductionType = Type::I32,
                          uint32_t carriedValues = 1) {
        cfg.block(preheader).terminator =
            {TerminatorKind::Jump, header};
        cfg.block(header).terminator =
            {TerminatorKind::Branch, header, exit};
        cfg.block(exit).terminator =
            {TerminatorKind::Return};
        // These value IDs are the logical Phi operands, not LLVM Values.
        // The backend supplies the live initial and next SSA Values.
        for (uint32_t i = 0; i < carriedValues; ++i)
            cfg.block(header).phis.push_back(
                {inductionType, {{preheader, i * 2}, {header, i * 2 + 1}}});
    }

    bool validate() const { return static_cast<bool>(verifyControlFlow(cfg)); }
};

// Backend contract:
//   begin(header)           // branch to loop header; establish loop Phi
//   body() -> condition     // emit body, update Phi backedge; returns i1
//   latch(condition,header,exit) // backedge conditional branch
//   finish(exit)            // set insertion point after the loop
//
// Both the condition and the Phi backedge must use values from the same
// latch block; not all Scratch operations are safe inside this region.
template <typename Backend, typename Body>
void lowerBackedgeLoop(const BackedgeLoop& loop, Backend& backend,
                       Body&& body) {
    assert(loop.validate());
    backend.begin(loop.header);
    auto condition = std::forward<Body>(body)();
    backend.latch(condition, loop.header, loop.exit);
    backend.finish(loop.exit);
}

} // namespace xyo::ir
