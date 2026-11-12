// LLVM adapter for the backend-neutral, verified per-iteration numerical CFG.
// Region selection is the default repeat path where it is profitable; unknown
// effects never enter this graph, and complete iterations alone publish state.
#include "numeric_region_mixed_ssa.inc"
void emitNumericRegionChunk(const NumericRepeatPlan& plan,
                            llvm::Value* remaining32,
                            AllocaInst* countdown, BasicBlock* loopHead) {
    const char* mixedMode = std::getenv("XYO_JIT_NUMERIC_REGION_MIXED");
    const bool mixedEnabled = !mixedMode || mixedMode[0] != '0';
    if (plan.region->graph.listSlots ||
        (!plan.regionAnalysis.integerSafe && mixedEnabled)) {
        emitMixedNumericRegionChunk(plan, remaining32, countdown, loopHead);
        return;
    }
    using ROp = ir::region::Op;
    const auto& graph = plan.region->graph;
    const bool integers = plan.regionAnalysis.integerSafe;
    const char* factMode = std::getenv("XYO_JIT_NUMERIC_REGION_FACT_GUARDS");
    const bool proofFlags = !factMode || factMode[0] != '0';
    bool narrowIntegers = integers;
    for (size_t i = 0; i < graph.ssa.definitions.size() && narrowIntegers; ++i) {
        if (graph.ssa.definitions[i].type != ir::Type::F64) continue;
        const auto& fact = plan.regionAnalysis.values[i];
        narrowIntegers = fact.finite && fact.integral &&
            fact.lo >= -2147483648.0L && fact.hi <= 2147483647.0L;
    }
    const char* costMode = std::getenv("XYO_JIT_NUMERIC_REGION_COST_SELECT");
    const bool costSelect = !costMode || costMode[0] != '0';
    if (costSelect && ir::region::hasNestedConditional(graph))
        narrowIntegers = false;
    // Width legality follows facts; optional CFG cost hints may choose wider.
    auto* numericType = integers ? (narrowIntegers ? i32t : i64t) : f64t;
    BasicBlock* fast = newBB("rep.region.fast");
    BasicBlock* slow = newBB("rep.region.slow");
    llvm::Value* valid = b.CreateICmpNE(curYieldWarp, ci32(0));
    std::vector<llvm::Value*> initial;
    // A readonly input can be coerced only if every use is arithmetic. A
    // direct comparison, Phi or published exact copy requires its Number tag.
    const auto arithmeticOnlyInput = [&](size_t slot) {
        if (graph.written[slot]) return false;
        const auto id = graph.inputs[slot];
        for (size_t i = 0; i < graph.outputs.size(); ++i)
            if (graph.written[i] && graph.outputs[i] == id) return false;
        for (const auto& block : graph.ssa.cfg.blocks())
            for (const auto& phi : block.phis)
                for (const auto& edge : phi.inputs)
                    if (edge.value == id) return false;
        for (const auto& block : graph.instructions) {
            for (const auto& inst : block) {
                if (inst.op == ROp::Parameter || inst.op == ROp::Constant)
                    continue;
                const bool used = inst.lhs == id ||
                    (inst.op != ROp::Not && inst.rhs == id);
                if (!used) continue;
                if (inst.op != ROp::Add && inst.op != ROp::Subtract &&
                    inst.op != ROp::Multiply && inst.op != ROp::Divide &&
                    inst.op != ROp::Modulo) return false;
            }
        }
        return true;
    };
    for (size_t slot = 0; slot < plan.variables.size(); ++slot) {
        llvm::Value* bits = emitVarGet(plan.variables[slot].field);
        llvm::Value* plain = b.CreateICmpSLT(bits, ci64(val::kNan));
        const bool coerce = arithmeticOnlyInput(slot);
        llvm::Value* number = coerce ? emitToNumF64(bits) : b.CreateBitCast(bits, f64t);
        llvm::Value* accepted = plain;
        if (coerce) {
            llvm::Value* string = b.CreateOr(
                b.CreateICmpEQ(bits, ci64(val::kEmptyStr)),
                b.CreateICmpEQ(b.CreateLShr(bits, ci64(48)), ci64(val::kTagStr >> 48)));
            accepted = b.CreateOr(plain, string, "region.readonly.accepted");
        }
        llvm::Value* eligible = b.CreateAnd(accepted, b.CreateFCmpORD(number, number));
        if (integers) {
            llvm::Value* range = b.CreateAnd(
                b.CreateFCmpOGE(number, cf64(0.0)),
                b.CreateFCmpOLE(number, cf64(2097152.0)));
            eligible = b.CreateAnd(eligible, b.CreateAnd(range,
                b.CreateFCmpOEQ(number, b.CreateUnaryIntrinsic(Intrinsic::trunc, number))));
            eligible = b.CreateAnd(eligible, b.CreateICmpNE(
                b.CreateBitCast(number, i64t), ci64(0x8000000000000000ull)));
        }
        valid = b.CreateAnd(valid, eligible);
        initial.push_back(integers
            ? b.CreateFPToSI(b.CreateSelect(eligible, number, cf64(0.0)), numericType)
            : number);
    }
    b.CreateCondBr(valid, fast, slow);
    b.SetInsertPoint(fast);
    llvm::Value* ticks = b.CreateLoad(i64t, curYieldCnt);
    llvm::Value* untilPoll = yieldToPoll(ticks);
    llvm::Value* available = b.CreateZExt(remaining32, i64t);
    llvm::Value* chunk = b.CreateSelect(b.CreateICmpULT(available, untilPoll), available, untilPoll);
    BasicBlock* preheader = b.GetInsertBlock();
    BasicBlock* head = newBB("rep.region.head");
    BasicBlock* tail = newBB("rep.region.tail");
    BasicBlock* done = newBB("rep.region.exit");
    bool needsBail = false;
    for (const auto& block : graph.instructions)
        for (const auto& inst : block)
            needsBail |= !integers && (inst.op == ROp::Add || inst.op == ROp::Subtract ||
                inst.op == ROp::Multiply || inst.op == ROp::Divide || inst.op == ROp::Modulo);
    BasicBlock* bail = needsBail ? newBB("rep.region.bail") : nullptr;
    b.CreateBr(head);
    b.SetInsertPoint(head);
    auto* iteration = b.CreatePHI(i64t, 2, "region.iter");
    iteration->addIncoming(ci64(0), preheader);
    std::vector<llvm::PHINode*> carries;
    for (llvm::Value* value : initial) {
        auto* phi = b.CreatePHI(numericType, 2, "region.var");
        phi->addIncoming(value, preheader);
        carries.push_back(phi);
    }
    std::vector<BasicBlock*> blocks, ends(graph.instructions.size());
    for (size_t i = 0; i < graph.instructions.size(); ++i)
        blocks.push_back(newBB(("region.cfg." + std::to_string(i)).c_str()));
    b.CreateBr(blocks[graph.entry]);
    std::vector<llvm::Value*> values(graph.ssa.definitions.size(), nullptr);
    // Declare all graph PHIs before emitting any operation into those blocks.
    for (size_t id = 0; id < graph.ssa.definitions.size(); ++id) {
        const auto& def = graph.ssa.definitions[id];
        if (!def.phi) continue;
        b.SetInsertPoint(blocks[def.block]);
        auto* type = def.type == ir::Type::I1 ? b.getInt1Ty() : numericType;
        values[id] = b.CreatePHI(type,
            graph.ssa.cfg.block(def.block).phis[def.phiIndex].inputs.size(), "region.merge");
    }
    for (auto blockId : ir::region::topologicalOrder(graph)) {
        b.SetInsertPoint(blocks[blockId]);
        for (const auto& inst : graph.instructions[blockId]) {
            llvm::Value* lhs = nullptr;
            llvm::Value* rhs = nullptr;
            if (inst.op != ROp::Parameter && inst.op != ROp::Constant) {
                lhs = values[inst.lhs];
                if (inst.op != ROp::Not) rhs = values[inst.rhs];
            }
            llvm::Value* result = nullptr;
            bool arithmetic = false;
            switch (inst.op) {
            case ROp::Parameter: result = carries[inst.slot]; break;
            case ROp::Constant:
                result = integers
                    ? (narrowIntegers ? ci32(int32_t(inst.literal)) : ci64(uint64_t(int64_t(inst.literal))))
                    : cf64(inst.literal);
                break;
            case ROp::Add:
                result = integers ? (proofFlags ? b.CreateNSWAdd(lhs, rhs) : b.CreateAdd(lhs, rhs)) : b.CreateFAdd(lhs, rhs);
                arithmetic = true; break;
            case ROp::Subtract:
                result = integers ? (proofFlags ? b.CreateNSWSub(lhs, rhs) : b.CreateSub(lhs, rhs)) : b.CreateFSub(lhs, rhs);
                arithmetic = true; break;
            case ROp::Multiply:
                result = integers ? (proofFlags ? b.CreateNSWMul(lhs, rhs) : b.CreateMul(lhs, rhs)) : b.CreateFMul(lhs, rhs);
                arithmetic = true; break;
            case ROp::Divide:
                result = b.CreateFDiv(lhs, rhs); arithmetic = true; break;
            case ROp::Modulo:
                if (integers) result = b.CreateURem(lhs, rhs);
                else {
                    llvm::Value* remainder = b.CreateFRem(lhs, rhs);
                    result = b.CreateSelect(
                        b.CreateFCmpOLT(b.CreateFDiv(remainder, rhs), cf64(0.0)),
                        b.CreateFAdd(remainder, rhs), remainder);
                }
                arithmetic = true; break;
            case ROp::Less:
                result = integers ? b.CreateICmpSLT(lhs, rhs) : b.CreateFCmpOLT(lhs, rhs); break;
            case ROp::Greater:
                result = integers ? b.CreateICmpSGT(lhs, rhs) : b.CreateFCmpOGT(lhs, rhs); break;
            case ROp::Equal:
                result = integers ? b.CreateICmpEQ(lhs, rhs) : b.CreateFCmpOEQ(lhs, rhs); break;
            case ROp::And: result = b.CreateAnd(lhs, rhs); break;
            case ROp::Or: result = b.CreateOr(lhs, rhs); break;
            case ROp::Not: result = b.CreateNot(lhs); break;
            }
            values[inst.result] = result;
            if (arithmetic && !integers) {
                // Bail immediately: a semantic Number NaN must not enter a
                // comparison or a later Scratch arithmetic coercion as f64.
                BasicBlock* next = newBB("region.non_nan");
                b.CreateCondBr(b.CreateFCmpORD(result, result), next, bail);
                b.SetInsertPoint(next);
            }
        }
        ends[blockId] = b.GetInsertBlock();
        const auto& term = graph.ssa.cfg.block(blockId).terminator;
        switch (term.kind) {
        case ir::TerminatorKind::Jump: b.CreateBr(blocks[term.trueBlock]); break;
        case ir::TerminatorKind::Branch:
            b.CreateCondBr(values[term.condition], blocks[term.trueBlock], blocks[term.falseBlock]); break;
        case ir::TerminatorKind::Return: b.CreateBr(tail); break;
        default: llvm_unreachable("verified numerical CFG has a terminator");
        }
    }
    for (size_t id = 0; id < graph.ssa.definitions.size(); ++id) {
        const auto& def = graph.ssa.definitions[id];
        if (!def.phi) continue;
        auto* phi = llvm::cast<llvm::PHINode>(values[id]);
        for (const auto& edge : graph.ssa.cfg.block(def.block).phis[def.phiIndex].inputs)
            phi->addIncoming(values[edge.value], ends[edge.predecessor]);
    }
    const auto publish = [&](const std::vector<llvm::Value*>& state) {
        for (size_t i = 0; i < state.size(); ++i) {
            if (!graph.written[i]) continue;
            llvm::Value* number = integers ? b.CreateSIToFP(state[i], f64t) : state[i];
            llvm::Value* bits = b.CreateBitCast(number, i64t);
            const auto* field = plan.variables[i].field;
            if (PromotedVar* promoted = findPromoted(field); promoted && promoted->numeric)
                storeNumericPair(*promoted, bits, number, b.getInt1(true));
            else emitVarSet(field, bits);
        }
    };
    if (bail) {
        b.SetInsertPoint(bail);
        std::vector<llvm::Value*> before(carries.begin(), carries.end());
        publish(before);
        b.CreateStore(b.CreateSub(remaining32, b.CreateTrunc(iteration, i32t)), countdown);
        b.CreateStore(b.CreateAdd(ticks, iteration), curYieldCnt);
        b.CreateBr(slow);
    }
    b.SetInsertPoint(tail);
    std::vector<llvm::Value*> output;
    for (auto id : graph.outputs) output.push_back(values[id]);
    llvm::Value* nextIteration = b.CreateAdd(iteration, ci64(1));
    iteration->addIncoming(nextIteration, tail);
    for (size_t i = 0; i < carries.size(); ++i) carries[i]->addIncoming(output[i], tail);
    b.CreateCondBr(b.CreateICmpULT(nextIteration, chunk), head, done);
    b.SetInsertPoint(done);
    publish(output);
    b.CreateStore(b.CreateSub(remaining32, b.CreateTrunc(chunk, i32t)), countdown);
    emitSsaChunkScheduler(ticks, chunk, untilPoll, true);
    b.CreateBr(loopHead);
    b.SetInsertPoint(slow);
}
