#include "numeric_repeat_ssa.inc"

// Guarded integer SSA for pure conditional Scratch repeat bodies.
// No scheduling, conversion or variable writes happen within the native
// micro-loop; all observable operations remain at the original yield points.
struct ConditionalRepeatExpr {
    enum class Kind { Variable, Constant, Add, Sub, Mod, Eq, Lt, Gt, And, Or, Not };
    Kind kind;
    int lhs = -1;
    int rhs = -1;
    int64_t value = 0;
};
struct ConditionalRepeatStmt {
    enum class Kind { Change, Set, If };
    Kind kind;
    unsigned variable = 0;
    unsigned source = 0;
    int64_t delta = 0;
    int condition = -1;
    std::vector<ConditionalRepeatStmt> yes, no;
};
struct ConditionalRepeatVar {
    const BlockField* field;
    bool written = false;
};
struct ConditionalRepeatPlan {
    std::vector<ConditionalRepeatVar> vars;
    std::vector<ConditionalRepeatExpr> exprs;
    std::vector<ConditionalRepeatStmt> stmts;
    std::optional<NumericRepeatPlan> numeric;
    // Entry guards plus monotone writes prove nonnegative modulo operands.
    bool positiveModulo = false;
    // MATH-019: i32 is safe only with the tighter entry range and a
    // statically bounded expression tree across all 64 native iterations.
    bool integer32State = false;
};

