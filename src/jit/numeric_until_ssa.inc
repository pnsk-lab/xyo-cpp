// Guarded numeric-SSA micro-loop for side-effect-free Scratch "repeat until".
//
// This path reuses the exact arithmetic lowering from numeric_repeat_ssa.inc.
// It optimizes only loops whose condition and body can be evaluated from
// private SSA variables without invoking any Scratch reporter with effects.
// Each chunk publishes its state before the normal 64-iteration warp poll.
// Boolean conditions are recomputed by the original JIT loop at the end of
// each chunk, preserving any deoptimization or scheduler handoff.
struct NumericUntilNode {
    enum class Kind {
        Variable, Literal, Add, Sub, Mul, Div, Mod,
        Eq, Lt, Gt, Not, And, Or
    } kind = Kind::Literal;
    int lhs = -1, rhs = -1;
    unsigned variable = 0;
    double literal = 0.0;
};
struct NumericUntilPlan {
    NumericRepeatPlan body;
    std::vector<NumericUntilNode> nodes;
    int predicate = -1;
};

std::optional<NumericUntilPlan> numericUntilPlan(Block* loop) {
    if (!loop || loop->opcode != "control_repeat_until")
        return std::nullopt;
    const char* enabled = std::getenv("XYO_JIT_NUMERIC_UNTIL_SSA");
    // Default to the strictly parsed, runtime-guarded path. A single
    // environment switch can restore the original compiler at any time.
    if ((enabled && enabled[0] == '0') || runtime.blockCountEnabled ||
        std::getenv("XYO_PROF") || !curFnWarp)
        return std::nullopt;

    auto body = numericRepeatPlan(loop, true);
    if (!body) return std::nullopt;
    // Keep the fast floating path limited to arithmetic whose exact
    // IEEE-754 double result matches ordinary Scratch arithmetic whenever
    // no operation returns NaN. NaN triggers a pre-iteration rollback.
    const char* fast = std::getenv("XYO_JIT_NUMERIC_UNTIL_FAST");
    if ((!fast || fast[0] != '0') &&
        std::any_of(body->exprs.begin(), body->exprs.end(),
                    [](const auto& e) {
                        return e.kind == NumericRepeatExpr::Kind::Mod;
                    }))
        return std::nullopt;
    // Integer-specific repeat proofs concern a *fixed* count and do not
    // establish correctness for an until predicate with dynamic trip count.
    body->integerVars.clear();
    NumericUntilPlan plan{std::move(*body), {}, -1};

    std::function<std::optional<int>(Block*, bool, unsigned)> parse;
    parse = [&](Block* node, bool boolean, unsigned depth)
        -> std::optional<int> {
        if (!node || depth > 12 || plan.nodes.size() >= 96)
            return std::nullopt;
        using K = NumericUntilNode::Kind;
        NumericUntilNode e;
        if (!boolean && node->opcode == "data_variable") {
            const auto* field = findField(node, "VARIABLE");
            if (!field || field->id.empty()) return std::nullopt;
            unsigned index = 0;
            for (; index < plan.body.variables.size(); ++index) {
                const auto* existing = plan.body.variables[index].field;
                if (existing->id == field->id) {
                    if (!sameNamedField(existing, field))
                        return std::nullopt;
                    break;
                }
                if (sameNamedField(existing, field))
                    return std::nullopt;
            }
            if (index == plan.body.variables.size())
                return std::nullopt;
            e.kind = K::Variable;
            e.variable = index;
        } else if (!boolean && node->shadow &&
                   (node->opcode == "math_number" ||
                    node->opcode == "math_integer" ||
                    node->opcode == "math_whole_number" ||
                    node->opcode == "math_positive_number" ||
                    node->opcode == "math_angle") &&
                   node->fields.size() == 1) {
            const auto& field = node->fields.begin()->second;
            const auto raw = field.value;
            const double number = cast::toNumberOrNaN(raw);
            if (!std::isfinite(number) || cast::isWhiteSpace(raw))
                return std::nullopt;
            e.kind = K::Literal;
            e.literal = number;
        } else {
            const auto& op = node->opcode;
            const char* left = nullptr;
            const char* right = nullptr;
            bool childBoolean = false;
            if (!boolean) {
                if (op == "operator_add") e.kind = K::Add;
                else if (op == "operator_subtract") e.kind = K::Sub;
                else if (op == "operator_multiply") e.kind = K::Mul;
                else if (op == "operator_divide") e.kind = K::Div;
                // A numeric modulo inside a comparison needs Scratch
                // remainder/NaN semantics, not a raw LLVM FRem.
                else if (op == "operator_mod") return std::nullopt;
                else return std::nullopt;
                left = "NUM1"; right = "NUM2";
            } else {
                if (op == "operator_equals") e.kind = K::Eq;
                else if (op == "operator_lt") e.kind = K::Lt;
                else if (op == "operator_gt") e.kind = K::Gt;
                else if (op == "operator_not") {
                    e.kind = K::Not;
                    left = "OPERAND";
                    childBoolean = true;
                } else if (op == "operator_and") {
                    e.kind = K::And; childBoolean = true;
                } else if (op == "operator_or") {
                    e.kind = K::Or; childBoolean = true;
                } else return std::nullopt;
                if (!left) {
                    left = "OPERAND1"; right = "OPERAND2";
                }
            }
            auto first = parse(transientChild(node, left),
                               childBoolean, depth + 1);
            if (!first) return std::nullopt;
            e.lhs = *first;
            if (right) {
                auto second = parse(transientChild(node, right),
                                    childBoolean, depth + 1);
                if (!second) return std::nullopt;
                e.rhs = *second;
            }
        }
        plan.nodes.push_back(e);
        return int(plan.nodes.size() - 1);
    };
    plan.predicate = parse(transientChild(loop, "CONDITION"), true, 0)
                         .value_or(-1);
    if (plan.predicate < 0) return std::nullopt;
    if (verbose)
        std::fprintf(stderr, "xyo-jit: numeric until SSA %s vars=%zu body=%zu\n",
                     loop->id.c_str(), plan.body.variables.size(),
                     plan.body.stmts.size());
    return plan;
}

