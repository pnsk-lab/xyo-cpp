// Mixed-carrier adapter for a verified, pure numerical region.
// Include inside Codegen, after NumericRepeatPlan is declared. Selection and
// opt-out flags belong to the caller; no Scratch effects are admitted here.
void emitMixedNumericRegionChunk(const NumericRepeatPlan& plan,
                                 llvm::Value* remaining32,
                                 AllocaInst* countdown, BasicBlock* loopHead) {
    using ROp = ir::region::Op;
    const auto& graph = plan.region->graph;
    std::vector<bool> integral(graph.ssa.definitions.size(), false);
    bool narrowIntegers = true;
    for (size_t id = 0; id < integral.size(); ++id) {
        if (graph.ssa.definitions[id].type != ir::Type::F64) continue;
        const auto& fact = plan.regionAnalysis.values[id];
        integral[id] = fact.finite && fact.integral &&
            std::isfinite(fact.lo) && std::isfinite(fact.hi) &&
            fact.lo <= fact.hi && fact.lo >= -281474976710656.0L &&
            fact.hi <= 281474976710656.0L;
        if (integral[id] &&
            (fact.lo < -2147483648.0L || fact.hi > 2147483647.0L))
            narrowIntegers = false;
    }
    const char* costMode = std::getenv("XYO_JIT_NUMERIC_REGION_COST_SELECT");
    const bool costSelect = !costMode || costMode[0] != '0';
    if (costSelect && ir::region::hasNestedConditional(graph))
        narrowIntegers = false;
    auto* integerType = narrowIntegers ? i32t : i64t;
    const char* factMode = std::getenv("XYO_JIT_NUMERIC_REGION_FACT_GUARDS");
    const bool factGuards = !factMode || factMode[0] != '0';
    std::vector<ir::region::FloatFact> floatFacts;
    if (factGuards) {
        std::vector<ir::region::FloatFact> inputFacts(graph.inputs.size());
        for (size_t slot = 0; slot < graph.inputs.size(); ++slot) {
            auto& fact = inputFacts[slot];
            fact.resultNonNaN = true; // Every input is guarded below.
            const auto id = graph.inputs[slot];
            if (integral[id]) {
                const auto& range = plan.regionAnalysis.values[id];
                fact.finite = true;
                fact.lo = range.lo;
                fact.hi = range.hi;
                fact.nonzero = range.lo > 0 || range.hi < 0;
            }
        }
        floatFacts = ir::region::analyzeFloatFacts(graph, inputFacts);
    }
    size_t modSites = 0;
    for (const auto& block : graph.instructions)
        for (const auto& instruction : block)
            modSites += instruction.op == ROp::Modulo;
    const char* signedMod = std::getenv("XYO_JIT_NUMERIC_REPEAT_SIGNED_MOD");
    const bool signedRemainder = narrowIntegers &&
        (signedMod ? signedMod[0] == '1' : modSites == 1);
    std::vector<bool> integerSlots(graph.inputs.size(), false);
    for (size_t slot = 0; slot < integerSlots.size(); ++slot)
        integerSlots[slot] = integral[graph.inputs[slot]] &&
                             integral[graph.outputs[slot]];

    // Projections are deliberately not cached at use sites: a conversion
    // emitted in one branch need not dominate a later use in another branch.
    // LLVM can CSE dominating conversions without violating that invariant.
    struct MixedValue {
        llvm::Value* number = nullptr;
        llvm::Value* integer = nullptr;
    };
    std::vector<MixedValue> values(graph.ssa.definitions.size());
    std::vector<llvm::PHINode*> graphPhis(graph.ssa.definitions.size(), nullptr);
    std::vector<llvm::Value*> booleans(graph.ssa.definitions.size(), nullptr);
    const auto getDouble = [&](ir::ValueId id) -> llvm::Value* {
        const auto& value = values[id];
        return value.number ? value.number : b.CreateSIToFP(value.integer, f64t);
    };
    const auto getInt = [&](ir::ValueId id) -> llvm::Value* {
        const auto& value = values[id];
        return value.integer ? value.integer : b.CreateFPToSI(value.number, integerType);
    };
    // 0 = exact Number tag required, 1 = arithmetic projection, 2 = numeric
    // comparison/index projection (whitespace/invalid strings must still bail).
    // Default ON after mixed/raw-Value parity and paired normal-workload tests.
    // =0 retains the historical duplicate raw-tag/NaN acceptance gates.
    const char* coercionMode = std::getenv("XYO_JIT_COERCION_PROJECTION");
    const bool coercionProjection = !coercionMode || coercionMode[0] != '0';
    const auto projection = [&](ir::ValueId id) {
        for (size_t i = 0; i < graph.outputs.size(); ++i)
            if (graph.written[i] && graph.outputs[i] == id) return 0;
        for (const auto& block : graph.ssa.cfg.blocks())
            for (const auto& phi : block.phis)
                for (const auto& edge : phi.inputs)
                    if (edge.value == id) return 0;
        int mode = 1;
        for (const auto& block : graph.instructions) {
            for (const auto& inst : block) {
                if (inst.op == ROp::Parameter || inst.op == ROp::Constant) continue;
                const bool used = inst.lhs == id ||
                    (inst.op != ROp::Not && inst.op != ROp::ListRead && inst.rhs == id);
                if (!used) continue;
                if (inst.op == ROp::Less || inst.op == ROp::Greater ||
                    inst.op == ROp::Equal || inst.op == ROp::ListRead) mode = 2;
                else if (inst.op != ROp::Add && inst.op != ROp::Subtract &&
                    inst.op != ROp::Multiply && inst.op != ROp::Divide &&
                    inst.op != ROp::Modulo) return 0;
            }
        }
        return mode;
    };
    const auto arithmeticOnlyInput = [&](size_t slot) {
        return !graph.written[slot] && projection(graph.inputs[slot]) == 1;
    };

    BasicBlock* fast = newBB("rep.mixed.fast");
    BasicBlock* slow = newBB("rep.mixed.slow");
    llvm::Value* valid = b.CreateICmpNE(curYieldWarp, ci32(0));
    std::vector<llvm::Value*> initial;
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
            accepted = b.CreateOr(plain, string, "mixed.readonly.accepted");
        }
        llvm::Value* eligible = b.CreateAnd(accepted, b.CreateFCmpORD(number, number));
        // The analysis assumes these bounds for Parameters, even when the
        // physical loop carrier must be f64 because its output is not integral.
        if (integral[graph.inputs[slot]]) {
            llvm::Value* range = b.CreateAnd(
                b.CreateFCmpOGE(number, cf64(0.0)),
                b.CreateFCmpOLE(number, cf64(2097152.0)));
            eligible = b.CreateAnd(eligible, b.CreateAnd(range,
                b.CreateFCmpOEQ(number, b.CreateUnaryIntrinsic(Intrinsic::trunc, number))));
            eligible = b.CreateAnd(eligible, b.CreateICmpNE(
                b.CreateBitCast(number, i64t), ci64(0x8000000000000000ull)));
        }
        valid = b.CreateAnd(valid, eligible);
        // This conversion precedes the guard branch, so reject values are
        // masked before fptosi to avoid poison on strings/NaN/large values.
        initial.push_back(integerSlots[slot]
            ? b.CreateFPToSI(b.CreateSelect(eligible, number, cf64(0.0)), integerType)
            : number);
    }
    struct ListState { llvm::Value* length; llvm::Value* items; };
    std::vector<ListState> lists;
    for (const auto* field : plan.region->lists) {
        llvm::Value* list = varListPtr(field);
        BasicBlock* have = newBB("mixed.list.have");
        BasicBlock* missing = newBB("mixed.list.missing");
        BasicBlock* merge = newBB("mixed.list.header");
        auto* nullPointer = llvm::Constant::getNullValue(ptrt);
        b.CreateCondBr(b.CreateICmpNE(list, nullPointer), have, missing);
        b.SetInsertPoint(have);
        llvm::Value* length = b.CreateLoad(i32t, b.CreateConstGEP1_32(i8t, list, kListLenOff));
        llvm::Value* items = b.CreateLoad(ptrt, b.CreateConstGEP1_32(i8t, list, kListItemsOff));
        llvm::Value* extra = b.CreateLoad(ptrt, b.CreateConstGEP1_32(i8t, list, kListExtraOff));
        // Only the contiguous ABI is dereferenced. Alternate/adaptive stores
        // fall back; the layout, length and pointer are resampled every chunk.
        llvm::Value* contiguous = b.CreateAnd(b.CreateICmpEQ(extra, nullPointer),
            b.CreateOr(b.CreateICmpNE(items, nullPointer), b.CreateICmpEQ(length, ci32(0))));
        BasicBlock* haveEnd = b.GetInsertBlock();
        b.CreateBr(merge);
        b.SetInsertPoint(missing);
        b.CreateBr(merge);
        b.SetInsertPoint(merge);
        auto* len = b.CreatePHI(i32t, 2, "list.length");
        auto* data = b.CreatePHI(ptrt, 2, "list.items");
        auto* usable = b.CreatePHI(b.getInt1Ty(), 2, "list.contiguous");
        len->addIncoming(length, haveEnd); len->addIncoming(ci32(0), missing);
        data->addIncoming(items, haveEnd); data->addIncoming(nullPointer, missing);
        usable->addIncoming(contiguous, haveEnd); usable->addIncoming(b.getFalse(), missing);
        valid = b.CreateAnd(valid, usable);
        lists.push_back({len, data});
    }
    b.CreateCondBr(valid, fast, slow);
    b.SetInsertPoint(fast);
    llvm::Value* ticks = b.CreateLoad(i64t, curYieldCnt);
    llvm::Value* untilPoll = yieldToPoll(ticks);
    llvm::Value* available = b.CreateZExt(remaining32, i64t);
    llvm::Value* chunk = b.CreateSelect(b.CreateICmpULT(available, untilPoll), available, untilPoll);
    BasicBlock* preheader = b.GetInsertBlock();
    BasicBlock* head = newBB("rep.mixed.head");
    BasicBlock* tail = newBB("rep.mixed.tail");
    BasicBlock* done = newBB("rep.mixed.exit");
    // Allocate rollback lazily; a mixed graph can have fractional constants
    // without any floating arithmetic that needs a bailout edge.
    BasicBlock* bail = nullptr;
    b.CreateBr(head);
    b.SetInsertPoint(head);
    auto* iteration = b.CreatePHI(i64t, 2, "mixed.iter");
    iteration->addIncoming(ci64(0), preheader);
    std::vector<llvm::PHINode*> carries;
    for (size_t slot = 0; slot < initial.size(); ++slot) {
        auto* phi = b.CreatePHI(integerSlots[slot] ? integerType : f64t, 2, "mixed.var");
        phi->addIncoming(initial[slot], preheader);
        carries.push_back(phi);
    }
    std::vector<BasicBlock*> blocks, ends(graph.instructions.size());
    for (size_t i = 0; i < graph.instructions.size(); ++i)
        blocks.push_back(newBB(("mixed.cfg." + std::to_string(i)).c_str()));
    b.CreateBr(blocks[graph.entry]);
    // All graph Phis must remain contiguous before any conversions/operations.
    for (size_t id = 0; id < graph.ssa.definitions.size(); ++id) {
        const auto& def = graph.ssa.definitions[id];
        if (!def.phi) continue;
        b.SetInsertPoint(blocks[def.block]);
        auto* type = def.type == ir::Type::I1 ? b.getInt1Ty()
                    : integral[id] ? integerType : f64t;
        auto* phi = b.CreatePHI(type,
            graph.ssa.cfg.block(def.block).phis[def.phiIndex].inputs.size(), "mixed.merge");
        graphPhis[id] = phi;
        if (def.type == ir::Type::I1) booleans[id] = phi;
        else if (integral[id]) values[id].integer = phi;
        else values[id].number = phi;
    }
    // Integer modulo is justified only by the shared positive-literal proof.
    // Do not silently make signed or dynamic-divisor remainder unsigned.
    std::vector<const ir::region::Instruction*> producers(values.size(), nullptr);
    for (const auto& block : graph.instructions)
        for (const auto& inst : block) producers[inst.result] = &inst;
    for (auto blockId : ir::region::topologicalOrder(graph)) {
        b.SetInsertPoint(blocks[blockId]);
        for (const auto& inst : graph.instructions[blockId]) {
            if (inst.op == ROp::Parameter) {
                if (integerSlots[inst.slot]) values[inst.result].integer = carries[inst.slot];
                else {
                    values[inst.result].number = carries[inst.slot];
                    // Never convert an unknown output at the tail. The
                    // Parameter's aggregate 64-step proof licenses this at
                    // graph entry, after the chunk entry guard has succeeded.
                    if (integral[inst.result])
                        values[inst.result].integer = b.CreateFPToSI(carries[inst.slot], integerType);
                }
                continue;
            }
            if (inst.op == ROp::Constant) {
                if (integral[inst.result]) {
                    values[inst.result].integer = narrowIntegers
                        ? ci32(int32_t(inst.literal)) : ci64(uint64_t(int64_t(inst.literal)));
                } else values[inst.result].number = cf64(inst.literal);
                continue;
            }
            if (inst.op == ROp::ListRead) {
                if (!bail) bail = newBB("rep.mixed.bail");
                const auto& list = lists[inst.slot];
                llvm::Value* index0 = nullptr;
                llvm::Value* inBounds = nullptr;
                if (integral[inst.lhs]) {
                    llvm::Value* index = getInt(inst.lhs);
                    if (index->getType() != i64t) index = b.CreateSExt(index, i64t);
                    index0 = b.CreateSub(index, ci64(1));
                    inBounds = b.CreateICmpULT(index0, b.CreateZExt(list.length, i64t));
                } else {
                    llvm::Value* index = getDouble(inst.lhs);
                    llvm::Value* upper = b.CreateFAdd(b.CreateUIToFP(list.length, f64t), cf64(1.0));
                    inBounds = b.CreateAnd(b.CreateFCmpOGE(index, cf64(1.0)), b.CreateFCmpOLT(index, upper));
                }
                BasicBlock* bounded = newBB("mixed.list.bounded");
                b.CreateCondBr(inBounds, bounded, bail);
                b.SetInsertPoint(bounded);
                if (!index0)
                    index0 = b.CreateFPToUI(b.CreateFSub(getDouble(inst.lhs), cf64(1.0)), i64t);
                llvm::Value* raw = b.CreateLoad(i64t, b.CreateGEP(i64t, list.items, index0));
                llvm::Value* number = nullptr;
                llvm::Value* accepted = nullptr;
                const int use = projection(inst.result);
                if (use == 2) {
                    // Numeric comparison compatibility is stricter than toNum:
                    // invalid/whitespace strings must execute the original body.
                    number = emitCmpNumF64(raw, &accepted, false);
                } else if (use == 1) {
                    number = emitToNumF64(raw);
                    if (coercionProjection) {
                        // This SSA result has arithmetic-only consumers: no
                        // raw publication, Phi, comparison or index use.
                        // toNumber accepts EVERY Value (color/undefined -> 0)
                        // and repairs NaN, so a second raw-tag gate is redundant.
                        accepted = b.getTrue();
                    } else {
                        llvm::Value* string = b.CreateOr(
                            b.CreateICmpEQ(raw, ci64(val::kEmptyStr)),
                            b.CreateICmpEQ(b.CreateLShr(raw, ci64(48)), ci64(val::kTagStr >> 48)));
                        llvm::Value* boolean = b.CreateOr(b.CreateICmpEQ(raw, ci64(val::kTrue)),
                                                          b.CreateICmpEQ(raw, ci64(val::kFalse)));
                        accepted = b.CreateOr(b.CreateOr(b.CreateICmpSLT(raw, ci64(val::kNan)),
                                                         b.CreateICmpEQ(raw, ci64(val::kNan))),
                                              b.CreateOr(string, boolean), "mixed.list.raw.accepted");
                    }
                } else {
                    // Exact publication or Phi use cannot lose a String/Bool tag.
                    number = b.CreateBitCast(raw, f64t);
                    accepted = b.CreateICmpSLT(raw, ci64(val::kNan));
                }
                llvm::Value* numeric = graph.listElementsFinite
                    ? b.CreateICmpULT(b.CreateAnd(b.CreateBitCast(number, i64t), ci64(0x7fffffffffffffffull)),
                                     ci64(0x7ff0000000000000ull), "mixed.list.finite")
                    : (coercionProjection && use == 1)
                        ? static_cast<llvm::Value*>(b.getTrue())
                        : b.CreateFCmpORD(number, number);
                BasicBlock* usable = newBB("mixed.list.number");
                b.CreateCondBr(b.CreateAnd(accepted, numeric), usable, bail);
                b.SetInsertPoint(usable);
                values[inst.result].number = number;
                continue;
            }
            if (inst.op == ROp::And || inst.op == ROp::Or || inst.op == ROp::Not) {
                booleans[inst.result] = inst.op == ROp::And
                    ? b.CreateAnd(booleans[inst.lhs], booleans[inst.rhs])
                    : inst.op == ROp::Or
                        ? b.CreateOr(booleans[inst.lhs], booleans[inst.rhs])
                        : b.CreateNot(booleans[inst.lhs]);
                continue;
            }
            const bool integerOperands = integral[inst.lhs] && integral[inst.rhs];
            if (inst.op == ROp::Less || inst.op == ROp::Greater || inst.op == ROp::Equal) {
                llvm::Value* lhs = integerOperands ? getInt(inst.lhs) : getDouble(inst.lhs);
                llvm::Value* rhs = integerOperands ? getInt(inst.rhs) : getDouble(inst.rhs);
                booleans[inst.result] = inst.op == ROp::Less
                    ? (integerOperands ? b.CreateICmpSLT(lhs, rhs) : b.CreateFCmpOLT(lhs, rhs))
                    : inst.op == ROp::Greater
                        ? (integerOperands ? b.CreateICmpSGT(lhs, rhs) : b.CreateFCmpOGT(lhs, rhs))
                        : (integerOperands ? b.CreateICmpEQ(lhs, rhs) : b.CreateFCmpOEQ(lhs, rhs));
                continue;
            }
            bool integerOperation = integral[inst.result] && integerOperands &&
                (inst.op == ROp::Add || inst.op == ROp::Subtract ||
                 inst.op == ROp::Multiply || inst.op == ROp::Modulo);
            if (inst.op == ROp::Modulo) {
                const auto* divisor = producers[inst.rhs];
                integerOperation = integerOperation && divisor &&
                    divisor->op == ROp::Constant && divisor->literal > 0 &&
                    plan.regionAnalysis.values[inst.lhs].lo >= 0;
            }
            llvm::Value* lhs = integerOperation ? getInt(inst.lhs) : getDouble(inst.lhs);
            llvm::Value* rhs = integerOperation ? getInt(inst.rhs) : getDouble(inst.rhs);
            llvm::Value* result = nullptr;
            switch (inst.op) {
            case ROp::Add:
                result = integerOperation ? (factGuards ? b.CreateNSWAdd(lhs, rhs) : b.CreateAdd(lhs, rhs)) : b.CreateFAdd(lhs, rhs); break;
            case ROp::Subtract:
                result = integerOperation ? (factGuards ? b.CreateNSWSub(lhs, rhs) : b.CreateSub(lhs, rhs)) : b.CreateFSub(lhs, rhs); break;
            case ROp::Multiply:
                result = integerOperation ? (factGuards ? b.CreateNSWMul(lhs, rhs) : b.CreateMul(lhs, rhs)) : b.CreateFMul(lhs, rhs); break;
            case ROp::Divide: result = b.CreateFDiv(lhs, rhs); break;
            case ROp::Modulo:
                if (integerOperation)
                    result = signedRemainder ? b.CreateSRem(lhs, rhs) : b.CreateURem(lhs, rhs);
                else {
                    llvm::Value* remainder = b.CreateFRem(lhs, rhs);
                    result = b.CreateSelect(
                        b.CreateFCmpOLT(b.CreateFDiv(remainder, rhs), cf64(0.0)),
                        b.CreateFAdd(remainder, rhs), remainder);
                }
                break;
            default: llvm_unreachable("verified mixed numerical operation");
            }
            if (integerOperation) values[inst.result].integer = result;
            else {
                values[inst.result].number = result;
                // The proof is conditional on all earlier unproven producers
                // keeping their guards. Unguarded float inputs may be Infinity;
                // their hypothetical integer-analysis bounds are NOT used.
                const bool proven = factGuards && floatFacts.size() == values.size() &&
                    floatFacts[inst.result].resultNonNaN;
                if (proven && inst.op != ROp::Modulo) {
                    if (auto* instruction = llvm::dyn_cast<llvm::Instruction>(result))
                        instruction->setHasNoNaNs(true);
                }
                if (!proven) {
                    BasicBlock* next = newBB("mixed.non_nan");
                    if (!bail) bail = newBB("rep.mixed.bail");
                    b.CreateCondBr(b.CreateFCmpORD(result, result), next, bail);
                    b.SetInsertPoint(next);
                }
            }
        }
        ends[blockId] = b.GetInsertBlock();
        const auto& term = graph.ssa.cfg.block(blockId).terminator;
        switch (term.kind) {
        case ir::TerminatorKind::Jump: b.CreateBr(blocks[term.trueBlock]); break;
        case ir::TerminatorKind::Branch:
            b.CreateCondBr(booleans[term.condition], blocks[term.trueBlock], blocks[term.falseBlock]); break;
        case ir::TerminatorKind::Return: b.CreateBr(tail); break;
        default: llvm_unreachable("verified mixed numerical terminator");
        }
    }
    // A Phi's natural type follows its own proof, not its predecessors'.
    // Materialize each incoming conversion in the actual predecessor end,
    // after any NaN-guard split but BEFORE its existing branch terminator.
    for (size_t id = 0; id < graphPhis.size(); ++id) {
        if (!graphPhis[id]) continue;
        const auto& def = graph.ssa.definitions[id];
        for (const auto& edge : graph.ssa.cfg.block(def.block).phis[def.phiIndex].inputs) {
            BasicBlock* predecessor = ends[edge.predecessor];
            b.SetInsertPoint(predecessor->getTerminator());
            llvm::Value* incoming = def.type == ir::Type::I1 ? booleans[edge.value]
                : integral[id] ? getInt(edge.value) : getDouble(edge.value);
            graphPhis[id]->addIncoming(incoming, predecessor);
        }
    }
    const auto publish = [&](const std::vector<llvm::Value*>& state) {
        for (size_t slot = 0; slot < state.size(); ++slot) {
            if (!graph.written[slot]) continue;
            llvm::Value* number = integerSlots[slot] ? b.CreateSIToFP(state[slot], f64t) : state[slot];
            llvm::Value* bits = b.CreateBitCast(number, i64t);
            const auto* field = plan.variables[slot].field;
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
    for (size_t slot = 0; slot < graph.outputs.size(); ++slot)
        output.push_back(integerSlots[slot] ? getInt(graph.outputs[slot]) : getDouble(graph.outputs[slot]));
    llvm::Value* nextIteration = b.CreateAdd(iteration, ci64(1));
    iteration->addIncoming(nextIteration, tail);
    for (size_t slot = 0; slot < carries.size(); ++slot)
        carries[slot]->addIncoming(output[slot], tail);
    b.CreateCondBr(b.CreateICmpULT(nextIteration, chunk), head, done);
    b.SetInsertPoint(done);
    publish(output);
    b.CreateStore(b.CreateSub(remaining32, b.CreateTrunc(chunk, i32t)), countdown);
    emitSsaChunkScheduler(ticks, chunk, untilPoll, true);
    b.CreateBr(loopHead);
    b.SetInsertPoint(slow);
}