std::optional<ConditionalRepeatPlan> conditionalRepeatPlan(Block* loop) {
    const char* enabled = std::getenv("XYO_JIT_CONDITIONAL_SSA");
    // Default ON only for this statically restricted, runtime-guarded
    // arithmetic-only repeat shape. Deoptimization and exact block-count
    // runs retain the existing interpreter/JIT fallback. Keep an opt-out
    // for per-project A/B tests and unexpected regression diagnostics.
    if (enabled && enabled[0] == '0') return std::nullopt;
    // The fast chunk bypasses individual Scratch reporter/stack instructions.
    // Exact block counting and per-block profiling require the baseline path.
    if (runtime.blockCountEnabled || std::getenv("XYO_PROF"))
        return std::nullopt;
    const BlockInput* body = loop->inputs.find("SUBSTACK");
    if (!body || body->block.empty()) return std::nullopt;
    // Pure arithmetic repeats reuse the same per-chunk scheduler checkpoint
    // without changing the primary LLVM codegen module.
    if (auto numeric = numericRepeatPlan(loop)) {
        ConditionalRepeatPlan selected;
        selected.numeric = std::move(numeric);
        return selected;
    }
    ConditionalRepeatPlan plan;
    bool hasConditional = false;
    unsigned statementCount = 0;
    auto resolve = [&](const BlockField* field) -> std::optional<unsigned> {
        if (!field || field->id.empty() || !field->value.isString() ||
            !field->value.asStrPtr()) return std::nullopt;
        for (unsigned i = 0; i < plan.vars.size(); ++i) {
            const auto* old = plan.vars[i].field;
            if (old->id == field->id)
                return sameNamedField(old, field) ? std::optional(i) : std::nullopt;
            if (sameNamedField(old, field)) return std::nullopt;
        }
        if (plan.vars.size() >= 8) return std::nullopt;
        plan.vars.push_back({field, false});
        return unsigned(plan.vars.size() - 1);
    };
    // The expression grammar is deliberately side-effect free.  All
    // arithmetic operands are integral, and modulo uses a positive literal
    // divisor, so neither rounding nor the sign of an intermediate -0 can
    // affect a predicate.
    std::function<std::optional<int>(Block*, bool, unsigned)> expr;
    expr = [&](Block* node, bool boolean, unsigned depth) -> std::optional<int> {
        if (!node || depth > 8 || plan.exprs.size() >= 48) return std::nullopt;
        using K = ConditionalRepeatExpr::Kind;
        ConditionalRepeatExpr e{K::Constant};
        if (!boolean && node->opcode == "data_variable") {
            auto v = resolve(findField(node, "VARIABLE"));
            if (!v) return std::nullopt;
            e.kind = K::Variable;
            e.value = *v;
        } else if (!boolean && node->shadow &&
                   (node->opcode == "math_number" ||
                    node->opcode == "math_integer" ||
                    node->opcode == "math_whole_number" ||
                    node->opcode == "math_positive_number" ||
                    node->opcode == "math_angle") && node->fields.size() == 1) {
            const Value raw = node->fields.begin()->second.value;
            const double n = cast::toNumberOrNaN(raw);
            if (cast::isWhiteSpace(raw) || !std::isfinite(n) ||
                std::trunc(n) != n || std::abs(n) > 65536.0)
                return std::nullopt;
            e.value = int64_t(n);
        } else {
            const auto& op = node->opcode;
            const char* leftName = nullptr;
            const char* rightName = nullptr;
            bool operandBoolean = false;
            if (!boolean && (op == "operator_add" ||
                             op == "operator_subtract" ||
                             op == "operator_mod")) {
                e.kind = op == "operator_add" ? K::Add
                    : op == "operator_subtract" ? K::Sub : K::Mod;
                leftName = "NUM1"; rightName = "NUM2";
            } else if (boolean && (op == "operator_equals" ||
                                   op == "operator_lt" ||
                                   op == "operator_gt")) {
                e.kind = op == "operator_equals" ? K::Eq
                    : op == "operator_lt" ? K::Lt : K::Gt;
                leftName = "OPERAND1"; rightName = "OPERAND2";
            } else if (boolean && (op == "operator_and" ||
                                   op == "operator_or")) {
                e.kind = op == "operator_and" ? K::And : K::Or;
                leftName = "OPERAND1"; rightName = "OPERAND2";
                operandBoolean = true;
            } else if (boolean && op == "operator_not") {
                e.kind = K::Not;
                leftName = "OPERAND";
                operandBoolean = true;
            } else {
                return std::nullopt;
            }
            // i64 remainder with a constant divisor is strength-reduced by
            // LLVM. Reject arbitrary divisors rather than speculating on
            // division-by-zero or Scratch's floating remainder rules.
            if (e.kind == K::Mod) {
                auto d = literalNumber(node, "NUM2");
                if (!d || *d < 1.0 || *d > 65536.0 ||
                    std::trunc(*d) != *d) return std::nullopt;
                e.value = int64_t(*d);
                rightName = nullptr;
            }
            auto lhs = expr(transientChild(node, leftName),
                            operandBoolean, depth + 1);
            if (!lhs) return std::nullopt;
            e.lhs = *lhs;
            if (rightName) {
                auto rhs = expr(transientChild(node, rightName),
                                operandBoolean, depth + 1);
                if (!rhs) return std::nullopt;
                e.rhs = *rhs;
            }
        }
        plan.exprs.push_back(e);
        return int(plan.exprs.size() - 1);
    };
    std::function<bool(Block*, std::vector<ConditionalRepeatStmt>&, unsigned)> scan;
    scan = [&](Block* current, std::vector<ConditionalRepeatStmt>& out,
               unsigned depth) -> bool {
        if (depth > 5) return false;
        while (current) {
            if (++statementCount > 24) return false;
            ConditionalRepeatStmt stmt{ConditionalRepeatStmt::Kind::Change};
            if (current->opcode == "data_changevariableby") {
                auto var = resolve(findField(current, "VARIABLE"));
                auto amount = literalNumber(current, "VALUE");
                if (!var || !amount || std::trunc(*amount) != *amount ||
                    *amount == 0 || std::abs(*amount) > 65536.0)
                    return false;
                stmt.variable = *var;
                stmt.delta = int64_t(*amount);
                plan.vars[*var].written = true;
            } else if (current->opcode == "data_setvariableto") {
                // set dst = src + K, K + src, or src - K.
                // This is the same exact integer recurrence, with ordinary
                // Scratch numeric coercion before arithmetic. Do not accept
                // a zero offset: -0 +/- 0 has observable signed-zero rules.
                Block* rhs = transientChild(current, "VALUE");
                const auto dst = resolve(findField(current, "VARIABLE"));
                if (!dst || !rhs ||
                    (rhs->opcode != "operator_add" &&
                     rhs->opcode != "operator_subtract"))
                    return false;
                const BlockField* src = nullptr;
                std::optional<double> step;
                if (auto left = directVariableInput(rhs, "NUM1")) {
                    src = left;
                    step = literalNumber(rhs, "NUM2");
                    if (step && rhs->opcode == "operator_subtract")
                        *step = -*step;
                } else if (rhs->opcode == "operator_add") {
                    src = directVariableInput(rhs, "NUM2");
                    step = literalNumber(rhs, "NUM1");
                }
                if (!src || !step || std::trunc(*step) != *step ||
                    *step == 0.0 || std::abs(*step) > 65536.0)
                    return false;
                const auto source = resolve(src);
                if (!source) return false;
                stmt.kind = ConditionalRepeatStmt::Kind::Set;
                stmt.variable = *dst;
                stmt.source = *source;
                stmt.delta = int64_t(*step);
                plan.vars[*dst].written = true;
            } else if (current->opcode == "control_if" ||
                       current->opcode == "control_if_else") {
                hasConditional = true;
                stmt.kind = ConditionalRepeatStmt::Kind::If;
                auto cond = expr(transientChild(current, "CONDITION"), true, 0);
                if (!cond) return false;
                stmt.condition = *cond;
                const BlockInput* yes = current->inputs.find("SUBSTACK");
                const BlockInput* no = current->inputs.find("SUBSTACK2");
                if (yes && !yes->block.empty() &&
                    !scan(getBlock(yes->block), stmt.yes, depth + 1))
                    return false;
                if (current->opcode == "control_if_else" &&
                    no && !no->block.empty() &&
                    !scan(getBlock(no->block), stmt.no, depth + 1))
                    return false;
            } else {
                return false;
            }
            out.push_back(std::move(stmt));
            if (current->next.empty()) break;
            current = getBlock(current->next);
        }
        return current != nullptr;
    };
    if (!scan(getBlock(body->block), plan.stmts, 0) ||
        !hasConditional || plan.vars.empty()) return std::nullopt;
    bool hasWrite = false;
    for (const auto& v : plan.vars) hasWrite |= v.written;
    if (!hasWrite) return std::nullopt;
    // All assignments in every branch must preserve the nonnegative domain.
    // A negative initial value takes the normal Scratch fallback.
    const auto nonnegativeChanges = [&](const auto& self,
                                        const auto& stmts) -> bool {
        for (const auto& stmt : stmts) {
            if (stmt.kind == ConditionalRepeatStmt::Kind::Change) {
                if (stmt.delta < 0) return false;
            } else if (stmt.kind == ConditionalRepeatStmt::Kind::If) {
                if (!self(self, stmt.yes) || !self(self, stmt.no))
                    return false;
            } else {
                return false;
            }
        }
        return true;
    };
    const char* unsignedMode = std::getenv("XYO_JIT_CONDITIONAL_UREM");
    plan.positiveModulo = (!unsignedMode || unsignedMode[0] != '0') &&
        nonnegativeChanges(nonnegativeChanges, plan.stmts) &&
        std::any_of(plan.exprs.begin(), plan.exprs.end(),
                    [](const auto& e) {
                        return e.kind == ConditionalRepeatExpr::Kind::Mod;
                    });

    // A guarded initial variable is at most 2^20 in absolute value.
    // A repeat chunk runs at most 64 times, and the parser permits at
    // most 24 statements of +/-65536 each, even across nested branches.
    // Set and change operations cannot move any scalar outside this
    // conservative bound. Every arithmetic subexpression must ALSO fit
    // signed i32; nested additions can grow faster than the variables.
    const char* narrow = std::getenv("XYO_JIT_CONDITIONAL_I32");
    // MATH-019 remains opt-in until a clean integrated CTest build is
    // available. Absent/unknown values retain the previous i64 path.
    if (narrow && narrow[0] == '1') {
        constexpr long double kI32Max = 2147483647.0L;
        constexpr long double kInitialMax = 1048576.0L;
        constexpr long double kScalarMax =
            kInitialMax + 64.0L * 24.0L * 65536.0L;
        std::vector<long double> absolute;
        absolute.reserve(plan.exprs.size());
        bool bounded = true;
        using K = ConditionalRepeatExpr::Kind;
        // Expression nodes are emitted in post-order by the parser.
        for (const auto& e : plan.exprs) {
            long double limit = 0;
            switch (e.kind) {
            case K::Variable: limit = kScalarMax; break;
            case K::Constant: limit = std::abs((long double)e.value); break;
            case K::Add: case K::Sub:
                limit = absolute[size_t(e.lhs)] + absolute[size_t(e.rhs)];
                break;
            case K::Mod: limit = (long double)e.value - 1.0L; break;
            case K::Eq: case K::Lt: case K::Gt:
            case K::And: case K::Or: case K::Not:
                limit = 1.0L; break;
            }
            if (limit > kI32Max) bounded = false;
            absolute.push_back(limit);
        }
        // The signed-i32 remainder lowering for loops that cross
        // negative values is not safe to enable yet: it exhibited
        // wrong branch selection on the adversarial -40, step -2 case.
        // Select i32 only for the fully nonnegative monotone-modulus
        // family, for which all remainder operations are unsigned.
        const auto positiveExpr = [&](const auto& self, int idx) -> bool {
            const auto& e = plan.exprs[size_t(idx)];
            switch (e.kind) {
            case K::Variable: return true;
            case K::Constant: return e.value >= 0;
            case K::Add: return self(self, e.lhs) && self(self, e.rhs);
            case K::Mod: return true;
            case K::Eq: case K::Lt: case K::Gt:
            case K::And: case K::Or: case K::Not:
                return true;
            default: return false;
            }
        };
        bool unsignedOnly = plan.positiveModulo;
        for (const auto& e : plan.exprs) {
            if (e.kind == K::Mod &&
                !positiveExpr(positiveExpr, e.lhs))
                unsignedOnly = false;
        }
        // On targets tested here, reducing PHIs to i32 is profitable
        // for flat, simple-if predicates with several constant moduli,
        // but regresses nested if/else recurrences substantially.
        // Preserve the old i64 SSA for any else branch or nested if.
        const auto flatConditional = [&]() -> bool {
            for (const auto& stmt : plan.stmts) {
                if (stmt.kind != ConditionalRepeatStmt::Kind::If)
                    continue;
                if (!stmt.no.empty()) return false;
                for (const auto& nested : stmt.yes)
                    if (nested.kind == ConditionalRepeatStmt::Kind::If)
                        return false;
            }
            return true;
        };
        plan.integer32State = bounded && unsignedOnly &&
            flatConditional();
    }
    return plan;
}