// The numerical predicate carries an additional "safe" bit. If any operand
// is NaN (or a non-number produced by a calculation), the original Scratch
// comparison must handle its string fallback, so the native chunk exits and
// lets the regular JIT re-evaluate the same side-effect-free predicate.
struct NumericUntilEval {
    llvm::Value* value;
    llvm::Value* safe;
};
NumericUntilEval emitUntilNumerical(
    const NumericUntilPlan& plan, int index,
    const std::vector<NativeNumericValue>& values) {
    using K = NumericUntilNode::Kind;
    const auto& e = plan.nodes[size_t(index)];
    if (e.kind == K::Literal)
        return {cf64(e.literal), b.getInt1(true)};
    if (e.kind == K::Variable) {
        const auto& v = values[e.variable];
        return {v.number, b.CreateFCmpORD(
            b.CreateBitCast(v.bits, f64t),
            b.CreateBitCast(v.bits, f64t))};
    }
    NumericUntilEval lhs = emitUntilNumerical(plan, e.lhs, values);
    NumericUntilEval rhs = emitUntilNumerical(plan, e.rhs, values);
    llvm::Value* number;
    if (e.kind == K::Add) number = b.CreateFAdd(lhs.value, rhs.value);
    else if (e.kind == K::Sub) number = b.CreateFSub(lhs.value, rhs.value);
    else if (e.kind == K::Mul) number = b.CreateFMul(lhs.value, rhs.value);
    else if (e.kind == K::Div) number = b.CreateFDiv(lhs.value, rhs.value);
    else {
        // Modulo needs its Scratch signed remainder semantics; refuse the
        // optimization for modulo-bearing conditions until a shared lowering
        // is available here. The parser can still safely reject it.
        number = cf64(std::numeric_limits<double>::quiet_NaN());
    }
    return {number, b.CreateAnd(
        lhs.safe, b.CreateAnd(rhs.safe,
        b.CreateFCmpORD(number, number)))};
}
NumericUntilEval emitUntilBoolean(
    const NumericUntilPlan& plan, int index,
    const std::vector<NativeNumericValue>& values) {
    using K = NumericUntilNode::Kind;
    const auto& e = plan.nodes[size_t(index)];
    if (e.kind == K::Not) {
        auto child = emitUntilBoolean(plan, e.lhs, values);
        return {b.CreateNot(child.value), child.safe};
    }
    if (e.kind == K::And || e.kind == K::Or) {
        auto left = emitUntilBoolean(plan, e.lhs, values);
        auto right = emitUntilBoolean(plan, e.rhs, values);
        llvm::Value* result = e.kind == K::And
            ? b.CreateAnd(left.value, right.value)
            : b.CreateOr(left.value, right.value);
        return {result, b.CreateAnd(left.safe, right.safe)};
    }
    auto lhs = emitUntilNumerical(plan, e.lhs, values);
    auto rhs = emitUntilNumerical(plan, e.rhs, values);
    llvm::Value* pred;
    if (e.kind == K::Eq)
        pred = b.CreateFCmpOEQ(lhs.value, rhs.value);
    else if (e.kind == K::Lt)
        pred = b.CreateFCmpOLT(lhs.value, rhs.value);
    else
        pred = b.CreateFCmpOGT(lhs.value, rhs.value);
    return {pred, b.CreateAnd(lhs.safe, rhs.safe)};
}

