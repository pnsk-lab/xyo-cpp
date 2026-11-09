// Browser WebAssembly HotSpot backend. Unsupported scripts remain interpreted.
#include "backend.hpp"
#include "jit.hpp"
#include "../wasm/binaryen_backend.hpp"
#include "../wasm/binaryen_c_api.hpp"
#include "../wasm/host_bridge.hpp"
#include "../wasm/hot_jit.hpp"
#include "../wasm/wasm_compile.hpp"
#include "../wasm/wasm_emit.hpp"
#include "../ir/cfg_lower_wasm.hpp"
#include "../ir/scratch_numeric_kernel.hpp"
#include "../ir/scratch_pipeline.hpp"
#include "../engine/thread.hpp"
#include "../engine/runtime.hpp"
#include "../model/target.hpp"
#include "../model/blocks.hpp"
#include "../model/variable.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <algorithm>
#include <cmath>
#include <functional>

namespace xyo {
namespace {

// Compile a hybrid script from the same lossless Scratch compilation unit as
// the ordinary full WASM path. A region is either a transactionally compiled
// run of consecutive top-level statements or the original Scratch statements
// interpreted with their full stack/yield state.
static std::unique_ptr<CompiledScript> compileHybridWasm(
    Thread& thread, const ir::ScratchCompilationUnit& unit, const Block* top) {
#if !defined(__EMSCRIPTEN__) || !defined(XYO_WASM_HOTSPOT)
    (void)thread; (void)unit; (void)top;
    return {};
#else
    // LLVM and WASM must consume the exact same immutable Scratch frontend
    // snapshot. Never recapture on hybrid fallback; doing so can mix source
    // revisions and needlessly repeat the reachable procedure traversal.
    if (!top || !unit.valid() || unit.script.topId != top->id) return {};
    const Block* capturedTop = unit.script.getBlock(unit.script.topId);
    if (!capturedTop) return {};

    std::vector<const Block*> chain;
    std::unordered_set<std::string> seen;
    for (std::string id = capturedTop->next; !id.empty();) {
        if (chain.size() >= 256 || !seen.insert(id).second) return {};
        const Block* block = unit.script.getBlock(id);
        if (!block) return {};
        chain.push_back(block);
        id = block->next;
    }
    if (chain.empty()) return {};

    const auto selection = wasm::selectHotWasmBackend(
        std::getenv("XYO_WASM_HOT_EMITTER"),
        wasm::hasBinaryenCAPI(), wasm::hasHostOptimizer());
    if (!selection.valid()) {
        if (std::getenv("XYO_JIT_VERBOSE"))
            std::fprintf(stderr, "xyo-wasm-hot: %s\n",
                         selection.error.c_str());
        return {};
    }

    // No non-warp nested Scratch loop may run atomically as a WASM region.
    // Reuse the same conservative scheduler rule as the full compiler:
    // only bounded-by-fuel repeat loops *inside* an inlined warp procedure.
    const auto safeRegion = [](const wasm::Program& program) {
        const auto isLoop = [](wasm::SK kind) {
            return kind == wasm::SK::Repeat ||
                   kind == wasm::SK::Until ||
                   kind == wasm::SK::While ||
                   kind == wasm::SK::Forever ||
                   kind == wasm::SK::ForEach;
        };
        std::function<bool(uint32_t, bool)> check =
            [&](uint32_t id, bool warp) -> bool {
            if (id >= program.stmts.size()) return false;
            const auto& statement = program.stmts[id];
            if (isLoop(statement.k) &&
                (!warp || statement.k != wasm::SK::Repeat))
                return false;
            const bool childWarp =
                warp || statement.k == wasm::SK::InlineProcedure;
            for (uint32_t next : statement.body)
                if (!check(next, childWarp)) return false;
            for (uint32_t next : statement.elseBody)
                if (!check(next, childWarp)) return false;
            return true;
        };
        for (uint32_t id : program.main)
            if (!check(id, false)) return false;
        return true;
    };

    auto hybrid = std::make_shared<wasm::HotCode>(
        -1, std::vector<std::string>{});
    size_t compiledCount = 0, compiledBytes = 0;
    for (size_t i = 0; i < chain.size();) {
        size_t end = i + 1;
        wasm::Program program;
        if (!wasm::lowerCapturedRange(unit.script, unit.effects,
                                      chain[i]->id,
                                      end < chain.size() ? chain[end]->id : "",
                                      program) ||
            !safeRegion(program)) {
            // Coalesce all adjacent interpreter-only blocks to avoid one
            // virtual handoff per Scratch statement.
            if (!hybrid->hybridRegions.empty() &&
                !hybrid->hybridRegions.back().code)
                hybrid->hybridRegions.back().pastEndId =
                    end < chain.size() ? chain[end]->id : "";
            else
                hybrid->hybridRegions.push_back({
                    chain[i]->id,
                    end < chain.size() ? chain[end]->id : "", {}});
            i = end;
            continue;
        }

        // Keep a bounded region so a speculative bailout does not replay
        // huge amounts of previously computed pure Scratch work.
        for (size_t j = end + 1;
             j <= chain.size() && j <= i + 16; ++j) {
            wasm::Program extended;
            if (!wasm::lowerCapturedRange(unit.script, unit.effects,
                       chain[i]->id,
                       j < chain.size() ? chain[j]->id : "",
                       extended) || !safeRegion(extended)) break;
            program = std::move(extended);
            end = j;
        }
        // The host ABI is available even for pure numeric regions: a
        // variable may become a Scratch string between scheduler ticks.
        program.useHostMath = true;
        std::shared_ptr<wasm::HotCode> regionCode;
        if (compiledCount < 32 &&
            program.slotIds.size() <= 4096 &&
            program.listIds.size() <= 4096) {
            auto emitted = wasm::compileWithBackend(program, selection.emitter, 1);
            if (emitted.error.empty() && !emitted.bytes.empty() &&
                emitted.bytes.size() <= 4 * 1024 * 1024) {
                const int handle = wasm::instantiate(
                    emitted.bytes.data(),
                    static_cast<int>(emitted.bytes.size()), selection.hostOptimize);
                if (handle >= 0) {
                    regionCode = std::make_shared<wasm::HotCode>(
                        handle, program.slotIds);
                    regionCode->listIds = program.listIds;
                    regionCode->stringLiterals = program.stringLiterals;
                    regionCode->usesStringHost = !program.stringLiterals.empty();
                    for (const auto& e : program.exprs)
                        if (e.k == wasm::EK::StringJoin ||
                            e.k == wasm::EK::ListContents ||
                            e.k == wasm::EK::StringLetter ||
                            e.k == wasm::EK::StringContains ||
                            e.k == wasm::EK::StringLengthValue ||
                            e.k == wasm::EK::LessThan ||
                            e.k == wasm::EK::GreaterThan ||
                            e.k == wasm::EK::Equals)
                            regionCode->usesStringHost = true;
                    regionCode->writableSlots.resize(program.slotIds.size());
                    for (const auto& stmt : program.stmts) {
                        if (stmt.var >= regionCode->writableSlots.size())
                            continue;
                        const bool tagged =
                            stmt.k == wasm::SK::SetVarRaw ||
                            stmt.k == wasm::SK::SetVarValue ||
                            stmt.k == wasm::SK::SetVarBoolean;
                        const bool numeric =
                            stmt.k == wasm::SK::SetVar ||
                            stmt.k == wasm::SK::ChangeVar ||
                            stmt.k == wasm::SK::ForEach;
                        if (tagged || numeric)
                            regionCode->writableSlots[stmt.var] =
                                std::max<uint8_t>(
                                    regionCode->writableSlots[stmt.var],
                                    tagged ? 2 : 1);
                    }
                    ++compiledCount;
                    compiledBytes += emitted.bytes.size();
                }
            }
        }
        if (!regionCode && !hybrid->hybridRegions.empty() &&
            !hybrid->hybridRegions.back().code) {
            hybrid->hybridRegions.back().pastEndId =
                end < chain.size() ? chain[end]->id : "";
        } else {
            hybrid->hybridRegions.push_back({
                chain[i]->id,
                end < chain.size() ? chain[end]->id : "",
                std::move(regionCode)});
        }
        i = end;
    }
    // No worthwhile WASM found; preserve the ordinary interpreter path.
    if (!compiledCount || hybrid->hybridRegions.empty()) return {};

    auto compiled = std::make_unique<CompiledScript>();
    compiled->wasm = std::move(hybrid);
    compiled->mayYield = true;  // an interpreted region may yield anywhere
    compiled->mainThread = true;
    compiled->shortSync = false;
    if (std::getenv("XYO_JIT_VERBOSE"))
        std::fprintf(stderr,
            "xyo-wasm-hot: hybrid script=%s regions=%zu compiled=%zu "
            "bytes=%zu emitter=%s\n",
            thread.topBlock.c_str(),
            compiled->wasm->hybridRegions.size(), compiledCount, compiledBytes,
            selection.name());
    return compiled;
#endif
}

class WasmJitBackend final : public JitBackend {
public:
    bool ok() const override {
#if defined(__EMSCRIPTEN__) && defined(XYO_WASM_HOTSPOT)
        return true;
#else
        return false;
#endif
    }