llvm::Value* emitConditionalExpr(
    const ConditionalRepeatPlan& plan, int index,
    const std::vector<llvm::Value*>& states) {
    using K = ConditionalRepeatExpr::Kind;
    const auto& e = plan.exprs[size_t(index)];
    const auto constant = [&](int64_t n) -> llvm::Value* {
        return plan.integer32State
            ? ci32(uint32_t(n)) : ci64(uint64_t(n));
    };
    auto* scalarType = plan.integer32State ? i32t : i64t;
    if (e.kind == K::Variable) return states[size_t(e.value)];
    if (e.kind == K::Constant) return constant(e.value);
    llvm::Value* a = emitConditionalExpr(plan, e.lhs, states);
    if (e.kind == K::Not) return b.CreateXor(a, constant(1));
    llvm::Value* rhs = e.kind == K::Mod ? constant(e.value)
                                      : emitConditionalExpr(plan, e.rhs, states);
    switch (e.kind) {
    case K::Add: return b.CreateNSWAdd(a, rhs);
    case K::Sub: return b.CreateNSWSub(a, rhs);
    case K::Mod: {
        // Every referenced scalar is nonnegative at chunk entry, and every
        // update monotonically increases it. Signed-zero is excluded at the
        // entry guard. Nonnegative expressions may use unsigned remainder.
        const auto nonnegative = [&](const auto& self, int node) -> bool {
            const auto& ex = plan.exprs[size_t(node)];
            switch (ex.kind) {
            case K::Variable: return true;
            case K::Constant: return ex.value >= 0;
            case K::Add: return self(self, ex.lhs) && self(self, ex.rhs);
            case K::Mod: return true;  // positive constant divisor
            case K::Eq: case K::Lt: case K::Gt:
            case K::And: case K::Or: case K::Not: return true;
            default: return false;  // subtraction may be negative
            }
        };
        if (plan.positiveModulo && nonnegative(nonnegative, e.lhs))
            return b.CreateURem(a, rhs, "rep.cond.urem");
        llvm::Value* rem = b.CreateSRem(a, rhs);
        return b.CreateSelect(b.CreateICmpSLT(rem, constant(0)),
                              b.CreateAdd(rem, rhs), rem);
    }
    case K::Eq: return b.CreateZExt(b.CreateICmpEQ(a, rhs), scalarType);
    case K::Lt: return b.CreateZExt(b.CreateICmpSLT(a, rhs), scalarType);
    case K::Gt: return b.CreateZExt(b.CreateICmpSGT(a, rhs), scalarType);
    case K::And: return b.CreateAnd(a, rhs);
    case K::Or: return b.CreateOr(a, rhs);
    default: return constant(0);
    }
}

