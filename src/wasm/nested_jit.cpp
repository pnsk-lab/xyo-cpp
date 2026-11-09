#include "nested_jit.hpp"
#include "../ir/execution_abi.hpp"

#include "binaryen_backend.hpp"
#include "binaryen_c_api.hpp"
#include "host_bridge.hpp"
#include "hot_jit.hpp"
#include "wasm_compile.hpp"
#include "../engine/thread.hpp"
#include "../ir/scratch_pipeline.hpp"
#include "../model/blocks.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <unordered_set>
#include <utility>

namespace xyo::wasm {

NestedJitCache::~NestedJitCache() {
    if (std::getenv("XYO_NESTED_WASM_STATS"))
        std::fprintf(stderr,
            "[nested-wasm] attempted=%llu compiled=%llu executed=%llu fast=%llu bailed=%llu warp_compiled=%llu warp_runs=%llu warp_bails=%llu tree_compiled=%llu tree_runs=%llu tree_bails=%llu tail_compiled=%llu tail_runs=%llu tail_bails=%llu return_compiled=%llu return_runs=%llu return_bails=%llu pc_compiled=%llu pc_runs=%llu pc_bails=%llu\n",
            static_cast<unsigned long long>(attempted_),
            static_cast<unsigned long long>(compiled_),
            static_cast<unsigned long long>(executed_),
            static_cast<unsigned long long>(fastExecuted_),
            static_cast<unsigned long long>(bailed_),
            static_cast<unsigned long long>(warpCompiled_),
            static_cast<unsigned long long>(warpExecuted_),
            static_cast<unsigned long long>(warpBailed_),
            static_cast<unsigned long long>(treeCompiled_),
            static_cast<unsigned long long>(treeExecuted_),
            static_cast<unsigned long long>(treeBailed_),
            static_cast<unsigned long long>(tailCompiled_),
            static_cast<unsigned long long>(tailExecuted_),
            static_cast<unsigned long long>(tailBailed_),
            static_cast<unsigned long long>(returnCompiled_),
            static_cast<unsigned long long>(returnExecuted_),
            static_cast<unsigned long long>(returnBailed_),
            static_cast<unsigned long long>(pcCompiled_),
            static_cast<unsigned long long>(pcExecuted_),
            static_cast<unsigned long long>(pcBailed_));
}

#if defined(__EMSCRIPTEN__) && defined(XYO_WASM_HOTSPOT)
namespace {

bool eligibleStatement(const Block* block, bool allowWarpLoop = false) {
    if (!block) return false;
    const auto& op = block->opcode;
    if (allowWarpLoop &&
        (op == "control_repeat" ||
         op == "control_repeat_until" || op == "control_while" ||
         op == "control_for_each"))
        return true;
    // Never cross a Scratch scheduler boundary. In particular, procedure
    // calls can cause an entry-time warp yield even without an explicit
    // wait opcode; those remain under the original interpreter for now.
    return op == "data_setvariableto" ||
           op == "data_changevariableby" ||
           op == "data_addtolist" ||
           op == "data_replaceitemoflist" ||
           op == "data_insertatlist" ||
           op == "data_deleteoflist" ||
           op == "data_deletealloflist" ||
           op == "control_if" ||
           op == "control_if_else";
}

// Shared whitelist for the allocation-free numeric slot ABI. Keep it
// identical for nonwarp straight-line and bounded warp control trees.
bool numericExpressionsOnly(const Program& p) {
    for (const auto& e : p.exprs) {
        switch (e.k) {
        case EK::ConstNum:
        case EK::VarGet:
        case EK::ProcedureArgument:
        case EK::Add:
        case EK::Sub:
        case EK::Mul:
        case EK::Div:
        case EK::Mod:
        case EK::Round:
        case EK::MathOp:
        case EK::Neg:
        case EK::LessThan:
        case EK::GreaterThan:
        case EK::Equals:
        case EK::And:
        case EK::Or:
        case EK::Not:
            break;
        default:
            return false;
        }
    }
    return true;
}

// Numeric-only straight-line/conditional fragments do not need the
// general transactional list/string journal. A nonwarp interpreter owns
// every surrounding control frame and the original per-tick yield.
bool numericScalarProgram(const Program& p) {
    if (!p.listIds.empty() || !p.stringLiterals.empty() ||
        p.stmts.size() > 256 || p.exprs.size() > 2048)
        return false;
    for (const auto& stmt : p.stmts) {
        switch (stmt.k) {
        case SK::SetVar:
        case SK::ChangeVar:
            if (!stmt.body.empty() || !stmt.elseBody.empty())
                return false;
            break;
        case SK::If:
            if (!stmt.elseBody.empty()) return false;
            break;
        case SK::IfElse:
            break;
        default:
            // Never fuse repeat/while/for-each in nonwarp mode: all loop
            // continuation and inter-thread scheduling stays interpreted.
            return false;
        }
        if (stmt.expr >= p.exprs.size()) return false;
    }
    return numericExpressionsOnly(p);
}

// Verify the *entire* captured CFG. Inline synchronous WebAssembly can
// replace a nested warp control tree only when all transitive paths are
// scalar-numeric and bounded. A shared 1024-iteration fuel is charged on
// EVERY backedge, including inner loops; overflow discards the transaction.
bool pureNumericWarpLoop(const Program& p) {
    if (!p.listIds.empty() || !p.stringLiterals.empty() ||
        p.main.size() != 1 || p.main.front() >= p.stmts.size() ||
        p.stmts.size() > 256 || p.exprs.size() > 2048)
        return false;

    const auto root = p.stmts[p.main.front()].k;
    if (root != SK::Repeat && root != SK::Until &&
        root != SK::While && root != SK::ForEach)
        return false;

    if (!numericExpressionsOnly(p)) return false;

    // This first implementation owns exactly one memory-resident
    // return-frame stack per compiled region. Multiple independent
    // recursive helpers could otherwise alias the same frame area
    // while one helper is still active. Refuse the region instead.
    if (std::count_if(
            p.stmts.begin(), p.stmts.end(),
            [](const Stmt& stmt) {
                return stmt.k == SK::WarpReturnPcStack;
            }) > 1)
        return false;

    std::vector<uint8_t> visit(p.stmts.size(), 0);
    const auto walk = [&](const auto& self, uint32_t id,
                          uint32_t depth) -> bool {
        if (depth > 8 || id >= p.stmts.size() || visit[id] != 0)
            return false; // malformed cyclic/aliased CFG
        visit[id] = 1;
        const Stmt& stmt = p.stmts[id];
        if (stmt.expr >= p.exprs.size()) return false;
        switch (stmt.k) {
        case SK::SetVar:
        case SK::ChangeVar:
            if (!stmt.body.empty() || !stmt.elseBody.empty()) return false;
            break;
        case SK::Repeat:
        case SK::Until:
        case SK::While:
        case SK::ForEach:
        case SK::If:
        case SK::IfElse:
            break;
        case SK::WarpReturnPcStack:
            // Explicit LIFO contains the return PC and 0–2 numeric
            // arguments, wholly inside the interpreter's private buffer.
            if (!stmt.inlineWarp || !stmt.body.empty() ||
                stmt.elseBody.empty() || stmt.returnSites.empty() ||
                stmt.returnSites.size() > 2 ||
                stmt.argumentSlots.size() > 2)
                return false;
            for (const auto& site : stmt.returnSites) {
                if (site.before.empty() ||
                    site.condition >= p.exprs.size() ||
                    site.nextArguments.size() != stmt.argumentSlots.size())
                    return false;
                for (uint32_t expr : site.nextArguments) {
                    if (expr >= p.exprs.size()) return false;
                    switch (p.exprs[expr].k) {
                    case EK::ConstNum: case EK::ProcedureArgument:
                    case EK::Add: case EK::Sub: case EK::Mul:
                    case EK::Div: case EK::Mod: case EK::Round:
                    case EK::MathOp: case EK::Neg:
                        break;
                    default:
                        return false;
                    }
                }
            }
            break;
        case SK::WarpReturnStack:
            // A single numeric prefix / conditional self-call / suffix
            // has one return address at every depth, so an explicit
            // bounded continuation depth is sufficient.
            if (!stmt.inlineWarp || stmt.body.empty() ||
                stmt.elseBody.empty() || !stmt.argumentExprs.empty() ||
                !stmt.argumentSlots.empty())
                return false;
            break;
        case SK::WarpTailRecurse:
            // Canonical warp tail recursion with up to four known
            // numeric parameters becomes one budgeted do/while.
            // Non-tail, dynamic-type or nonwarp recursion is rejected.
            if (!stmt.inlineWarp || stmt.body.empty() ||
                stmt.argumentExprs.size() != stmt.argumentSlots.size() ||
                stmt.argumentExprs.size() > 4)
                return false;
            // Recursive argument values must also remain numeric:
            // boolean, raw variable or tagged string results could
            // violate the strong invariant established by the literal
            // numeric entry values. Arithmetic expressions always
            // produce a numeric result under Scratch coercion.
            for (uint32_t expr : stmt.argumentExprs) {
                if (expr >= p.exprs.size()) return false;
                switch (p.exprs[expr].k) {
                case EK::ConstNum:
                case EK::ProcedureArgument:
                case EK::Add:
                case EK::Sub:
                case EK::Mul:
                case EK::Div:
                case EK::Mod:
                case EK::Round:
                case EK::MathOp:
                case EK::Neg:
                    break;
                default:
                    return false;
                }
            }
            break;
        case SK::InlineProcedure:
            // Only verified warp procedure calls with numeric-literal
            // initial arguments. The frontend rejects unsafe call cycles;
            // canonical bounded tail recursion is encoded separately.
            // Entry-time warp deadlines are checked before dispatch and
            // again before the speculative transaction is committed.
            if (!stmt.inlineWarp || stmt.body.empty() ||
                stmt.argumentExprs.size() != stmt.argumentSlots.size() ||
                stmt.argumentExprs.size() > 4)
                return false;
            for (uint32_t expr : stmt.argumentExprs) {
                // The first invocation must capture an actual number,
                // not a string, boolean or dynamically typed variable.
                if (expr >= p.exprs.size() ||
                    p.exprs[expr].k != EK::ConstNum)
                    return false;
            }
            break;
        default:
            // No nonwarp calls, lists, strings, reporter effects or
            // arbitrary scheduling operations inside a fused warp tree.
            return false;
        }
        if (stmt.k != SK::IfElse && stmt.k != SK::WarpReturnStack &&
            stmt.k != SK::WarpReturnPcStack &&
            !stmt.elseBody.empty()) return false;
        if (stmt.k == SK::IfElse && stmt.body.empty() &&
            stmt.elseBody.empty()) return false;
        if (stmt.k == SK::ForEach && stmt.var >= p.slotIds.size())
            return false;
        for (const auto& site : stmt.returnSites)
            for (const uint32_t child : site.before)
                if (!self(self, child, depth + 1)) return false;
        for (const uint32_t child : stmt.body)
            if (!self(self, child, depth + 1)) return false;
        for (const uint32_t child : stmt.elseBody)
            if (!self(self, child, depth + 1)) return false;
        visit[id] = 2;
        return true;
    };
    if (!walk(walk, p.main.front(), 0)) return false;
    // Reject any orphan lowered statement, which may indicate an effect
    // silently omitted by a future frontend change.
    return std::all_of(visit.begin(), visit.end(),
                       [](uint8_t v) { return v == 2; });
}

bool atomicProgram(const Program& program) {
    // The shared frontend preserves unknown/opaque ops. A valid range must
    // not contain nested loops or inlined procedure calls: either could
    // introduce an interpreter yield/checkpoint not represented in WASM.
    std::vector<uint32_t> pending = program.main;
    std::unordered_set<uint32_t> visited;
    while (!pending.empty()) {
        const uint32_t i = pending.back();
        pending.pop_back();
        if (i >= program.stmts.size()) return false;
        if (!visited.insert(i).second) continue;
        const auto& statement = program.stmts[i];
        switch (statement.k) {
        case SK::SetVar:
        case SK::SetVarRaw:
        case SK::SetVarValue:
        case SK::SetVarBoolean:
        case SK::ChangeVar:
        case SK::ListAppend:
        case SK::ListReplace:
        case SK::ListInsert:
        case SK::ListDelete:
        case SK::ListDeleteAll:
        case SK::If:
        case SK::IfElse:
            break;
        default:
            return false;
        }
        pending.insert(pending.end(), statement.body.begin(),
                       statement.body.end());
        pending.insert(pending.end(), statement.elseBody.begin(),
                       statement.elseBody.end());
    }
    return true;
}

} // namespace
#endif

bool NestedJitCache::execute(Thread& thread, const Block* first,
                             std::string& pastEndId,
                             bool* permanentlyUnsupported) {
#if !defined(__EMSCRIPTEN__) || !defined(XYO_WASM_HOTSPOT)
    (void)thread;
    (void)first;
    (void)pastEndId;
    if (permanentlyUnsupported) *permanentlyUnsupported = true;
    return false;
#else
    if (permanentlyUnsupported) *permanentlyUnsupported = false;
    const StackFrame* currentFrame = thread.peekStackFrame();
    const bool warpRoot = first &&
        (first->opcode == "control_repeat" ||
         first->opcode == "control_repeat_until" ||
         first->opcode == "control_while" ||
         first->opcode == "control_for_each") &&
        currentFrame && currentFrame->warpMode;
    if (!first || !thread.blockContainer ||
        !thread.target || !eligibleStatement(first, warpRoot))
        return false;
    // Construction is already guarded by XYO_NESTED_WASM=1; do not
    // scan the process environment on every hot Scratch body statement.
    // Scratch scripts may be replaced by editor/extension mutations.
    // Do not execute a stale module after Blocks::resetCache(), nor
    // confuse a recycled Block allocation with the old region.
    auto it = entries_.find(first);
    const uint64_t revision = thread.blockContainer->revision();
    if (it != entries_.end() &&
        (it->second.owner != thread.blockContainer ||
         it->second.sourceRevision != revision)) {
        it = entries_.erase(it);
    }
    if (it == entries_.end()) {
        auto [created, inserted] = entries_.try_emplace(first);
        (void)inserted;
        it = created;
        it->second.owner = thread.blockContainer;
        it->second.sourceRevision = revision;
    }
    auto& entry = it->second;
    // The engine starts with the interpreter. Only compile sites that have
    // proven hot through repeated interpreter entries; otherwise hundreds
    // of cold WebAssembly.Module/Instance pairs inflate RSS and startup.
    static constexpr uint32_t kWarmup = 64;
    static constexpr uint64_t kMaxModules = 48;
    static const bool allowMixed = [] {
        const char* env = std::getenv("XYO_NESTED_WASM_ALL");
        return env && env[0] == '1';
    }();
    if (!entry.attempted && ++entry.visits >= kWarmup) {
        entry.attempted = true;
        ++attempted_;
        if (compiled_ < kMaxModules) {
            const auto unit = ir::ScratchCompilationUnit::capture(
                *thread.blockContainer, first);
            if (unit.valid()) {
                Program program;
                size_t count = 0;
                if (warpRoot) {
                    // The original interpreter owns the surrounding
                    // procedure frame. Only a bounded numeric *warp*
                    // repeat can run to completion atomically; a nonwarp
                    // repeat must keep yielding once per iteration.
                    const bool lowered = lowerCapturedStatement(
                        unit.script, unit.effects, first->id, program);
                    const bool safe = lowered && pureNumericWarpLoop(program);
                    if (std::getenv("XYO_NESTED_WASM_DEBUG"))
                        std::fprintf(stderr,
                            "[nested-wasm-debug] op=%s lowered=%d safe=%d stmts=%zu exprs=%zu main=%zu\n",
                            first->opcode.c_str(), int(lowered), int(safe),
                            program.stmts.size(), program.exprs.size(),
                            program.main.size());
                    if (safe) {
                        count = 1;
                        entry.pastEndId = first->next;
                    }
                } else {
                    std::string cursor = first->id;
                    // Bounded transactional range. Existing Thread
                    // StackFrames remain authoritative across each
                    // Scratch loop iteration.
                    for (size_t j = 0; j < 12; ++j) {
                        const Block* statement = unit.script.getBlock(cursor);
                        if (!eligibleStatement(statement)) break;
                        const std::string next = statement->next;
                        Program extended;
                        if (!lowerCapturedRange(unit.script, unit.effects,
                                                first->id, next, extended) ||
                            !atomicProgram(extended))
                            break;
                        program = std::move(extended);
                        entry.pastEndId = next;
                        ++count;
                        if (next.empty()) break;
                        cursor = next;
                    }
                }

                // A tiny one-statement interpreter primitive is cheaper
                // than dispatching through Node -> WASM -> C++ and a
                // transaction. Prefer numeric expression-heavy regions.
                // Keep the general effect-safe path available explicitly
                // for parity/scheduler testing via XYO_NESTED_WASM_ALL.
                const bool warpLoop = pureNumericWarpLoop(program);
                const bool numeric = warpLoop || numericScalarProgram(program);
                const bool worthwhile = warpLoop ||
                    program.exprs.size() >= 4 || program.main.size() >= 3;
                if (count && program.slotIds.size() <= 256 &&
                    program.listIds.size() <= 256 &&
                    (allowMixed || (numeric && worthwhile))) {
                    // All scoped WASM regions require exact dynamic Scratch
                    // Value conversion and transactional list/string helpers.
                    program.useHostMath = true;
                    program.allowBoundedConditionalLoop = warpLoop;
                    const char* requested =
                        std::getenv("XYO_WASM_HOT_EMITTER");
                    const bool wantBinaryen = requested &&
                        std::strcmp(requested, "binaryen") == 0;
                    const bool builtin = requested &&
                        std::strcmp(requested, "builtin") == 0;
                    const bool hasC = hasBinaryenCAPI();
                    const bool hasHost = hasHostOptimizer();
                    const bool useBinaryen = wantBinaryen ||
                        (!builtin && (hasC || hasHost));
                    if ((!wantBinaryen || hasC || hasHost) &&
                        (!requested || !*requested || builtin ||
                         wantBinaryen ||
                         std::strcmp(requested, "auto") == 0)) {
                        const WasmBackend backend =
                            useBinaryen && hasC
                                ? WasmBackend::Binaryen
                                : WasmBackend::Builtin;
                        auto result = compileWithBackend(program, backend, 1);
                        if (std::getenv("XYO_NESTED_WASM_DEBUG") && warpLoop)
                            std::fprintf(stderr, "[nested-wasm-debug] op=%s compileError=%s bytes=%zu emitter=%d\n",
                                first->opcode.c_str(), result.error.c_str(),
                                result.bytes.size(), int(backend));
                        if (result.error.empty() &&
                            !result.bytes.empty() &&
                            result.bytes.size() <= 1024 * 1024) {
                            const int handle = instantiate(
                                result.bytes.data(),
                                static_cast<int>(result.bytes.size()),
                                useBinaryen && !hasC && hasHost);
                            if (std::getenv("XYO_NESTED_WASM_DEBUG") && warpLoop)
                                std::fprintf(stderr, "[nested-wasm-debug] op=%s instantiatedHandle=%d\n",
                                    first->opcode.c_str(), handle);
                            if (handle >= 0) {
                                auto code = std::make_shared<HotCode>(
                                    handle, program.slotIds);
                                code->listIds = std::move(program.listIds);
                                code->stringLiterals =
                                    std::move(program.stringLiterals);
                                // Any candidate can allocate a string via
                                // a dynamic reporter or numeric conversion.
                                code->usesStringHost = true;
                                code->fastNumeric = numeric;
                                code->nestedWarpRepeat = warpLoop;
                                code->nestedWarpPcStack = warpLoop &&
                                    std::any_of(
                                        program.stmts.begin(),
                                        program.stmts.end(),
                                        [](const Stmt& stmt) {
                                            return stmt.k == SK::WarpReturnPcStack;
                                        });
                                if (code->nestedWarpPcStack) {
                                    const auto it = std::find_if(
                                        program.stmts.begin(),
                                        program.stmts.end(),
                                        [](const Stmt& stmt) {
                                            return stmt.k == SK::WarpReturnPcStack;
                                        });
                                    code->nestedFrameWords = uint32_t(
                                        ir::AtomicWarpExecutionAbi::frameWords(
                                            it->argumentSlots.size()));
                                }
                                code->nestedWarpReturnStack = warpLoop &&
                                    std::any_of(
                                        program.stmts.begin(),
                                        program.stmts.end(),
                                        [](const Stmt& stmt) {
                                            return stmt.k == SK::WarpReturnStack;
                                        });
                                code->nestedWarpTailRecursion = warpLoop &&
                                    std::any_of(
                                        program.stmts.begin(),
                                        program.stmts.end(),
                                        [](const Stmt& stmt) {
                                            return stmt.k == SK::WarpTailRecurse;
                                        });
                                code->nestedWarpTree = warpLoop &&
                                    std::count_if(
                                        program.stmts.begin(),
                                        program.stmts.end(),
                                        [](const Stmt& stmt) {
                                            return stmt.k == SK::WarpTailRecurse ||
                                                stmt.k == SK::WarpReturnStack ||
                                                stmt.k == SK::WarpReturnPcStack ||
                                                stmt.k == SK::Repeat ||
                                                stmt.k == SK::Until ||
                                                stmt.k == SK::While ||
                                                stmt.k == SK::ForEach;
                                        }) > 1;
                                code->writableSlots.resize(
                                    code->slotIds.size(), 0);
                                for (const auto& statement : program.stmts) {
                                    const bool tagged =
                                        statement.k == SK::SetVarRaw ||
                                        statement.k == SK::SetVarValue ||
                                        statement.k == SK::SetVarBoolean;
                                    const bool numeric =
                                        statement.k == SK::SetVar ||
                                        statement.k == SK::ChangeVar ||
                                        statement.k == SK::ForEach;
                                    if ((tagged || numeric) &&
                                        statement.var <
                                            code->writableSlots.size()) {
                                        code->writableSlots[statement.var] =
                                            std::max<uint8_t>(
                                                code->writableSlots[
                                                    statement.var],
                                                tagged ? 2 : 1);
                                    }
                                }
                                code->nestedIdentity = ++compiled_;
                                if (warpLoop) ++warpCompiled_;
                                if (code->nestedWarpTree) ++treeCompiled_;
                                if (code->nestedWarpTailRecursion) ++tailCompiled_;
                                if (code->nestedWarpReturnStack) ++returnCompiled_;
                                if (code->nestedWarpPcStack) ++pcCompiled_;
                                entry.code = std::move(code);
                            }
                        }
                    }
                }
            }
        }
    }
    if (!entry.code || entry.failures >= 2) {
        if (permanentlyUnsupported && entry.attempted)
            *permanentlyUnsupported = true;
        return false;
    }
    const bool fast = entry.code->fastNumeric;
    const HotResult result = fast
        ? executeNestedNumeric(*entry.code, thread)
        : executeHybridRegion(*entry.code, thread);
    if (result != HotResult::Completed) {
        ++bailed_;
        if (entry.code->nestedWarpRepeat) ++warpBailed_;
        if (entry.code->nestedWarpTree) ++treeBailed_;
        if (entry.code->nestedWarpTailRecursion) ++tailBailed_;
        if (entry.code->nestedWarpReturnStack) ++returnBailed_;
        if (entry.code->nestedWarpPcStack) ++pcBailed_;
        ++entry.failures;
        if (entry.failures >= 2) {
            entry.code.reset();
            if (permanentlyUnsupported) *permanentlyUnsupported = true;
        }
        return false;
    }
    ++executed_;
    if (fast) ++fastExecuted_;
    if (entry.code->nestedWarpRepeat) ++warpExecuted_;
    if (entry.code->nestedWarpTree) ++treeExecuted_;
    if (entry.code->nestedWarpTailRecursion) ++tailExecuted_;
    if (entry.code->nestedWarpReturnStack) ++returnExecuted_;
    if (entry.code->nestedWarpPcStack) ++pcExecuted_;
    pastEndId = entry.pastEndId;
    return true;
#endif
}

} // namespace xyo::wasm