    std::unique_ptr<CompiledScript> compile(
        Thread& thread, Runtime&, const Block* top, uint64_t,
        const ir::ScratchCompilationUnit& source) override {
#if !defined(__EMSCRIPTEN__) || !defined(XYO_WASM_HOTSPOT)
        (void)thread; (void)top;
        return {};
#else
        // Match the event dispatch of the LLVM backend. Static hats are
        // already filtered/activated by Runtime::startHats, so their bodies
        // can run directly in WASM. Edge-activated hats require evaluation
        // of sensing reporters and remain in the interpreter.
        if (!source.valid() || !top ||
            source.script.topId != top->id) return {};
        const Block* capturedTop =
            source.script.getBlock(source.script.topId);
        if (!capturedTop) return {};
        const std::string& hat = capturedTop->opcode;
        const bool staticHat =
            hat == "event_whenflagclicked" ||
            hat == "event_whenbroadcastreceived" ||
            hat == "event_whenkeypressed" ||
            hat == "event_whenthisspriteclicked" ||
            hat == "event_whenstageclicked" ||
            hat == "event_whenbackdropswitchesto" ||
            hat == "control_start_as_clone";
        if (!thread.target || !thread.blockContainer || !staticHat ||
            thread.stackClick || thread.updateMonitor) return {};

        wasm::Program program;
        if (!wasm::lowerCapturedScript(source.script, program) ||
            program.main.empty() || program.slotIds.size() > 4096 ||
            program.listIds.size() > 4096)
            return compileHybridWasm(thread, source, top);
        // The transactional HotSpot can call Scratch runtime helpers for
        // exact modulo, math operations, and heap-string numeric coercion.
        // ChangeVar may need a string parser even when its source was
        // numeric during compilation; variable types can change later.
        for (const auto& expr : program.exprs)
            if (expr.k == wasm::EK::Mod ||
                (expr.k == wasm::EK::MathOp && expr.var >= 4 && expr.var != 255)) {
                program.useHostMath = true;
                break;
            }
        for (const auto& expr : program.exprs)
            if (expr.k == wasm::EK::StringLength ||
                expr.k == wasm::EK::StringLengthValue ||
                expr.k == wasm::EK::ConstText ||
                expr.k == wasm::EK::ProcedureArgument ||
                expr.k == wasm::EK::StringJoin ||
                expr.k == wasm::EK::ListContents ||
                expr.k == wasm::EK::StringLetter ||
                expr.k == wasm::EK::StringContains ||
                expr.k == wasm::EK::LessThan ||
                expr.k == wasm::EK::GreaterThan ||
                expr.k == wasm::EK::Equals ||
                expr.k == wasm::EK::DaysSince2000)
                program.useHostMath = true;
        if (!program.listIds.empty()) program.useHostMath = true;
        for (const auto& stmt : program.stmts)
            if (stmt.k == wasm::SK::ChangeVar) {
                program.useHostMath = true;
                break;
            }

        // Compiled loops are split at every top-level scheduler checkpoint.
        // Loops nested in branches/other loops still use the interpreter
        // because they require a general resumable control-flow stack.
        const auto isLoop = [](wasm::SK kind) {
            return kind == wasm::SK::Repeat || kind == wasm::SK::Until ||
                   kind == wasm::SK::While || kind == wasm::SK::Forever ||
                   kind == wasm::SK::ForEach;
        };
        std::vector<size_t> loopPositions;
        for (size_t i = 0; i < program.main.size(); ++i)
            if (isLoop(program.stmts[program.main[i]].k))
                loopPositions.push_back(i);
        // General nested loops need their own resumable scheduler frames.
        // A repeat entirely inside a (nonrecursive) warp procedure may be
        // emitted synchronously: wasm_compile's shared loop-fuel guard
        // speculatively limits work and rolls back before interpreter
        // fallback. Dynamic iteration counts are safe only in that context.
        std::function<bool(uint32_t, bool)> safeNested =
            [&](uint32_t index, bool inWarp) -> bool {
            if (index >= program.stmts.size()) return false;
            const auto& stmt = program.stmts[index];
            if (isLoop(stmt.k) &&
                (!inWarp || stmt.k != wasm::SK::Repeat ||
                 stmt.expr >= program.exprs.size()))
                return false;
            const bool childWarp = inWarp ||
                stmt.k == wasm::SK::InlineProcedure;
            for (uint32_t child : stmt.body)
                if (!safeNested(child, childWarp)) return false;
            for (uint32_t child : stmt.elseBody)
                if (!safeNested(child, childWarp)) return false;
            return true;
        };
        for (uint32_t index : program.main) {
            if (index >= program.stmts.size())
                return compileHybridWasm(thread, source, top);
            const auto& stmt = program.stmts[index];
            if (isLoop(stmt.k)) {
                for (uint32_t child : stmt.body)
                    if (!safeNested(child, false))
                        return compileHybridWasm(thread, source, top);
            } else if (!safeNested(index, false)) {
                return compileHybridWasm(thread, source, top);
            }
        }
        if (loopPositions.size() > 32)
            return compileHybridWasm(thread, source, top);
        const bool hasLoop = !loopPositions.empty();

        // The source block chain and lowered main statements correspond 1:1.
        // Record exact continuation IDs so a deoptimization never replays
        // already-committed statements or completed loops.
        std::vector<const Block*> topLevel;
        topLevel.reserve(program.main.size());
        for (std::string id = capturedTop->next; !id.empty();) {
            const Block* blk = source.script.getBlock(id);
            if (!blk || topLevel.size() >= program.main.size()) return {};
            topLevel.push_back(blk);
            id = blk->next;
        }
        if (topLevel.size() != program.main.size()) return {};

        const auto toLoopKind = [](wasm::SK op) {
            using Kind = wasm::HotCode::Loop::Kind;
            switch (op) {
            case wasm::SK::Until: return Kind::Until;
            case wasm::SK::While: return Kind::While;
            case wasm::SK::Forever: return Kind::Forever;
            case wasm::SK::ForEach: return Kind::ForEach;
            default: return Kind::Repeat;
            }
        };
        // Check actual variable IDs (including stage variables), with
        // strict scalar resolution. Variable names are intentionally not
        // synthesized: Scratch's name fallback can be ambiguous.
        for (const auto& id : program.slotIds) {
            const Variable* v = thread.target->lookupVariableById(id);
            if (!v || v->type != Variable::kScalarType || v->isCloud)
                return {};
        }

        for (const auto& id : program.listIds) {
            const Variable* v = thread.target->lookupVariableById(id);
            if (!v || v->type != Variable::kListType || v->isCloud ||
                !v->list)
                return {};
        }

        const auto selection = wasm::selectHotWasmBackend(
            std::getenv("XYO_WASM_HOT_EMITTER"),
            wasm::hasBinaryenCAPI(), wasm::hasHostOptimizer());
        if (!selection.valid()) {
            if (std::getenv("XYO_JIT_VERBOSE"))
                std::fprintf(stderr, "xyo-wasm-hot: %s\n",
                             selection.error.c_str());
            return {};
        }
        if (std::getenv("XYO_JIT_VERBOSE"))
            std::fprintf(stderr,
                "xyo-wasm-hot: selecting emitter=%s\n",
                selection.name());
        size_t byteCount = 0;
        const auto compileModule = [&](const wasm::Program& part) -> int {
            auto result = wasm::compileWithBackend(part, selection.emitter, 1);
            if (!result.error.empty() || result.bytes.empty() ||
                result.bytes.size() > 4 * 1024 * 1024) {
                if (std::getenv("XYO_JIT_VERBOSE"))
                    std::fprintf(stderr,
                        "xyo-wasm-hot: compile failed: %s\n",
                        result.error.c_str());
                return -1;
            }
            byteCount += result.bytes.size();
            return wasm::instantiate(result.bytes.data(),
                                    static_cast<int>(result.bytes.size()),
                                    selection.hostOptimize);
        };

        // Count captures use a temporary value slot that is never committed
        // to Scratch. Each loop has a separate WASM body and optional test.
        const auto addCapture = [&](wasm::Program& target, uint32_t expr) {
            wasm::Stmt capture;
            capture.k = wasm::SK::SetVar;
            capture.var = program.nSlots;
            capture.expr = expr;
            target.main.push_back(target.stmt(std::move(capture)));
            ++target.nSlots;
        };
        wasm::Program prefix = program;
        if (hasLoop) {
            prefix.main.assign(program.main.begin(),
                               program.main.begin() + loopPositions.front());
            const auto& first = program.stmts[program.main[loopPositions.front()]];
            if (first.k == wasm::SK::Repeat)
                addCapture(prefix, first.expr);
        }

        const int handle = compileModule(prefix);
        if (handle < 0) return {};
        // Keep the original slot IDs alive while compiling each segment:
        // host-math scratch offsets are derived from slotIds.size().
        // Moving them here used to make subsequent modules write to variable
        // slots instead of scratch (e.g. a modulo in a loop could clobber y).
        auto hot = std::make_shared<wasm::HotCode>(
            handle, program.slotIds);
        hot->listIds = program.listIds;
        hot->stringLiterals = program.stringLiterals;
        hot->usesStringHost = !program.stringLiterals.empty();
        for (const auto& e : program.exprs)
            if (e.k == wasm::EK::StringJoin ||
                e.k == wasm::EK::ListContents ||
                e.k == wasm::EK::StringLetter ||
                e.k == wasm::EK::StringContains ||
                e.k == wasm::EK::StringLengthValue ||
                e.k == wasm::EK::LessThan ||
                e.k == wasm::EK::GreaterThan ||
                e.k == wasm::EK::Equals)
                hot->usesStringHost = true;
        hot->writableSlots.resize(program.slotIds.size());
        for (const auto& stmt : program.stmts) {
            if (stmt.var >= hot->writableSlots.size()) continue;
            const bool tagged = stmt.k == wasm::SK::SetVarRaw ||
                                stmt.k == wasm::SK::SetVarValue ||
                                stmt.k == wasm::SK::SetVarBoolean;
            const bool numeric = stmt.k == wasm::SK::SetVar ||
                                 stmt.k == wasm::SK::ChangeVar ||
                                 stmt.k == wasm::SK::ForEach;
            if (tagged || numeric)
                hot->writableSlots[stmt.var] =
                    std::max<uint8_t>(hot->writableSlots[stmt.var],
                                      tagged ? 2 : 1);
        }
        for (size_t i = 0; i < loopPositions.size(); ++i) {
            const size_t pos = loopPositions[i];
            const auto& stmt = program.stmts[program.main[pos]];
            hot->loops.emplace_back();
            auto& loop = hot->loops.back();
            loop.kind = toLoopKind(stmt.k);
            loop.blockId = topLevel[pos]->id;
            loop.suffixId = topLevel[pos]->next;
            loop.variableSlot = stmt.var;

            wasm::Program body = program;
            body.main = stmt.body;
            loop.bodyHandle = compileModule(body);
            if (loop.bodyHandle < 0) return {};

            if (loop.kind == wasm::HotCode::Loop::Kind::Until ||
                loop.kind == wasm::HotCode::Loop::Kind::While ||
                loop.kind == wasm::HotCode::Loop::Kind::ForEach) {
                wasm::Program condition = program;
                condition.main.clear();
                addCapture(condition, stmt.expr);
                loop.conditionHandle = compileModule(condition);
                if (loop.conditionHandle < 0) return {};
            }

            const size_t endPos = i + 1 < loopPositions.size()
                ? loopPositions[i + 1] : program.main.size();
            wasm::Program suffix = program;
            suffix.main.assign(program.main.begin() + pos + 1,
                               program.main.begin() + endPos);
            if (i + 1 < loopPositions.size()) {
                const auto& next = program.stmts[program.main[endPos]];
                if (next.k == wasm::SK::Repeat)
                    addCapture(suffix, next.expr);
            }
            loop.suffixHandle = compileModule(suffix);
            if (loop.suffixHandle < 0) return {};
        }

        auto compiled = std::make_unique<CompiledScript>();
        compiled->wasm = std::move(hot);
        compiled->mayYield = hasLoop;
        compiled->mainThread = true; // JS VM is not safe on native workers.
        compiled->shortSync = false;
        if (std::getenv("XYO_JIT_VERBOSE"))
            std::fprintf(stderr,
                "xyo-wasm-hot: compiled script=%s bytes=%zu emitter=%s loops=%zu\n",
                thread.topBlock.c_str(), byteCount,
                selection.name(), loopPositions.size());
        return compiled;
#endif
    }