void emitConditionalStatements(const ConditionalRepeatPlan& plan,
                               const std::vector<ConditionalRepeatStmt>& stmts,
                               std::vector<llvm::Value*>& states) {
    for (const auto& stmt : stmts) {
        if (stmt.kind == ConditionalRepeatStmt::Kind::Change ||
            stmt.kind == ConditionalRepeatStmt::Kind::Set) {
            llvm::Value* source =
                stmt.kind == ConditionalRepeatStmt::Kind::Set
                    ? states[stmt.source] : states[stmt.variable];
            states[stmt.variable] =
                b.CreateNSWAdd(source, plan.integer32State
                    ? ci32(uint32_t(stmt.delta))
                    : ci64(uint64_t(stmt.delta)));
            continue;
        }
        llvm::Value* condition = emitConditionalExpr(plan, stmt.condition, states);
        BasicBlock* yes = newBB("rep.cond.then");
        BasicBlock* no = newBB("rep.cond.else");
        BasicBlock* join = newBB("rep.cond.join");
        b.CreateCondBr(b.CreateICmpNE(condition,
            plan.integer32State ? ci32(0) : ci64(0)), yes, no);
        auto elseStates = states;
        b.SetInsertPoint(yes);
        emitConditionalStatements(plan, stmt.yes, states);
        BasicBlock* yesEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(no);
        emitConditionalStatements(plan, stmt.no, elseStates);
        BasicBlock* noEnd = b.GetInsertBlock();
        b.CreateBr(join);
        b.SetInsertPoint(join);
        for (size_t i = 0; i < states.size(); ++i) {
            if (states[i] == elseStates[i]) continue;
            auto* value = b.CreatePHI(
                plan.integer32State ? i32t : i64t, 2, "rep.cond.value");
            value->addIncoming(states[i], yesEnd);
            value->addIncoming(elseStates[i], noEnd);
            states[i] = value;
        }
    }
}

