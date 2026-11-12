// Strictly guarded parity/affine integer recurrence inside repeat-until.
//
// Recognized pattern (no benchmark-name dependency):
//
// until n == stop:
//     if n % divisor == 0:
//         set n = n / divisor
//     else:
//         set n = n * multiplier + offset
//     change accumulator by step
//
// The native loop is bounded to the ordinary warp scheduler's 64-tick
// checkpoint. Each numerical step is exact and monotone in the accumulator.
// If the next recurrence is outside the exact-positive integer domain,
// publish previous complete iterations and execute the rejected iteration
// using the unmodified Scratch body (no speculative effects).
struct AffineUntilPlan {
    const BlockField* number = nullptr;
    const BlockField* accumulator = nullptr;
    uint64_t stop = 1;
    uint64_t divisor = 2;
    uint64_t multiplier = 3;
    uint64_t offset = 1;
    uint64_t accumulatorDelta = 1;
};

std::optional<AffineUntilPlan> affineUntilPlan(Block* loop) {
    if (!loop || loop->opcode != "control_repeat_until" ||
        runtime.blockCountEnabled || std::getenv("XYO_PROF") ||
        !curFnWarp)
        return std::nullopt;
    const char* enabled = std::getenv("XYO_JIT_AFFINE_UNTIL");
    // The matcher requires a pure recurrence and the runtime entry/step
    // guards ensure that every accepted integer remains IEEE-754 exact.
    if (enabled && enabled[0] == '0') return std::nullopt;
    auto getOperand = [&](Block* parent, const char* input) -> Block* {
        return parent ? transientChild(parent, input) : nullptr;
    };
    auto numericLiteral = [&](Block* parent, const char* input)
        -> std::optional<uint64_t> {
        auto n = literalNumber(parent, input);
        if (!n || !std::isfinite(*n) || *n < 0.0 ||
            *n > 65536.0 || std::trunc(*n) != *n ||
            std::signbit(*n))
            return std::nullopt;
        return uint64_t(*n);
    };
    auto equalFields = [&](const BlockField* a, const BlockField* b) {
        return a && b && !a->id.empty() && !b->id.empty() &&
               a->id == b->id && sameNamedField(a, b);
    };
    auto scalarField = [&](const BlockField* f) {
        if (!f || f->id.empty() || !target) return false;
        const auto* var = target->lookupVariableById(f->id);
        return var && var->type == Variable::kScalarType;
    };
    AffineUntilPlan plan;
    Block* stopTest = getOperand(loop, "CONDITION");
    if (!stopTest || stopTest->opcode != "operator_equals")
        return std::nullopt;
    for (auto [variable, constant] :
         {std::pair{"OPERAND1", "OPERAND2"},
          std::pair{"OPERAND2", "OPERAND1"}}) {
        if (const auto* f = directVariableInput(stopTest, variable)) {
            if (auto n = numericLiteral(stopTest, constant)) {
                plan.number = f;
                plan.stop = *n;
                break;
            }
        }
    }
    if (!scalarField(plan.number)) return std::nullopt;

    Block* branch = getOperand(loop, "SUBSTACK");
    if (!branch || branch->opcode != "control_if_else")
        return std::nullopt;
    Block* condition = getOperand(branch, "CONDITION");
    if (!condition || condition->opcode != "operator_equals")
        return std::nullopt;
    Block* modulo = nullptr;
    bool zeroRemainder = false;
    for (auto [variable, constant] :
         {std::pair{"OPERAND1", "OPERAND2"},
          std::pair{"OPERAND2", "OPERAND1"}}) {
        Block* test = getOperand(condition, variable);
        if (test && test->opcode == "operator_mod") {
            auto remainder = numericLiteral(condition, constant);
            if (remainder && *remainder == 0) {
                modulo = test;
                zeroRemainder = true;
                break;
            }
        }
    }
    if (!zeroRemainder ||
        !equalFields(directVariableInput(modulo, "NUM1"),
                     plan.number))
        return std::nullopt;
    auto modulus = numericLiteral(modulo, "NUM2");
    if (!modulus || *modulus < 2 || *modulus > 65536)
        return std::nullopt;
    plan.divisor = *modulus;

    Block* divisible = getOperand(branch, "SUBSTACK");
    Block* otherwise = getOperand(branch, "SUBSTACK2");
    if (!divisible || !otherwise || divisible->next.size() ||
        otherwise->next.size() ||
        divisible->opcode != "data_setvariableto" ||
        otherwise->opcode != "data_setvariableto" ||
        !equalFields(findField(divisible, "VARIABLE"), plan.number) ||
        !equalFields(findField(otherwise, "VARIABLE"), plan.number))
        return std::nullopt;

    Block* division = getOperand(divisible, "VALUE");
    if (!division || division->opcode != "operator_divide" ||
        !equalFields(directVariableInput(division, "NUM1"),
                     plan.number))
        return std::nullopt;
    auto divisor = numericLiteral(division, "NUM2");
    if (!divisor || *divisor != plan.divisor)
        return std::nullopt;

    Block* addition = getOperand(otherwise, "VALUE");
    if (!addition || addition->opcode != "operator_add")
        return std::nullopt;
    Block* multiplication = nullptr;
    for (auto [variable, constant] :
         {std::pair{"NUM1", "NUM2"}, std::pair{"NUM2", "NUM1"}}) {
        auto* candidate = getOperand(addition, variable);
        if (candidate && candidate->opcode == "operator_multiply") {
            if (auto offset = numericLiteral(addition, constant)) {
                multiplication = candidate;
                plan.offset = *offset;
                break;
            }
        }
    }
    if (!multiplication) return std::nullopt;
    bool foundFactor = false;
    for (auto [variable, constant] :
         {std::pair{"NUM1", "NUM2"}, std::pair{"NUM2", "NUM1"}}) {
        if (!equalFields(directVariableInput(multiplication, variable),
                         plan.number))
            continue;
        if (auto factor = numericLiteral(multiplication, constant)) {
            if (*factor < 1 || *factor > 1024) return std::nullopt;
            plan.multiplier = *factor;
            foundFactor = true;
            break;
        }
    }
    if (!foundFactor) return std::nullopt;

    Block* increment = getBlock(branch->next);
    if (!increment || !increment->next.empty() ||
        increment->opcode != "data_changevariableby")
        return std::nullopt;
    plan.accumulator = findField(increment, "VARIABLE");
    if (!scalarField(plan.accumulator) ||
        equalFields(plan.accumulator, plan.number) ||
        sameNamedField(plan.accumulator, plan.number))
        return std::nullopt;
    auto delta = numericLiteral(increment, "VALUE");
    if (!delta || *delta == 0) return std::nullopt;
    plan.accumulatorDelta = *delta;
    if (verbose)
        std::fprintf(stderr,
            "xyo-jit: affine until SSA %s modulo=%llu multiplier=%llu\n",
            loop->id.c_str(),
            (unsigned long long)plan.divisor,
            (unsigned long long)plan.multiplier);
    return plan;
}