    // The exact same typed, resumable CFG that LLVMBackend::compileNumeric
    // receives. No independent Scratch opcode lowering happens here.
    std::shared_ptr<NumericJitCode> compileNumeric(
        std::shared_ptr<const ir::ScratchNumericKernel> kernel,
        uint64_t) override {
#if !defined(__EMSCRIPTEN__) || !defined(XYO_WASM_HOTSPOT)
        (void)kernel;
        return {};
#else
        if (!kernel || !kernel->executable.resumable ||
            !kernel->executable.externalValueSlots ||
            kernel->executable.yieldBoundaries.empty() ||
            kernel->executable.yieldBoundaries.size() > 16 ||
            !ir::verifyExecutable(kernel->executable)) return {};

        wasm::Module module;
        module.importMemory("env", "memory", 1);
        const uint32_t type = module.type(
            {wasm::ValType::I32, wasm::ValType::I32},
            {wasm::ValType::I64});
        const uint32_t function = module.addFunc(type);
        module.exportFunc("f", function);
        auto& body = module.code(function);
        ir::WasmCfgBackend compiler(body, 2);
        if (!ir::lowerExecutable(kernel->executable, compiler)) return {};
        body.end();
        std::vector<uint8_t> bytes = module.finish();

        const auto selection = wasm::selectHotWasmBackend(
            std::getenv("XYO_WASM_HOT_EMITTER"),
            wasm::hasBinaryenCAPI(), wasm::hasHostOptimizer());
        if (!selection.valid()) return {};
        auto emitted = wasm::optimizeWasmWithBackend(
            std::move(bytes), selection.emitter, 1);
        if (!emitted.error.empty() || emitted.bytes.empty() ||
            emitted.bytes.size() > 4 * 1024 * 1024) return {};
        const int handle = wasm::instantiate(
            emitted.bytes.data(), static_cast<int>(emitted.bytes.size()),
            selection.hostOptimize);
        if (handle < 0) return {};
        // The closure owns the JS module handle independently of its thread.
        auto lifetime = std::shared_ptr<int>(new int(handle), [](int* p) {
            wasm::release(*p);
            delete p;
        });
        auto result = std::make_shared<NumericJitCode>();
        result->kernel = std::move(kernel);
        result->run = [lifetime](uint64_t* slots, uint64_t* frame) {
            return wasm::invokeNumeric(*lifetime,
                static_cast<int>(reinterpret_cast<uintptr_t>(slots)),
                static_cast<int>(reinterpret_cast<uintptr_t>(frame)));
        };
        if (std::getenv("XYO_JIT_VERBOSE"))
            std::fprintf(stderr,
                "xyo-wasm-hot: compiled shared CFG bytes=%zu emitter=%s\n",
                emitted.bytes.size(),
                selection.usesBinaryen() ? "binaryen" : "builtin");
        return result;
#endif
    }

    bool addObjectFile(const void*, size_t) override { return false; }
    void* lookupSymbol(const std::string&) override { return nullptr; }
};

} // namespace
std::unique_ptr<JitBackend> createJitBackend() {
    return std::make_unique<WasmJitBackend>();
}
} // namespace xyo
