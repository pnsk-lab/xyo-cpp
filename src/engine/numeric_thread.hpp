// Experimental scheduler adapter for the resumable shared numeric CFG.
//
// Deliberately opt-in: an embedder supplies an already-compiled LLVM/WASM
// entrypoint with a lifetime-bound runner. The normal Scratch JIT path is not
// replaced. Interpreter reconstruction is restricted to statically verified
// loop, branch, and nonwarp procedure ancestry. Arguments are restored from
// the committed SSA frame, never re-evaluated from mutable Scratch variables.
#pragma once

#include "../ir/scratch_continuation.hpp"
#include "../ir/scratch_f64_kernel.hpp"
#include "../core/cast.hpp"
#include <cmath>
#include "thread.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <unordered_set>

namespace xyo::ir {

// The same Scratch procedure body may be inlined at several call sites.
// Source block IDs are not unique CFG continuation identities. Loop headers
// disambiguate each inlined occurrence; non-loop ancestors have no header.
struct NumericStackSite {
    std::string scratchBlockId;
    std::optional<BlockId> loopHeader;
    // Set for an inlined nonwarp procedure call, not for branches/loops.
    std::optional<size_t> callOccurrence;
};
using NumericLoopPaths = std::unordered_map<BlockId, std::vector<NumericStackSite>>;

struct NumericThreadSession {
    using Runner = std::function<uint64_t(uint64_t*, uint64_t*)>;
    std::shared_ptr<const ScratchNumericKernel> kernel;
    NumericContinuation continuation;
    Runner run;
    NumericLoopPaths loopAncestors;
    bool warp = false;
    bool progressed = false;
    const Blocks* sourceBlocks = nullptr;
    uint64_t sourceRevision = 0;

    bool sourceUnchanged(const Thread& thread) const {
        return thread.blockContainer == sourceBlocks && sourceBlocks &&
            sourceBlocks->revision() == sourceRevision;
    }

    NumericThreadSession(std::shared_ptr<const ScratchNumericKernel> kernel_,
                         Runner runner, bool warp_)
        : kernel(std::move(kernel_)), continuation(*kernel),
          run(std::move(runner)), warp(warp_) {}

    // Compare the executable contract produced from the current live script
    // to the one held by the compiled machine-code runner. This prevents
    // attaching a stale/different kernel after source edits or to a thread
    // whose slots happened to share the same IDs.
    static bool equalCode(const Executable& a, const Executable& b) {
        if (a.variableSlots != b.variableSlots ||
            a.externalValueSlots != b.externalValueSlots ||
            a.resumable != b.resumable ||
            a.instructions.size() != b.instructions.size() ||
            a.ssa.definitions.size() != b.ssa.definitions.size() ||
            a.ssa.cfg.blocks().size() != b.ssa.cfg.blocks().size() ||
            a.yieldBoundaries.size() != b.yieldBoundaries.size())
            return false;
        for (size_t i = 0; i < a.yieldBoundaries.size(); ++i) {
            const auto& x = a.yieldBoundaries[i];
            const auto& y = b.yieldBoundaries[i];
            if (x.latch != y.latch || x.header != y.header ||
                x.scratchBlockId != y.scratchBlockId ||
                x.counterPhi != y.counterPhi ||
                x.repeatCount != y.repeatCount ||
                x.limitValue != y.limitValue) return false;
        }
        for (size_t i = 0; i < a.ssa.definitions.size(); ++i) {
            const auto& x = a.ssa.definitions[i];
            const auto& y = b.ssa.definitions[i];
            if (x.block != y.block || x.type != y.type ||
                x.position != y.position || x.phi != y.phi ||
                x.phiIndex != y.phiIndex) return false;
        }
        for (size_t i = 0; i < a.instructions.size(); ++i) {
            const auto& xs = a.instructions[i];
            const auto& ys = b.instructions[i];
            if (xs.size() != ys.size()) return false;
            for (size_t j = 0; j < xs.size(); ++j) {
                const auto& x = xs[j];
                const auto& y = ys[j];
                if (x.result != y.result || x.op != y.op ||
                    x.lhs != y.lhs || x.rhs != y.rhs ||
                    x.literal != y.literal || x.slot != y.slot)
                    return false;
            }
            const auto& x = a.ssa.cfg.block(i);
            const auto& y = b.ssa.cfg.block(i);
            if (x.phis.size() != y.phis.size()) return false;
            if (x.terminator.kind != y.terminator.kind ||
                x.terminator.trueBlock != y.terminator.trueBlock ||
                x.terminator.falseBlock != y.terminator.falseBlock ||
                x.terminator.condition != y.terminator.condition ||
                x.terminator.result != y.terminator.result)
                return false;
            for (size_t j = 0; j < x.phis.size(); ++j) {
                if (x.phis[j].type != y.phis[j].type ||
                    x.phis[j].inputs.size() != y.phis[j].inputs.size())
                    return false;
                for (size_t k = 0; k < x.phis[j].inputs.size(); ++k) {
                    const auto& aIn = x.phis[j].inputs[k];
                    const auto& bIn = y.phis[j].inputs[k];
                    if (aIn.predecessor != bIn.predecessor ||
                        aIn.value != bIn.value) return false;
                }
            }
        }
        return true;
    }