void emitAffineUntilChunk(const AffineUntilPlan& plan,
                          BasicBlock* loopHead) {
    BasicBlock* fast = newBB("until.affine.fast");
    BasicBlock* slow = newBB("until.affine.slow");
    llvm::Value* nBits = emitVarGet(plan.number);
    llvm::Value* accBits = emitVarGet(plan.accumulator);
    llvm::Value* nF = b.CreateBitCast(nBits, f64t);
    llvm::Value* accF = b.CreateBitCast(accBits, f64t);
    constexpr double kEntry = 1099511627776.0;  // 2^40
    constexpr double kExact = 281474976710656.0; // 2^48
    auto guardedInteger = [&](llvm::Value* bits, llvm::Value* fp,
                              bool requirePositive) {
        llvm::Value* eligible = b.CreateICmpSLT(bits, ci64(val::kNan));
        eligible = b.CreateAnd(eligible,
            b.CreateFCmpOGE(fp, cf64(requirePositive ? 1.0 : 0.0)));
        eligible = b.CreateAnd(eligible,
            b.CreateFCmpOLE(fp, cf64(kEntry)));
        eligible = b.CreateAnd(eligible, b.CreateFCmpOEQ(
            fp, b.CreateUnaryIntrinsic(Intrinsic::trunc, fp)));
        eligible = b.CreateAnd(eligible, b.CreateICmpNE(
            bits, ci64(0x8000000000000000ull)));
        return eligible;
    };
    llvm::Value* valid = b.CreateAnd(
        b.CreateICmpNE(curYieldWarp, ci32(0)),
        b.CreateAnd(guardedInteger(nBits, nF, true),
                    guardedInteger(accBits, accF, false)));
    llvm::Value* initialN = b.CreateFPToUI(
        b.CreateSelect(valid, nF, cf64(1.0)), i64t);
    llvm::Value* initialAcc = b.CreateFPToUI(
        b.CreateSelect(valid, accF, cf64(0.0)), i64t);
    b.CreateCondBr(valid, fast, slow);
    b.SetInsertPoint(fast);

    llvm::Value* ticks = b.CreateLoad(i64t, curYieldCnt);
    llvm::Value* toPoll = yieldToPoll(ticks);
    BasicBlock* entry = b.GetInsertBlock();
    BasicBlock* nativeHead = newBB("until.affine.head");
    BasicBlock* nativeTail = newBB("until.affine.tail");
    BasicBlock* bail = newBB("until.affine.bail");
    BasicBlock* done = newBB("until.affine.done");
    b.CreateBr(nativeHead);
    b.SetInsertPoint(nativeHead);
    auto* iteration = b.CreatePHI(i64t, 2, "until.affine.iter");
    auto* n = b.CreatePHI(i64t, 2, "until.affine.n");
    auto* acc = b.CreatePHI(i64t, 2, "until.affine.acc");
    iteration->addIncoming(ci64(0), entry);
    n->addIncoming(initialN, entry);
    acc->addIncoming(initialAcc, entry);

    llvm::Value* rem = b.CreateURem(n, ci64(plan.divisor));
    llvm::Value* divisible = b.CreateICmpEQ(rem, ci64(0));
    llvm::Value* reduced = b.CreateUDiv(n, ci64(plan.divisor));
    // Bound arithmetic before the result is used in FP form. All accepted
    // n are <=2^48 and multiplier<=1024 so no unsigned i64 overflow.
    llvm::Value* grown = b.CreateAdd(
        b.CreateMul(n, ci64(plan.multiplier)), ci64(plan.offset));
    llvm::Value* nextN = b.CreateSelect(divisible, reduced, grown);
    llvm::Value* nextAcc = b.CreateAdd(acc, ci64(plan.accumulatorDelta));
    llvm::Value* safe = b.CreateAnd(
        b.CreateICmpULE(nextN, ci64(uint64_t(kExact))),
        b.CreateICmpULE(nextAcc, ci64(uint64_t(kExact))));
    b.CreateCondBr(safe, nativeTail, bail);

    b.SetInsertPoint(nativeTail);
    llvm::Value* nextIteration = b.CreateAdd(iteration, ci64(1));
    iteration->addIncoming(nextIteration, nativeTail);
    n->addIncoming(nextN, nativeTail);
    acc->addIncoming(nextAcc, nativeTail);
    llvm::Value* reachedStop =
        b.CreateICmpEQ(nextN, ci64(plan.stop));
    llvm::Value* reachedPoll =
        b.CreateICmpEQ(nextIteration, toPoll);
    b.CreateCondBr(b.CreateOr(reachedStop, reachedPoll), done, nativeHead);

    const auto writeValues = [&](llvm::Value* nValue,
                                 llvm::Value* accValue) {
        llvm::Value* nNumber = b.CreateUIToFP(nValue, f64t);
        llvm::Value* accNumber = b.CreateUIToFP(accValue, f64t);
        llvm::Value* nBox = b.CreateBitCast(nNumber, i64t);
        llvm::Value* accBox = b.CreateBitCast(accNumber, i64t);
        if (PromotedVar* p = findPromoted(plan.number); p && p->numeric)
            storeNumericPair(*p, nBox, nNumber, b.getInt1(true));
        else
            emitVarSet(plan.number, nBox);
        if (PromotedVar* p = findPromoted(plan.accumulator); p && p->numeric)
            storeNumericPair(*p, accBox, accNumber, b.getInt1(true));
        else
            emitVarSet(plan.accumulator, accBox);
    };
    b.SetInsertPoint(bail);
    // Speculative arithmetic for the current iteration was rejected; only
    // completed earlier iterations are visible. The original body then
    // executes this one iteration and performs its own scheduler accounting.
    writeValues(n, acc);
    b.CreateStore(b.CreateAdd(ticks, iteration), curYieldCnt);
    b.CreateBr(slow);

    b.SetInsertPoint(done);
    writeValues(nextN, nextAcc);
    emitSsaChunkScheduler(ticks, nextIteration, toPoll, true);
    b.CreateBr(loopHead);
    b.SetInsertPoint(slow);
}
