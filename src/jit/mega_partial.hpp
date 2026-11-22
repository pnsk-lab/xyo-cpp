// Optional bounded, local LLVM inlining for large Scratch script graphs.
//
// Unlike the all-or-nothing mega_goto::fuse, this pass keeps all original
// Scratch procedure symbols. Only selected call sites inside small procedure
// bodies are replaced with CFG edges; recursive cycles and oversized bodies
// are left alone. The caller's normal Coroutine/Fiber ABI is unchanged.
#pragma once

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <algorithm>
#include <cstdio>
#include <string_view>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xyo::mega_partial {

struct Stats {
    size_t selected = 0;
    size_t inlined = 0;
    size_t rejectedCycles = 0;
    size_t rejectedSize = 0;
    bool valid = true;
};

inline bool enabled() {
    const char* e = std::getenv("XYO_JIT_MEGA_PARTIAL");
    return e && e[0] == '1';
}

inline bool isSchedulerProcedure(const std::string& name) {
    if (name.rfind("sched_", 0) != 0) return false;
    const char* raw = std::getenv("XYO_JIT_MEGA_PARTIAL_SCOPE");
    if (!raw || !*raw || std::string_view(raw) == "all") return true;
    // Exact comma-delimited procedure names only. Accept either full
    // Scratch proccode or the suffix without the "sched_" prefix.
    const std::string_view candidates(raw);
    const std::string_view shortName(name.data() + 6, name.size() - 6);
    for (size_t start = 0; start < candidates.size();) {
        const size_t end = candidates.find(',', start);
        const size_t length = end == std::string_view::npos
            ? candidates.size() - start : end - start;
        const auto token = candidates.substr(start, length);
        if (token == name || token == shortName) return true;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return false;
}

inline size_t instructionCount(const llvm::Function& function) {
    size_t count = 0;
    for (const llvm::BasicBlock& bb : function)
        count += bb.size();
    return count;
}

inline bool reaches(llvm::Function* from, llvm::Function* target,
                    const std::unordered_set<llvm::Function*>& procedures) {
    std::vector<llvm::Function*> pending{from};
    std::unordered_set<llvm::Function*> visited;
    while (!pending.empty()) {
        llvm::Function* current = pending.back();
        pending.pop_back();
        if (current == target) return true;
        if (!visited.insert(current).second) continue;
        for (llvm::BasicBlock& bb : *current) {
            for (llvm::Instruction& inst : bb) {
                auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                if (!call) continue;
                llvm::Function* next = call->getCalledFunction();
                if (next && procedures.contains(next))
                    pending.push_back(next);
            }
        }
    }
    return false;
}

inline Stats fuse(llvm::Module& module,
                  const std::map<std::string, std::string>& names) {
    Stats stats;
    if (!enabled() || names.empty()) return stats;

    std::unordered_set<llvm::Function*> procedures;
    procedures.reserve(names.size());
    for (const auto& [code, symbol] : names) {
        (void)code;
        auto* function = module.getFunction(symbol);
        if (function && !function->isDeclaration())
            procedures.insert(function);
    }

    // A single selected caller may grow by at most 1,200 approximate IR
    // instructions and take at most eight inlinings. The normal LLVM
    // optimizer retains control of instruction scheduling afterward.
    constexpr size_t kPerCallerGrowth = 1200;
    constexpr size_t kPerCalleeLimit = 384;
    constexpr size_t kMaxCallsites = 8;

    for (const auto& [code, symbol] : names) {
        if (!isSchedulerProcedure(code)) continue;
        llvm::Function* caller = module.getFunction(symbol);
        if (!caller || caller->isDeclaration()) continue;
        ++stats.selected;
        size_t remainingBudget = kPerCallerGrowth;
        for (size_t iter = 0; iter < kMaxCallsites; ++iter) {
            llvm::CallInst* chosen = nullptr;
            size_t chosenSize = 0;
            // Prefer the smallest safe body; avoid duplicating expensive
            // reporters or recursive frame management in hot loop cases.
            for (llvm::BasicBlock& bb : *caller) {
                for (llvm::Instruction& inst : bb) {
                    auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                    if (!call || call->isMustTailCall()) continue;
                    llvm::Function* callee = call->getCalledFunction();
                    if (!callee || callee == caller ||
                        !procedures.contains(callee)) continue;
                    const size_t size = instructionCount(*callee);
                    if (size > remainingBudget || size > kPerCalleeLimit) {
                        ++stats.rejectedSize;
                        continue;
                    }
                    if (reaches(callee, caller, procedures)) {
                        ++stats.rejectedCycles;
                        continue;
                    }
                    if (!chosen || size < chosenSize) {
                        chosen = call;
                        chosenSize = size;
                    }
                }
            }
            if (!chosen) break;
            const std::string calleeSymbol =
                chosen->getCalledFunction()->getName().str();
            llvm::InlineFunctionInfo info;
            if (!llvm::InlineFunction(*chosen, info).isSuccess()) break;
            if (const char* trace = std::getenv("XYO_JIT_MEGA_PARTIAL_SITES");
                trace && trace[0] == '1')
                std::fprintf(stderr,
                    "[mega-partial-site] caller=%s callee=%s ir=%zu budget=%zu\n",
                    code.c_str(), calleeSymbol.c_str(),
                    chosenSize, remainingBudget);
            ++stats.inlined;
            remainingBudget -= chosenSize;
        }
        // A failed verifier is fatal: unlike budget rejection, a partially
        // modified module cannot safely be reused as if unmodified.
        if (llvm::verifyFunction(*caller, &llvm::errs())) {
            stats.valid = false;
            return stats;
        }
    }
    return stats;
}

} // namespace xyo::mega_partial