void emitConditionalRepeatChunk(const ConditionalRepeatPlan& plan,
                                llvm::Value* remaining32,
                                AllocaInst* countdown, BasicBlock* loopHead) {
    if (plan.numeric) {
        emitNumericRepeatChunk(*plan.numeric, remaining32, countdown, loopHead);
        return;
    }
    BasicBlock* fast = newBB("rep.cond.fast");
    BasicBlock* slow = newBB("rep.cond.slow");
    // ±2^40 leaves ample room for 64 iterations, 24 increments,
    // and up to 8 levels of arithmetic in the condition expressions.
    const double kLimit = plan.integer32State
        ? 1048576.0 : 1099511627776.0;
    llvm::Value* valid = b.CreateICmpNE(curYieldWarp, ci32(0));
    std::vector<llvm::Value*> initial;
    for (const auto& var : plan.vars) {
        llvm::Value* bits = emitVarGet(var.field);
        llvm::Value* plain = b.CreateICmpSLT(bits, ci64(val::kNan));
        llvm::Value* number = b.CreateBitCast(
            b.CreateSelect(plain, bits, ci64(0)), f64t);
        llvm::Value* range = b.CreateAnd(
            b.CreateFCmpOGE(number, cf64(-kLimit)),
            b.CreateFCmpOLE(number, cf64(kLimit)));
        llvm::Value* integral = b.CreateFCmpOEQ(
            number, b.CreateUnaryIntrinsic(Intrinsic::trunc, number));
        // Integer SSA cannot preserve negative zero. The exact signed-zero
        // bits of any unchanged Scratch variable must survive this chunk.
        llvm::Value* negativeZero = b.CreateICmpEQ(
            b.CreateBitCast(number, i64t), ci64(0x8000000000000000ull));
        llvm::Value* eligibility = b.CreateAnd(
            plain, b.CreateAnd(range, b.CreateAnd(
                integral, b.CreateNot(negativeZero))));
        if (plan.positiveModulo)
            eligibility = b.CreateAnd(
                eligibility, b.CreateFCmpOGE(number, cf64(0.0)));
        valid = b.CreateAnd(valid, eligibility);
        initial.push_back(b.CreateFPToSI(
            b.CreateSelect(range, number, cf64(0)),
            plan.integer32State ? i32t : i64t));
    }
    b.CreateCondBr(valid, fast, slow);
    b.SetInsertPoint(fast);
    llvm::Value* ticks = b.CreateLoad(i64t, curYieldCnt);
    llvm::Value* untilPoll = b.CreateSub(
        ci64(64), b.CreateAnd(ticks, ci64(63)));
    llvm::Value* available = b.CreateZExt(remaining32, i64t);
    llvm::Value* chunk = b.CreateSelect(
        b.CreateICmpULT(available, untilPoll), available, untilPoll);

    BasicBlock* preheader = b.GetInsertBlock();
    BasicBlock* nativeHead = newBB("rep.cond.ssa.head");
    BasicBlock* nativeExit = newBB("rep.cond.ssa.exit");
    b.CreateBr(nativeHead);
    b.SetInsertPoint(nativeHead);
    auto* iter = b.CreatePHI(
        plan.integer32State ? i32t : i64t, 2, "rep.cond.iter");
    iter->addIncoming(plan.integer32State ? ci32(0) : ci64(0), preheader);
    std::vector<llvm::PHINode*> carries;
    std::vector<llvm::Value*> state;
    for (llvm::Value* start : initial) {
        auto* carry = b.CreatePHI(
            plan.integer32State ? i32t : i64t, 2, "rep.cond.var");
        carry->addIncoming(start, preheader);
        carries.push_back(carry);
        state.push_back(carry);
    }
    emitConditionalStatements(plan, plan.stmts, state);
    BasicBlock* tail = b.GetInsertBlock();
    llvm::Value* nextIter = b.CreateAdd(
        iter, plan.integer32State ? ci32(1) : ci64(1));
    iter->addIncoming(nextIter, tail);
    for (size_t i = 0; i < carries.size(); ++i)
        carries[i]->addIncoming(state[i], tail);
    llvm::Value* limit = plan.integer32State
        ? b.CreateTrunc(chunk, i32t) : chunk;
    b.CreateCondBr(b.CreateICmpULT(nextIter, limit),
                   nativeHead, nativeExit);
    b.SetInsertPoint(nativeExit);
    std::vector<llvm::PHINode*> results;
    for (llvm::Value* result : state) {
        auto* phi = b.CreatePHI(
            plan.integer32State ? i32t : i64t, 1, "rep.cond.result");
        phi->addIncoming(result, tail);
        results.push_back(phi);
    }
    for (size_t i = 0; i < results.size(); ++i) {
        if (!plan.vars[i].written) continue;
        llvm::Value* number = b.CreateSIToFP(results[i], f64t);
        llvm::Value* bits = emitCanonKnownNonNaN(number);
        const auto* field = plan.vars[i].field;
        if (PromotedVar* p = findPromoted(field); p && p->numeric)
            storeNumericPair(*p, bits, number, b.getInt1(true));
        else
            emitVarSet(field, bits);
    }
    b.CreateStore(b.CreateSub(remaining32, b.CreateTrunc(chunk, i32t)),
                  countdown);
    emitSsaChunkScheduler(ticks, chunk, untilPoll, true);
    b.CreateBr(loopHead);
    b.SetInsertPoint(slow);
}

#include "numeric_until_ssa.inc"

#include "affine_until_ssa.inc"