    // Return false rather than accepting an opaque loop whose continuation
    // cannot be reconstructed by the interpreter on a guard failure.
    static bool safeToAttach(const Thread& thread,
                             const ScratchNumericKernel& kernel,
                             NumericLoopPaths* paths = nullptr) {
        const auto& program = kernel.executable;
        // A fresh Runtime::pushThread invokes tryCompile before installing
        // its root frame; an empty stack is safe only before any execution.
        if (thread.isCompiled || thread.interpreterStarted ||
            thread.isKilled || thread.status == STATUS_DONE ||
            thread.stackClick || thread.updateMonitor ||
            !thread.target || !thread.blockContainer ||
            thread.stack.size() > 1 ||
            (!thread.stack.empty() &&
             thread.peekStack() != thread.topBlock) ||
            !program.resumable || !kernel.requiresLoopScheduler ||
            !program.externalValueSlots ||
            program.yieldBoundaries.empty() ||
            program.yieldBoundaries.size() > 16 ||
            !verifyExecutable(program) ||
            kernel.slotIds.empty()) return false;
        ScratchNumericSlots guard(*thread.target, kernel.slotIds);
        if (!guard.valid()) return false;
        // Every checkpoint must correspond to a unique reachable loop.
        // Reconstruct the complete branch, call, and loop ancestry so a
        // deopt cannot replay a completed call or use freshly read arguments.
        const Block* top = thread.blockContainer->getBlock(thread.topBlock);
        if (!top || !isNumericStaticHat(top->opcode)) return false;
        std::unordered_map<std::string, std::vector<const LoopYieldBoundary*>> boundaries;
        for (const auto& boundary : program.yieldBoundaries)
            boundaries[boundary.scratchBlockId].push_back(&boundary);
        // The source walker and the CFG lowerer both visit call instances
        // in lexical order. Consume distinct headers for each occurrence.
        std::unordered_map<std::string, size_t> nextOccurrence;
        std::unordered_map<std::string, size_t> nextCallOccurrence;
        std::unordered_set<std::string> seen;
        size_t matched = 0;
        auto visit = [&](auto&& self, std::string id,
                         std::vector<NumericStackSite> parents) -> bool {
            for (; !id.empty();) {
                if (!seen.insert(id).second) return false;
                const Block* block = thread.blockContainer->getBlock(id);
                if (!block) return false;
                const bool repeat = block->opcode == "control_repeat";
                const bool loop = repeat || block->opcode == "control_while" ||
                    block->opcode == "control_repeat_until" ||
                    block->opcode == "control_forever";
                if (loop) {
                    const auto found = boundaries.find(id);
                    if (found == boundaries.end()) return false;
                    size_t& index = nextOccurrence[id];
                    if (index >= found->second.size()) return false;
                    const auto& boundary = *found->second[index++];
                    if (repeat != (boundary.counterPhi != kNoValue))
                        return false;
                    if (repeat && boundary.limitValue == kNoValue &&
                        boundary.repeatCount > kMaxScheduledNumericRepeat)
                        return false;
                    ++matched;
                    if (paths && !paths->emplace(boundary.header, parents).second)
                        return false;
                    auto nestedParents = parents;
                    nestedParents.push_back({id, boundary.header});
                    if (const auto* input = block->inputs.find("SUBSTACK")) {
                        const auto child = input->block.empty()
                            ? input->shadow : input->block;
                        if (!self(self, child, std::move(nestedParents)))
                            return false;
                    }
                } else if (block->opcode == "procedures_call" && block->mutation) {
                    const auto code = block->mutation->attr("proccode");
                    const auto* prototype = thread.blockContainer->getProcedurePrototype(code);
                    if (prototype && !prototype->warp()) {
                        const auto foundCall = kernel.procedureArguments.find(id);
                        if (foundCall == kernel.procedureArguments.end()) return false;
                        size_t& occurrence = nextCallOccurrence[id];
                        if (occurrence >= foundCall->second.size()) return false;
                        const size_t callIndex = occurrence++;
                        const auto definition = thread.blockContainer->getProcedureDefinition(code);
                        const auto* entry = thread.blockContainer->getBlock(definition);
                        if (!entry) return false;
                        auto callParents = parents;
                        callParents.push_back({id, std::nullopt, callIndex});
                        auto callerSeen = seen;
                        const bool valid = self(self, entry->next, std::move(callParents));
                        seen = std::move(callerSeen);
                        if (!valid) return false;
                    }
                } else if (block->opcode == "control_if" ||
                           block->opcode == "control_if_else") {
                    auto branchParents = parents;
                    branchParents.push_back({id, std::nullopt});
                    for (const char* name : {"SUBSTACK", "SUBSTACK2"}) {
                        if (const auto* input = block->inputs.find(name)) {
                            const auto child = input->block.empty()
                                ? input->shadow : input->block;
                            if (!self(self, child, branchParents)) return false;
                        }
                    }
                }
                id = block->next;
            }
            return true;
        };
        if (!visit(visit, top->next, {}) ||
            matched != program.yieldBoundaries.size()) return false;
        // Source and CFG walkers must agree on the number of inlined
        // expansions for every source call site.
        for (const auto& [id, variants] : kernel.procedureArguments)
            if (nextCallOccurrence[id] != variants.size()) return false;
        const auto unit = ScratchCompilationUnit::capture(
            *thread.blockContainer, top);
        const auto expected = lowerNumericF64Script(
            unit, NumericF64LoweringOptions{false, true});
        if (!expected || expected->slotIds != kernel.slotIds ||
            expected->resultVariable != kernel.resultVariable ||
            expected->procedureArguments != kernel.procedureArguments ||
            !equalCode(expected->executable, kernel.executable))
            return false;
        return true;
    }
};

inline bool attachNumericThread(
    Thread& thread, std::shared_ptr<const ScratchNumericKernel> kernel,
    NumericThreadSession::Runner runner, bool warp = false) {
    NumericLoopPaths paths;
    if (!kernel || !runner || thread.numericSession ||
        !NumericThreadSession::safeToAttach(thread, *kernel, &paths))
        return false;
    thread.numericSession = std::make_unique<NumericThreadSession>(
        std::move(kernel), std::move(runner), warp);
    thread.numericSession->loopAncestors = std::move(paths);
    thread.numericSession->sourceBlocks = thread.blockContainer;
    thread.numericSession->sourceRevision = thread.blockContainer->revision();
    return true;
}

// Called only after a failed compiled slice, with its speculative writes
// discarded. Rebuild the exact committed loop/call ancestry for the active
// CFG header, including multiple inlined copies of a Scratch procedure.
inline bool restoreNumericInterpreter(Thread& thread,
                                      const NumericThreadSession& session) {
    if (!session.continuation.hasCommittedSlice())
        return true; // original hat is still intact
    const auto& frame = session.continuation.frame();
    const LoopYieldBoundary* active = nullptr;
    for (const auto& candidate :
         session.kernel->executable.yieldBoundaries) {
        if (frame[NumericExecutionAbi::kPcWord] == candidate.header) {
            if (active) return false; // ambiguous continuation
            active = &candidate;
        }
    }
    if (!active) return false;
    const auto path = session.loopAncestors.find(active->header);
    if (path == session.loopAncestors.end()) return false;
    auto sites = path->second;
    sites.push_back({active->scratchBlockId, active->header});
    struct RestoredLoop { std::string id; bool counted; double remaining; bool loop; bool call = false;
        std::unordered_map<std::u16string, Value> parameters;
    };
    std::vector<RestoredLoop> restored;
    for (size_t index = 0; index < sites.size(); ++index) {
        const auto& site = sites[index];
        const auto& id = site.scratchBlockId;
        const LoopYieldBoundary* boundary = nullptr;
        if (site.loopHeader) {
            for (const auto& candidate : session.kernel->executable.yieldBoundaries)
                if (candidate.header == *site.loopHeader &&
                    candidate.scratchBlockId == id) {
                    boundary = &candidate;
                    break;
                }
            if (!boundary) return false;
        }
        if (!boundary) {
            const Block* branch = thread.blockContainer
                ? thread.blockContainer->getBlock(id) : nullptr;
            if (!branch) return false;
            const bool call = branch->opcode == "procedures_call" && branch->mutation;
            if (call) {
                const auto* prototype = thread.blockContainer->getProcedurePrototype(
                    branch->mutation->attr("proccode"));
                const auto args = session.kernel->procedureArguments.find(id);
                if (!prototype || prototype->warp() ||
                    args == session.kernel->procedureArguments.end() ||
                    !site.callOccurrence ||
                    *site.callOccurrence >= args->second.size())
                    return false;
                const auto& binding = args->second[*site.callOccurrence];
                if (prototype->argumentNames.size() !=
                    binding.numbers.size() + binding.booleans.size())
                    return false;
            } else if (branch->opcode != "control_if" &&
                       branch->opcode != "control_if_else") return false;
            restored.push_back({id, false, 0, false, call});
            if (call) {
                const auto& args = session.kernel->procedureArguments.at(id)
                    .at(*site.callOccurrence);
                auto& parameters = restored.back().parameters;
                const auto* prototype = thread.blockContainer->getProcedurePrototype(
                    branch->mutation->attr("proccode"));
                for (const auto& name : prototype->argumentNames) {
                    const auto numeric = args.numbers.find(name);
                    const auto boolean = args.booleans.find(name);
                    if (numeric == args.numbers.end() && boolean == args.booleans.end())
                        return false;
                    const auto word = NumericExecutionAbi::valueWord(
                        numeric != args.numbers.end() ? numeric->second : boolean->second);
                    if (word >= frame.size()) return false;
                    Value value;
                    if (numeric != args.numbers.end()) {
                        value = Value::numCanonical(std::bit_cast<double>(frame[word]));
                    } else {
                        if (frame[word] > 1) return false;
                        value = Value::boolean(frame[word] != 0);
                    }
                    if (!parameters.emplace(name, value).second) return false;
                }
            }
            continue;
        }
        const bool counted = boundary->counterPhi != kNoValue;
        const Block* block = thread.blockContainer
            ? thread.blockContainer->getBlock(id) : nullptr;
        if (!block || (counted ? block->opcode != "control_repeat" :
            (block->opcode != "control_while" &&
             block->opcode != "control_repeat_until" &&
             block->opcode != "control_forever"))) return false;
        uint64_t remaining = 0;
        if (counted) {
            const auto word = NumericExecutionAbi::valueWord(boundary->counterPhi);
            if (word >= frame.size()) return false;
            const auto completed = frame[word];
            uint64_t limit = boundary->repeatCount;
            if (boundary->limitValue != kNoValue) {
                const auto limitWord = NumericExecutionAbi::valueWord(boundary->limitValue);
                if (limitWord >= frame.size()) return false;
                limit = frame[limitWord];
            }
            const bool ancestor = index + 1 < sites.size();
            if (limit > kMaxScheduledNumericRepeat || completed > limit ||
                (ancestor && completed == limit)) return false;
            remaining = limit - completed - (ancestor ? 1 : 0);
        }
        restored.push_back({id, counted, double(remaining), true});
    }
    thread.stack.clear();
    thread.stackFrames.clear();
    thread.trashFrames.clear();
    for (const auto& loop : restored) {
        thread.pushStack(loop.id);
        auto* stackFrame = thread.peekStackFrame();
        if (!stackFrame) return false;
        stackFrame->isLoop = loop.loop;
        stackFrame->executionContext = std::make_unique<ExecContext>();
        if (loop.call) {
            stackFrame->hasParams = true;
            stackFrame->params = loop.parameters;
            stackFrame->executionContext->executed = true;
        }
        stackFrame->executionContext->hasLoopCounter = loop.counted;
        stackFrame->executionContext->loopCounter = loop.remaining;
    }
    thread.interpreterStarted = true;
    thread.status = STATUS_RUNNING;
    return true;
}

} // namespace xyo::ir