// Optimistic plain f64 expression lowering. Every intermediate that
// produces NaN makes this iteration ineligible; the caller then flushes
// the PRE-iteration SSA state and executes this one iteration through the
// unmodified Scratch JIT body, preserving all coercion/canonicalization.
llvm::Value* emitNativeUntilFloat(
    const NumericRepeatPlan& body, int index,
    const std::vector<NativeNumericValue>& state, llvm::Value*& safe) {
    using K = NumericRepeatExpr::Kind;
    const auto& e = body.exprs[size_t(index)];
    if (e.kind == K::Variable) return state[e.variable].number;
    if (e.kind == K::Number) return cf64(e.number);
    llvm::Value* a = emitNativeUntilFloat(body, e.left, state, safe);
    llvm::Value* result = nullptr;
    if (e.kind == K::Sqrt || e.kind == K::MathOp)
        result = emitMathop(e.kind == K::Sqrt ? 3 : e.mathFn, a);
    else if (e.kind == K::Round)
        result = emitJsRound(a);
    else {
        llvm::Value* rhs = emitNativeUntilFloat(body, e.right, state, safe);
        if (e.kind == K::Add) result = b.CreateFAdd(a, rhs);
        else if (e.kind == K::Sub) result = b.CreateFSub(a, rhs);
        else if (e.kind == K::Mul) result = b.CreateFMul(a, rhs);
        else if (e.kind == K::Div) result = b.CreateFDiv(a, rhs);
        // Modulo's Scratch sign-of-zero and number coercion semantics need
        // the original box-preserving lowering. This fast mode is disabled
        // by the parser when a modulo occurs in the body.
        else return cf64(std::numeric_limits<double>::quiet_NaN());
    }
    safe = b.CreateAnd(safe, b.CreateFCmpORD(result, result));
    return result;
}

