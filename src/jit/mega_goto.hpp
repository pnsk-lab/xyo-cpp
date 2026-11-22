// Beta: fuse an acyclic Scratch procedure call graph into one LLVM function.
//
// LLVM InlineFunction rewrites each procedure call into basic-block branches
// (the native equivalent of static goto labels). This keeps the Scratch
// procedure frame helpers, yield sites, and source-operation order intact.
//
// Scope: ONE compiled top-level Scratch script and all of its *reachable*
// non-recursive procedures. It does not merge different simultaneous
// Scratch threads into one global dispatch function. A true whole-project
// program-counter scheduler requires a separate continuation ABI.
#pragma once

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xyo::mega_goto {

struct Result {
    bool fused = false;
    size_t procedureCalls = 0;
    size_t estimatedInstructions = 0;
    size_t remainingCalls = 0;
    const char* reason = "disabled";
};

inline bool enabled() {
    static const bool setting = [] {
        const char* value = std::getenv("XYO_JIT_MEGA_DISPATCH");
        return value && value[0] == '1';
    }();
    return setting;
}

inline size_t maxInstructions() {
    static const size_t limit = [] {
        const char* value = std::getenv("XYO_JIT_MEGA_MAX_IR");
        if (!value || !*value) return size_t{512};
        char* end = nullptr;
        unsigned long long parsed = std::strtoull(value, &end, 10);
        if (end == value || *end || parsed < 100 || parsed > 100000)
            return size_t{512};
        return static_cast<size_t>(parsed);
    }();
    return limit;
}

// Called on the JIT-owning compiler thread, before LLVM's O pipeline.
// No shared engine state, no runtime modification, no codegen callbacks.
inline Result fuse(llvm::Module& module,
                   const std::string& entryName,
                   std::map<std::string, std::string>& procedureNames,
                   bool force = false, size_t budgetOverride = 0) {
    Result result;
    if (!force && !enabled()) return result;
    result.reason = "not-applicable";
    llvm::Function* entry = module.getFunction(entryName);
    if (!entry || entry->isDeclaration() || procedureNames.empty())
        return result;

    std::unordered_set<llvm::Function*> procedures;
    procedures.reserve(procedureNames.size());
    for (const auto& [key, name] : procedureNames) {
        (void)key;
        llvm::Function* fn = module.getFunction(name);
        if (!fn || fn->isDeclaration() || fn->isVarArg()) {
            result.reason = "missing-procedure";
            return result;
        }
        procedures.insert(fn);
    }

    // Reject recursion, native callbacks to the procedure entry points, and
    // an estimated expansion beyond the code-size budget BEFORE modifying IR.
    // Counts each reachable callsite, not merely each distinct procedure.
    const size_t budget = budgetOverride ? budgetOverride
                                       : maxInstructions();
    std::unordered_set<llvm::Function*> active;
    std::unordered_map<llvm::Function*, std::optional<size_t>> memo;
    bool recursive = false;
    std::function<std::optional<size_t>(llvm::Function*)> count =
        [&](llvm::Function* fn) -> std::optional<size_t> {
            if (active.contains(fn)) {
                recursive = true;
                return std::nullopt;
            }
            if (auto it = memo.find(fn); it != memo.end()) return it->second;
            active.insert(fn);
            size_t total = 0;
            for (auto& block : *fn) {
                for (auto& instruction : block) {
                    if (++total > budget) {
                        active.erase(fn);
                        return memo[fn] = std::nullopt;
                    }
                    auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (!call) continue;
                    auto* child = call->getCalledFunction();
                    if (!child || !procedures.contains(child)) continue;
                    auto* direct = llvm::dyn_cast<llvm::CallInst>(call);
                    if (!direct || direct->isMustTailCall()) {
                        active.erase(fn);
                        return memo[fn] = std::nullopt;
                    }
                    auto sub = count(child);
                    if (!sub || *sub > budget - total) {
                        active.erase(fn);
                        return memo[fn] = std::nullopt;
                    }
                    total += *sub;
                }
            }
            active.erase(fn);
            return memo[fn] = total;
        };

    const auto estimated = count(entry);
    if (!estimated) {
        result.reason = recursive ? "recursive" : "size-or-callsite";
        return result;
    }
    result.estimatedInstructions = *estimated;

    // A procedure must be referenced only by native LLVM callsites; no
    // function-address escaping into a callback/global/ORC host symbol.
    for (llvm::Function* fn : procedures) {
        for (llvm::User* use : fn->users()) {
            auto* call = llvm::dyn_cast<llvm::CallBase>(use);
            if (!call || call->getCalledFunction() != fn) {
                result.reason = "address-escape";
                return result;
            }
        }
    }

    // Re-scan only the *entry* function: new inlined bodies may contain
    // nested calls. Calling InlineFunction replaces the call with actual
    // LLVM BasicBlock branches / PHIs; there is no per-Scratch-procedure
    // machine call and no second native frame on the fused path.
    llvm::InlineFunctionInfo inlineInfo;
    for (size_t iteration = 0; iteration < budget; ++iteration) {
        llvm::CallInst* target = nullptr;
        for (auto& block : *entry) {
            for (auto& instruction : block) {
                auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
                if (call && procedures.contains(call->getCalledFunction())) {
                    target = call;
                    break;
                }
            }
            if (target) break;
        }
        if (!target) break;
        if (!llvm::InlineFunction(*target, inlineInfo).isSuccess()) {
            result.reason = "inline-failed";
            return result;
        }
        ++result.procedureCalls;
    }

    for (auto& block : *entry)
        for (auto& instruction : block)
            if (auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                call && procedures.contains(call->getCalledFunction()))
                ++result.remainingCalls;

    if (result.remainingCalls || result.procedureCalls == 0) {
        result.reason = result.remainingCalls
            ? "incomplete" : "no-reachable-procedure-calls";
        return result;
    }

    // These symbols are compiler-private now. The normal O pipeline
    // eliminates unused definitions; the entry contains all actual effects.
    // The runtime must not try to look up erased ORC procedure symbols.
    for (llvm::Function* fn : procedures)
        fn->setLinkage(llvm::GlobalValue::InternalLinkage);
    procedureNames.clear();

    if (llvm::verifyFunction(*entry, &llvm::errs())) {
        result.reason = "invalid-llvm";
        return result;
    }
    result.reason = "fused";
    result.fused = true;
    return result;
}

} // namespace xyo::mega_goto
