// LLVM-only experimental conversion of a direct-yield script into a real
// switched-resume coroutine.  No C++ helper that can suspend is permitted.
// This file is intentionally separate from the generic Scratch code emitter.
#pragma once
#include "llvm_compat.hpp"
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>

#include <cstddef>
#include <cmath>
#include "../core/cast.hpp"
#include <string>
#include <unordered_set>
#include <vector>

#include "../model/blocks.hpp"
#include "jit.hpp"
#include "mega_yield_source.hpp"

namespace xyo::stackless {

inline bool enabled() {
    static const bool active = [] {
        const char* value = std::getenv("XYO_JIT_CORO_DIRECT");
        return jitFiberlessEnabled() && value && value[0] == '1';
    }();
    return active;
}

// Strict source proof for the first actual LLVM coroutine integration:
// top hat -> one ordinary repeat -> one numeric literal change-variable.
// No reporters, arbitrary helper calls, nested procedures, waits or GC-bearing
// live values may cross the compiler-inserted yield.
inline bool eligible(const Blocks& blocks, const Block& top) {
    if (!enabled()) return false;
    if (top.opcode != "event_whenflagclicked" &&
        top.opcode != "event_whenbroadcastreceived" &&
        top.opcode != "control_start_as_clone")
        return false;
    const Block* loop = blocks.getBlock(top.next);
    if (!loop || loop->opcode != "control_repeat" || !loop->next.empty())
        return false;
    const auto constantNumber = [&](const BlockInput* input) {
        if (!input || input->block.empty() ||
            input->block != input->shadow) return false;
        const Block* literal = blocks.getBlock(input->block);
        if (!literal || !literal->shadow || !literal->next.empty())
            return false;
        const auto& opcode = literal->opcode;
        if (opcode != "math_number" && opcode != "math_integer" &&
            opcode != "math_whole_number" &&
            opcode != "math_positive_number") return false;
        const auto field = literal->fields.find("NUM");
        return field != literal->fields.end() &&
               std::isfinite(cast::toNumber(field->second.value));
    };
    const BlockInput* count = loop->inputs.find("TIMES");
    const BlockInput* sub = loop->inputs.find("SUBSTACK");
    if (!constantNumber(count) || !sub || sub->block.empty())
        return false;
    const auto numberBlock = blocks.getBlock(count->block);
    const auto numberField = numberBlock->fields.find("NUM");
    const double repetitions = cast::toNumber(numberField->second.value);
    // The first direct-state-machine experiment supports only exact
    // nonnegative counts within the signed 32-bit repeat range.
    if (repetitions < 0.0 || repetitions > 2147483647.0 ||
        std::floor(repetitions) != repetitions) return false;
    const Block* change = blocks.getBlock(sub->block);
    if (!change || change->opcode != "data_changevariableby" ||
        !change->next.empty()) return false;
    const BlockInput* delta = change->inputs.find("VALUE");
    const auto field = change->fields.find("VARIABLE");
    return constantNumber(delta) &&
           field != change->fields.end() && !field->second.id.empty();
}

// Stage 2: general direct-yield continuations. Any script whose only
// suspension points are direct loop yields may become a coroutine. Custom
// blocks are rejected for now: a warp procedure would turn its loop
// checkpoints into unconditional suspensions.
inline bool generalEnabled() {
    static const bool active = [] {
        const char* value = std::getenv("XYO_JIT_CORO_GENERAL");
        return value && value[0] == '1';
    }();
    return active;
}

// Walk one script chain (next/inputs/substacks) and collect reachable
// procedures_call proccodes.
inline bool collectProcedureCalls(const Blocks& blocks, const Block* start,
                                  std::vector<std::string>& calls) {
    std::unordered_set<std::string> seen;
    std::vector<const Block*> stack{start};
    size_t visited = 0;
    while (!stack.empty()) {
        const Block* block = stack.back();
        stack.pop_back();
        if (!block || !seen.insert(block->id).second) continue;
        if (++visited > 20000) return false;
        if (block->opcode == "event_broadcastandwait") {
            static const bool megaYield = [] {
                const char* flag = std::getenv("XYO_JIT_MEGA_BATCH_YIELD");
                return flag && flag[0] == '1';
            }();
            if (megaYield) return false;
        }
        if (block->opcode == "procedures_call") {
            if (!block->mutation) return false;
            const std::string code = block->mutation->attr("proccode");
            if (code.empty()) return false;
            calls.push_back(code);
        }
        if (!block->next.empty())
            if (const Block* next = blocks.getBlock(block->next))
                stack.push_back(next);
        for (const auto& [name, input] : block->inputs.items) {
            (void)name;
            if (!input.block.empty())
                if (const Block* next = blocks.getBlock(input.block))
                    stack.push_back(next);
            if (!input.shadow.empty() && input.shadow != input.block)
                if (const Block* next = blocks.getBlock(input.shadow))
                    stack.push_back(next);
        }
    }
    return true;
}

// True when any transitively reachable procedure is a warp definition.
inline bool reachableWarpProcedure(const Blocks& blocks,
                                   std::vector<std::string> calls) {
    std::unordered_set<std::string> visited;
    while (!calls.empty()) {
        const std::string code = std::move(calls.back());
        calls.pop_back();
        if (!visited.insert(code).second) continue;
        if (visited.size() > 256) return true;
        const std::string defId =
            const_cast<Blocks&>(blocks).getProcedureDefinition(code);
        if (defId.empty()) continue;
        const Block* def = blocks.getBlock(defId);
        if (!def) return true;
        const BlockInput* custom = def->inputs.find("custom_block");
        const Block* proto = custom ? blocks.getBlock(custom->block) : nullptr;
        if (proto && proto->mutation && proto->mutation->warp()) return true;
        std::vector<std::string> nested;
        if (!collectProcedureCalls(blocks, def, nested)) return true;
        calls.insert(calls.end(), nested.begin(), nested.end());
    }
    return false;
}

inline bool generalCandidate(const Blocks& blocks, const Block& top) {
    if (!generalEnabled()) return false;
    // Source-certified Mega batches take precedence: their switch(pc)
    // continuations already specialize these shapes, so leave the normal
    // compiled entry on the Fiber path for them.
    static const bool yieldBatch = [] {
        const char* flag = std::getenv("XYO_JIT_MEGA_BATCH_YIELD");
        return flag && flag[0] == '1';
    }();
    if (yieldBatch) {
        mega_yield::Source proof;
        if (mega_yield::prove(const_cast<Blocks&>(blocks), top, proof))
            return false;
    }
    std::vector<std::string> calls;
    if (!collectProcedureCalls(blocks, &top, calls)) {
        if (const char* tr = std::getenv("XYO_JIT_CORO_TRACE");
            tr && tr[0] == '1')
            std::fprintf(stderr, "[coro-cand] %s: collect failed\n",
                         top.id.c_str());
        return false;
    }
    // Warp procedures are supported: codegen only suspends their loop
    // checkpoints when the watchdog/GC budget is due (xyo_warp_yield_due),
    // so they keep their run-without-screen-refresh semantics.
    return true;
}

// Helpers that suspend the Fiber from inside C++. A coroutine frame cannot
// yield through them, so such scripts stay on the Fiber path until each
// helper is rewritten as an explicit poll loop.
inline bool internalYieldHelper(std::string_view name) {
    static const std::unordered_set<std::string_view> k = {
        "xyo_wait_yield", "xyo_wait_event_yield", "xyo_loop_yield",
        "xyo_empty_repeat_warp", "xyo_stop_all", "xyo_stop_script",
        "xyo_broadcast_wait_v", "xyo_broadcast_wait_lit",
        "xyo_rt_compat", "xyo_rt_async_reporter", "xyo_rt_reporter",
        "xyo_rt_nested_osr_arg_get", "xyo_mot_glidexy",
        "xyo_mot_glideto", "xyo_looks_saysecs", "xyo_looks_thinksecs",
        "xyo_looks_switchbackdropwait", "xyo_sns_askwait",
        "xyo_ctl_deleteclone", "xyo_snd_playwait", "xyo_snd_seteffect",
        "xyo_snd_changeeffect", "xyo_snd_setvolume",
        "xyo_snd_changevolume",
    };
    return k.count(name) != 0;
}

inline bool isCoopYield(llvm::CallInst* call) {
    return call->getMetadata("xyo.coop") != nullptr;
}

// Must run BEFORE LLVM's default coroutine-aware O pipeline. A generated
// ScriptFn still has the existing void(JitCtx*) ABI. It saves its opaque
// coroutine handle into JitCtx, which outlives every suspension.
inline bool lower(llvm::Module& mod, const std::string& entryName) {
    using namespace llvm;
    Function* entry = mod.getFunction(entryName);
    if (!entry || entry->arg_size() != 1 ||
        !entry->getReturnType()->isVoidTy()) return false;

    std::vector<CallInst*> yields;
    std::vector<ReturnInst*> returns;
    for (BasicBlock& bb : *entry) {
        for (Instruction& ins : bb) {
            if (auto* ret = dyn_cast<ReturnInst>(&ins))
                returns.push_back(ret);
            auto* call = dyn_cast<CallInst>(&ins);
            if (!call) continue;
            Function* callee = call->getCalledFunction();
            if (!callee) return false;
            const StringRef name = callee->getName();
            if (name == "xyo_yield_fast" || name == "xyo_yield") {
                yields.push_back(call);
            } else if (internalYieldHelper(name)) {
                // `xyo_proc_*` bookkeeping is allowed here: callers only
                // lower a fused (fully inlined), non-warp graph, so no
                // native procedure frame is live across a suspension.
                if (const char* tr = std::getenv("XYO_JIT_CORO_TRACE");
                    tr && tr[0] == '1')
                    std::fprintf(stderr,
                                 "[coro-reject] %s: internal yield %s\n",
                                 entryName.c_str(), name.str().c_str());
                return false;
            }
        }
    }
    if (yields.empty() || returns.empty()) {
        if (const char* tr = std::getenv("XYO_JIT_CORO_TRACE");
            tr && tr[0] == '1')
            std::fprintf(stderr,
                         "[coro-reject] %s: yields=%zu returns=%zu\n",
                         entryName.c_str(), yields.size(), returns.size());
        return false;
    }

    LLVMContext& lc = mod.getContext();
    Type* ptr = PointerType::get(lc, 0);
    Type* i8 = Type::getInt8Ty(lc);
    Type* i64 = Type::getInt64Ty(lc);
    Type* voidt = Type::getVoidTy(lc);
    llvm::Value* none = ConstantTokenNone::get(lc);
    auto* idFn = llvmc::intrinsicDeclaration(&mod, Intrinsic::coro_id);
    auto* beginFn = llvmc::intrinsicDeclaration(&mod, Intrinsic::coro_begin);
    auto* sizeFn = llvmc::intrinsicDeclaration(&mod, Intrinsic::coro_size, {i64});
    auto* suspendFn = llvmc::intrinsicDeclaration(&mod, Intrinsic::coro_suspend);
    auto* freeFn = llvmc::intrinsicDeclaration(&mod, Intrinsic::coro_free);
    auto* endFn = llvmc::intrinsicDeclaration(&mod, Intrinsic::coro_end);
    auto* resumeFn = llvmc::intrinsicDeclaration(&mod, Intrinsic::coro_resume);
    auto* doneFn = llvmc::intrinsicDeclaration(&mod, Intrinsic::coro_done);
    auto* destroyFn = llvmc::intrinsicDeclaration(&mod, Intrinsic::coro_destroy);
    FunctionCallee alloc = mod.getOrInsertFunction(
        "malloc", FunctionType::get(ptr, {i64}, false));
    FunctionCallee dealloc = mod.getOrInsertFunction(
        "free", FunctionType::get(voidt, {ptr}, false));

    entry->addFnAttr(llvm::Attribute::PresplitCoroutine);
    IRBuilder<> start(&*entry->getEntryBlock().getFirstInsertionPt());
    llvm::Value* id = start.CreateCall(
        idFn, {start.getInt32(0), ConstantPointerNull::get(llvm::cast<PointerType>(ptr)),
               ConstantPointerNull::get(llvm::cast<PointerType>(ptr)),
               ConstantPointerNull::get(llvm::cast<PointerType>(ptr))});
    llvm::Value* size = start.CreateCall(sizeFn);
    llvm::Value* storage = start.CreateCall(alloc, {size});
    llvm::Value* handle = start.CreateCall(beginFn, {id, storage});
    llvm::Value* context = entry->getArg(0);
    llvm::Value* slot = start.CreateConstGEP1_32(i8, context,
        static_cast<unsigned>(offsetof(JitCtx, coroHandle)));
    start.CreateStore(handle, slot);
    // Publish the heap frame so the scheduler can register it as a
    // conservative GC root while this coroutine is suspended.
    start.CreateStore(storage, start.CreateConstGEP1_32(i8, context,
        static_cast<unsigned>(offsetof(JitCtx, coroFrame))));
    start.CreateStore(size, start.CreateConstGEP1_32(i8, context,
        static_cast<unsigned>(offsetof(JitCtx, coroFrameSize))));

    BasicBlock* suspended = BasicBlock::Create(lc, "coro.suspended", entry);
    BasicBlock* cleanup = BasicBlock::Create(lc, "coro.cleanup", entry);
    BasicBlock* completed = BasicBlock::Create(lc, "coro.completed", entry);

    for (CallInst* call : yields) {
        BasicBlock* original = call->getParent();
        BasicBlock* continuation = original->splitBasicBlock(
            std::next(call->getIterator()), "coro.resume");
        original->getTerminator()->eraseFromParent();
        IRBuilder<> at(call);
        llvm::Value* action = at.CreateCall(
            suspendFn, {none, at.getFalse()});
        auto* sw = at.CreateSwitch(action, suspended, 2);
        sw->addCase(at.getInt8(0), continuation);
        sw->addCase(at.getInt8(1), cleanup);
        call->eraseFromParent();
    }

    for (ReturnInst* ret : returns) {
        IRBuilder<> at(ret);
        at.CreateBr(completed);
        ret->eraseFromParent();
    }

    IRBuilder<> finish(completed);
    llvm::Value* finalAction = finish.CreateCall(
        suspendFn, {none, finish.getTrue()});
    auto* onFinal = finish.CreateSwitch(finalAction, suspended, 1);
    onFinal->addCase(finish.getInt8(1), cleanup);

    IRBuilder<> tearDown(cleanup);
    llvm::Value* released = tearDown.CreateCall(freeFn, {id, handle});
    tearDown.CreateCall(dealloc, {released});
    tearDown.CreateBr(suspended);

    IRBuilder<> done(suspended);
    done.CreateCall(endFn, {handle, done.getFalse(), none});
    done.CreateRetVoid();

    const auto createWrapper = [&](const char* suffix, Type* result,
                                   Function* intrinsic) {
        Function* wrapper = Function::Create(
            FunctionType::get(result, {ptr}, false),
            GlobalValue::ExternalLinkage, entryName + suffix, &mod);
        BasicBlock* bb = BasicBlock::Create(lc, "entry", wrapper);
        IRBuilder<> b(bb);
        auto* v = b.CreateCall(intrinsic, {wrapper->getArg(0)});
        if (result->isVoidTy()) b.CreateRetVoid();
        else b.CreateRet(v);
    };
    createWrapper("_resume", voidt, resumeFn);
    createWrapper("_done", Type::getInt1Ty(lc), doneFn);
    createWrapper("_destroy", voidt, destroyFn);
    return !verifyFunction(*entry, &errs());
}


// ---------------------------------------------------------------------
// Recursive procedure coroutines ("yield from").
//
// A non-warp recursive procedure must yield at every recursive call.  The
// fused lowering cannot express that (the procedure graph is a cycle), so
// each procedure becomes its own coroutine and every call site resumes the
// callee and propagates its suspension to the caller:
//
//     callee(ctx, args...)          // starts, may suspend immediately
//     h = xyo_coro_top_frame(ctx)
//     while (!coro.done(h)) {
//         coro.resume(h);
//         if (!coro.done(h)) coro.suspend(this);   // yield from
//     }
//     coro.destroy(h); xyo_coro_pop_frame(ctx, h);
//
// Frames are heap allocations registered in the JitCtx list so the GC can
// scan every suspended frame.  The caller owns destruction (the callee
// lowering does not free its own frame).
inline bool recursionEnabled() {
    static const bool active = [] {
        const char* value = std::getenv("XYO_JIT_CORO_RECURSION");
        return !(value && value[0] == '0');
    }();
    return active;
}

namespace detail {

inline bool directYieldSite(llvm::Function* f) {
    using namespace llvm;
    for (BasicBlock& bb : *f) {
        for (Instruction& ins : bb) {
            auto* call = dyn_cast<CallInst>(&ins);
            if (!call) continue;
            Function* callee = call->getCalledFunction();
            if (!callee) continue;
            StringRef name = callee->getName();
            if (name == "xyo_yield" || name == "xyo_yield_fast")
                return true;
            if (internalYieldHelper(name)) return true;
        }
    }
    return false;
}

inline bool lowerOne(llvm::Module& mod, llvm::Function* fn, bool isEntry,
                     const std::unordered_set<llvm::Function*>& coroutines,
                     bool skipCoopYields = false) {
    using namespace llvm;
    LLVMContext& lc = mod.getContext();
    Type* ptr = PointerType::get(lc, 0);
    Type* i8 = Type::getInt8Ty(lc);
    Type* i64 = Type::getInt64Ty(lc);
    Type* voidt = Type::getVoidTy(lc);
    llvm::Value* none = ConstantTokenNone::get(lc);
    auto* idFn = llvmc::intrinsicDeclaration(&mod, Intrinsic::coro_id);
    auto* beginFn = llvmc::intrinsicDeclaration(&mod, Intrinsic::coro_begin);
    auto* sizeFn = llvmc::intrinsicDeclaration(&mod, Intrinsic::coro_size, {i64});
    auto* suspendFn = llvmc::intrinsicDeclaration(&mod, Intrinsic::coro_suspend);
    auto* freeFn = llvmc::intrinsicDeclaration(&mod, Intrinsic::coro_free);
    auto* endFn = llvmc::intrinsicDeclaration(&mod, Intrinsic::coro_end);
    auto* resumeFn = llvmc::intrinsicDeclaration(&mod, Intrinsic::coro_resume);
    auto* doneFn = llvmc::intrinsicDeclaration(&mod, Intrinsic::coro_done);
    auto* destroyFn = llvmc::intrinsicDeclaration(&mod, Intrinsic::coro_destroy);
    FunctionCallee alloc = mod.getOrInsertFunction(
        "malloc", FunctionType::get(ptr, {i64}, false));
    FunctionCallee dealloc = mod.getOrInsertFunction(
        "free", FunctionType::get(voidt, {ptr}, false));
    FunctionCallee pushFrame = mod.getOrInsertFunction(
        "xyo_coro_push_frame",
        FunctionType::get(voidt, {ptr, ptr, i64}, false));
    FunctionCallee topFrame = mod.getOrInsertFunction(
        "xyo_coro_top_frame", FunctionType::get(ptr, {ptr}, false));
    FunctionCallee popFrame = mod.getOrInsertFunction(
        "xyo_coro_pop_frame", FunctionType::get(voidt, {ptr, ptr}, false));

    fn->addFnAttr(Attribute::PresplitCoroutine);
    IRBuilder<> start(&*fn->getEntryBlock().getFirstInsertionPt());
    llvm::Value* id = start.CreateCall(
        idFn, {start.getInt32(0), ConstantPointerNull::get(llvm::cast<PointerType>(ptr)),
               ConstantPointerNull::get(llvm::cast<PointerType>(ptr)),
               ConstantPointerNull::get(llvm::cast<PointerType>(ptr))});
    llvm::Value* size = start.CreateCall(sizeFn);
    llvm::Value* storage = start.CreateCall(alloc, {size});
    llvm::Value* handle = start.CreateCall(beginFn, {id, storage});
    llvm::Value* context = fn->getArg(0);
    if (isEntry) {
        start.CreateStore(handle, start.CreateConstGEP1_32(i8, context,
            static_cast<unsigned>(offsetof(JitCtx, coroHandle))));
        start.CreateStore(storage, start.CreateConstGEP1_32(i8, context,
            static_cast<unsigned>(offsetof(JitCtx, coroFrame))));
        start.CreateStore(size, start.CreateConstGEP1_32(i8, context,
            static_cast<unsigned>(offsetof(JitCtx, coroFrameSize))));
    } else {
        start.CreateCall(pushFrame, {context, handle, size});
    }

    BasicBlock* suspended = BasicBlock::Create(lc, "coro.suspended", fn);
    BasicBlock* cleanup = BasicBlock::Create(lc, "coro.cleanup", fn);
    BasicBlock* completed = BasicBlock::Create(lc, "coro.completed", fn);

    // Rewrite calls to sibling coroutines into resume loops.  Done before
    // yield replacement; the loop's own suspend is created explicitly.
    std::vector<CallInst*> calls;
    for (BasicBlock& bb : *fn)
        for (Instruction& ins : bb)
            if (auto* call = dyn_cast<CallInst>(&ins)) {
                Function* callee = call->getCalledFunction();
                if (callee && coroutines.count(callee))
                    calls.push_back(call);
            }
    for (CallInst* call : calls) {
        if (!call->getParent()) continue;
        BasicBlock* bb = call->getParent();
        BasicBlock* cont = bb->splitBasicBlock(
            std::next(call->getIterator()), "coro.call.cont");
        llvm::BasicBlock* getHandle =
            BasicBlock::Create(lc, "coro.call.handle", fn);
        llvm::BasicBlock* check = BasicBlock::Create(lc, "coro.call.check", fn);
        llvm::BasicBlock* resume = BasicBlock::Create(lc, "coro.call.resume", fn);
        llvm::BasicBlock* yieldFrom =
            BasicBlock::Create(lc, "coro.call.yield", fn);
        llvm::BasicBlock* after = BasicBlock::Create(lc, "coro.call.after", fn);
        bb->getTerminator()->eraseFromParent();
        IRBuilder<> tail(bb);
        tail.CreateBr(getHandle);
        // The handle must be read AFTER the call: the callee pushes its
        // frame before its first suspension (or completion).
        IRBuilder<> g(getHandle);
        llvm::Value* h = g.CreateCall(topFrame, {context});
        g.CreateBr(check);
        IRBuilder<> c(check);
        llvm::Value* d = c.CreateCall(doneFn, {h});
        c.CreateCondBr(d, after, resume);
        IRBuilder<> r(resume);
        r.CreateCall(resumeFn, {h});
        llvm::Value* d2 = r.CreateCall(doneFn, {h});
        r.CreateCondBr(d2, after, yieldFrom);
        IRBuilder<> y(yieldFrom);
        llvm::Value* action = y.CreateCall(suspendFn, {none, y.getFalse()});
        auto* sw = y.CreateSwitch(action, suspended, 2);
        sw->addCase(y.getInt8(0), check);
        sw->addCase(y.getInt8(1), cleanup);
        IRBuilder<> a(after);
        a.CreateCall(destroyFn, {h});
        a.CreateCall(popFrame, {context, h});
        a.CreateBr(cont);
    }

    std::vector<CallInst*> yields;
    std::vector<ReturnInst*> returns;
    for (BasicBlock& bb : *fn) {
        for (Instruction& ins : bb) {
            if (auto* ret = dyn_cast<ReturnInst>(&ins))
                returns.push_back(ret);
            auto* call = dyn_cast<CallInst>(&ins);
            if (!call) continue;
            Function* callee = call->getCalledFunction();
            if (!callee) continue;
            StringRef name = callee->getName();
            if (name == "xyo_yield_fast" || name == "xyo_yield") {
                if (!skipCoopYields || !isCoopYield(call))
                    yields.push_back(call);
            } else if (internalYieldHelper(name)) {
                if (const char* tr = std::getenv("XYO_JIT_CORO_TRACE");
                    tr && tr[0] == '1')
                    std::fprintf(stderr,
                                 "[coro-rec-reject] %s: internal yield %s\n",
                                 fn->getName().str().c_str(),
                                 name.str().c_str());
                return false;
            }
        }
    }
    if (yields.empty() || returns.empty()) {
        if (const char* tr = std::getenv("XYO_JIT_CORO_TRACE");
            tr && tr[0] == '1')
            std::fprintf(stderr,
                         "[coro-rec-reject] %s: yields=%zu returns=%zu\n",
                         fn->getName().str().c_str(), yields.size(),
                         returns.size());
        return false;
    }

    for (CallInst* call : yields) {
        BasicBlock* original = call->getParent();
        BasicBlock* continuation = original->splitBasicBlock(
            std::next(call->getIterator()), "coro.resume");
        original->getTerminator()->eraseFromParent();
        IRBuilder<> at(call);
        llvm::Value* action = at.CreateCall(suspendFn, {none, at.getFalse()});
        auto* sw = at.CreateSwitch(action, suspended, 2);
        sw->addCase(at.getInt8(0), continuation);
        sw->addCase(at.getInt8(1), cleanup);
        call->eraseFromParent();
    }

    for (ReturnInst* ret : returns) {
        IRBuilder<> at(ret);
        at.CreateBr(completed);
        ret->eraseFromParent();
    }

    IRBuilder<> finish(completed);
    llvm::Value* finalAction = finish.CreateCall(
        suspendFn, {none, finish.getTrue()});
    auto* onFinal = finish.CreateSwitch(finalAction, suspended, 1);
    onFinal->addCase(finish.getInt8(1), cleanup);

    IRBuilder<> tearDown(cleanup);
    if (isEntry) {
        llvm::Value* released = tearDown.CreateCall(freeFn, {id, handle});
        tearDown.CreateCall(dealloc, {released});
    }
    tearDown.CreateBr(suspended);

    IRBuilder<> done(suspended);
    done.CreateCall(endFn, {handle, done.getFalse(), none});
    done.CreateRetVoid();

    if (isEntry) {
        const auto createWrapper = [&](const char* suffix, Type* result,
                                       Function* intrinsic) {
            Function* wrapper = Function::Create(
                FunctionType::get(result, {ptr}, false),
                GlobalValue::ExternalLinkage,
                fn->getName().str() + suffix, &mod);
            BasicBlock* bb = BasicBlock::Create(lc, "entry", wrapper);
            IRBuilder<> b(bb);
            auto* v = b.CreateCall(intrinsic, {wrapper->getArg(0)});
            if (result->isVoidTy()) b.CreateRetVoid();
            else b.CreateRet(v);
        };
        createWrapper("_resume", voidt, resumeFn);
        createWrapper("_done", Type::getInt1Ty(lc), doneFn);
        createWrapper("_destroy", voidt, destroyFn);
    }
    std::string err;
    return !verifyFunction(*fn, &errs());
}

} // namespace detail

/// Lower a script whose procedures can suspend through recursive calls.
/// `procNames` are the generated procedure symbols for this script.
inline bool lowerRecursive(llvm::Module& mod, const std::string& entryName,
                           const std::vector<std::string>& procNames) {
    using namespace llvm;
    if (!recursionEnabled()) return false;
    Function* entry = mod.getFunction(entryName);
    if (!entry || entry->arg_size() != 1 ||
        !entry->getReturnType()->isVoidTy()) return false;
    std::vector<Function*> candidates{entry};
    for (const std::string& name : procNames) {
        Function* f = mod.getFunction(name);
        if (!f || f->isDeclaration()) continue;
        if (f->arg_size() < 1 || !f->getReturnType()->isVoidTy())
            return false;
        candidates.push_back(f);
    }
    // Any other function that can yield would be a helper we cannot lower;
    // reject to stay on the Fiber path rather than suspend through it.
    std::unordered_set<Function*> considered(candidates.begin(),
                                             candidates.end());
    for (Function& f : mod) {
        if (f.isDeclaration() || considered.count(&f)) continue;
        if (detail::directYieldSite(&f)) {
            if (const char* tr = std::getenv("XYO_JIT_CORO_TRACE");
                tr && tr[0] == '1')
                std::fprintf(stderr, "[coro-rec] foreign yield in %s\n",
                             f.getName().str().c_str());
            return false;
        }
    }
    std::unordered_set<Function*> suspendable;
    for (Function* f : candidates)
        if (detail::directYieldSite(f)) suspendable.insert(f);
    bool changed = true;
    while (changed) {
        changed = false;
        for (Function* f : candidates) {
            if (suspendable.count(f)) continue;
            bool callsSuspendable = false;
            for (BasicBlock& bb : *f) {
                for (Instruction& ins : bb) {
                    auto* call = dyn_cast<CallInst>(&ins);
                    if (!call) continue;
                    Function* callee = call->getCalledFunction();
                    if (callee && suspendable.count(callee)) {
                        callsSuspendable = true;
                        break;
                    }
                }
                if (callsSuspendable) break;
            }
            if (callsSuspendable) {
                suspendable.insert(f);
                changed = true;
            }
        }
    }
    if (!suspendable.count(entry)) {
        if (const char* tr = std::getenv("XYO_JIT_CORO_TRACE");
            tr && tr[0] == '1') {
            std::fprintf(stderr, "[coro-rec] candidates:");
            for (Function* f : candidates)
                std::fprintf(stderr, " %s%s", f->getName().str().c_str(),
                             suspendable.count(f) ? "(Y)" : "");
            std::fprintf(stderr, " entry not suspendable\n");
        }
        return false;
    }
    // Validate every function before mutating the module: a half-lowered
    // graph (some functions already coroutine-ized, then a rejection) would
    // be compiled as invalid IR.
    auto canLower = [](Function* f) {
        bool hasYield = false, hasReturn = false;
        for (BasicBlock& bb : *f) {
            for (Instruction& ins : bb) {
                if (isa<ReturnInst>(ins)) hasReturn = true;
                auto* call = dyn_cast<CallInst>(&ins);
                if (!call) continue;
                Function* callee = call->getCalledFunction();
                if (!callee) return false;
                StringRef name = callee->getName();
                if ((name == "xyo_yield" || name == "xyo_yield_fast") &&
                    !isCoopYield(call))
                    hasYield = true;
                else if (internalYieldHelper(name))
                    return false;
            }
        }
        return hasYield && hasReturn;
    };
    for (Function* f : suspendable) {
        if (!canLower(f)) {
            if (const char* tr = std::getenv("XYO_JIT_CORO_TRACE");
                tr && tr[0] == '1')
                std::fprintf(stderr, "[coro-rec] not lowerable: %s\n",
                             f->getName().str().c_str());
            return false;
        }
    }
    for (Function* f : suspendable) f->addFnAttr(Attribute::PresplitCoroutine);
    for (Function* f : suspendable) {
        if (!detail::lowerOne(mod, f, f == entry, suspendable, true)) {
            if (const char* tr = std::getenv("XYO_JIT_CORO_TRACE");
                tr && tr[0] == '1')
                std::fprintf(stderr, "[coro-rec] lower failed for %s\n",
                             f->getName().str().c_str());
            return false;
        }
    }
    return true;
}

} // namespace xyo::stackless