void emitNumericUntilChunk(const NumericUntilPlan& plan,
                           BasicBlock* loopHead) {
    BasicBlock* fast = newBB("until.num.fast");
    BasicBlock* slow = newBB("until.num.slow");
    llvm::Value* valid = b.CreateICmpNE(curYieldWarp, ci32(0));
    std::vector<NativeNumericValue> initial;
    for (const auto& variable : plan.body.variables) {
        llvm::Value* bits = emitVarGet(variable.field);
        llvm::Value* value = b.CreateBitCast(bits, f64t);
        llvm::Value* plain = b.CreateICmpSLT(bits, ci64(val::kNan));
        valid = b.CreateAnd(valid,
            b.CreateAnd(plain, b.CreateFCmpORD(value, value)));
        initial.push_back({bits, value, nullptr});
    }
    b.CreateCondBr(valid, fast, slow);
    b.SetInsertPoint(fast);
    llvm::Value* ticks = b.CreateLoad(i64t, curYieldCnt);
    llvm::Value* toPoll = b.CreateSub(
        ci64(64), b.CreateAnd(ticks, ci64(63)));

    BasicBlock* preheader = b.GetInsertBlock();
    BasicBlock* nativeHead = newBB("until.num.head");
    BasicBlock* nativeExit = newBB("until.num.exit");
    b.CreateBr(nativeHead);
    b.SetInsertPoint(nativeHead);
    auto* iteration = b.CreatePHI(i64t, 2, "until.num.iter");
    iteration->addIncoming(ci64(0), preheader);

    std::vector<llvm::PHINode*> boxed, numerics;
    std::vector<NativeNumericValue> state;
    for (const auto& initialValue : initial) {
        auto* box = b.CreatePHI(i64t, 2, "until.num.box");
        auto* num = b.CreatePHI(f64t, 2, "until.num.value");
        box->addIncoming(initialValue.bits, preheader);
        num->addIncoming(initialValue.number, preheader);
        boxed.push_back(box);
        numerics.push_back(num);
        state.push_back({box, num, nullptr});
    }

    // We already evaluated a false condition in the original loop header,
    // so one iteration of this pure body is required before another check.
    const char* fastMode = std::getenv("XYO_JIT_NUMERIC_UNTIL_FAST");
    const bool fastFloat = !fastMode || fastMode[0] != '0';
    llvm::Value* safeIteration = b.getInt1(true);
    for (const auto& stmt : plan.body.stmts) {
        NativeNumericValue result;
        if (fastFloat) {
            llvm::Value* number = emitNativeUntilFloat(
                plan.body, stmt.expression, state, safeIteration);
            if (stmt.change) {
                number = b.CreateFAdd(state[stmt.variable].number, number);
                safeIteration = b.CreateAnd(
                    safeIteration, b.CreateFCmpORD(number, number));
            }
            result = {b.CreateBitCast(number, i64t), number, nullptr};
        } else {
            result = emitNumericRepeatExpr(
                plan.body, stmt.expression, state, false);
            if (stmt.change) {
                llvm::Value* bits = emitArithNumeric(
                    state[stmt.variable].number, result.number, AOp::Add);
                result = {bits, emitToNumF64(bits), nullptr};
            }
        }
        state[stmt.variable] = result;
    }
    if (fastFloat) {
        // On an unsafe arithmetic result do not execute or publish any part
        // of the current iteration. Restore the pre-iteration state (the
        // PHIs) and execute the normal body once through the slow path.
        BasicBlock* good = newBB("until.num.float.good");
        BasicBlock* rollback = newBB("until.num.float.rollback");
        b.CreateCondBr(safeIteration, good, rollback);
        b.SetInsertPoint(rollback);
        for (size_t i = 0; i < state.size(); ++i) {
            if (!plan.body.variables[i].written) continue;
            const auto* field = plan.body.variables[i].field;
            llvm::Value* old = boxed[i];
            if (PromotedVar* p = findPromoted(field); p && p->numeric) {
                storeNumericPair(*p, old, numerics[i], b.getInt1(true));
            } else {
                emitVarSet(field, old);
            }
        }
        // The rejected iteration has not yet run. Count only previously
        // completed native iterations; the slow body calls emitLoopYield().
        b.CreateStore(b.CreateAdd(ticks, iteration), curYieldCnt);
        b.CreateBr(slow);
        b.SetInsertPoint(good);
    }
    auto condition = emitUntilBoolean(plan, plan.predicate, state);
    llvm::Value* finished = b.CreateOr(
        condition.value, b.CreateNot(condition.safe));
    llvm::Value* completed = b.CreateAdd(iteration, ci64(1));
    BasicBlock* tail = b.GetInsertBlock();
    iteration->addIncoming(completed, tail);
    for (size_t i = 0; i < boxed.size(); ++i) {
        boxed[i]->addIncoming(state[i].bits, tail);
        numerics[i]->addIncoming(state[i].number, tail);
    }
    llvm::Value* atPoll = b.CreateICmpEQ(completed, toPoll);
    b.CreateCondBr(b.CreateOr(finished, atPoll), nativeExit, nativeHead);

    b.SetInsertPoint(nativeExit);
    for (size_t i = 0; i < state.size(); ++i) {
        if (!plan.body.variables[i].written) continue;
        const auto* field = plan.body.variables[i].field;
        if (PromotedVar* p = findPromoted(field); p && p->numeric) {
            llvm::Value* ok = b.CreateFCmpORD(
                b.CreateBitCast(state[i].bits, f64t),
                b.CreateBitCast(state[i].bits, f64t));
            storeNumericPair(
                *p, state[i].bits, state[i].number, ok);
        } else {
            emitVarSet(field, state[i].bits);
        }
    }
    emitSsaChunkScheduler(ticks, completed, toPoll, true);
    b.CreateBr(loopHead);
    b.SetInsertPoint(slow);
}
