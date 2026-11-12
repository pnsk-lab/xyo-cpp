// Pure numeric Scratch repeat -> guarded native boxed-value SSA.
// The normal arithmetic lowerer is reused for each expression; only the
// expensive per-statement scalar slot traffic and scheduler work are fused.
struct NumericRepeatExpr {
    enum class Kind {
        Variable, Number, Add, Sub, Mul, Div, Mod, Sqrt, MathOp, Round
    };
    Kind kind = Kind::Number;
    int32_t mathFn = -1;
    unsigned variable = 0;
    int left = -1, right = -1;
    double number = 0;
};
struct NumericRepeatStmt {
    unsigned variable;
    int expression;
    bool change;
};
struct NumericRepeatPlan {
    struct Variable {
        const BlockField* field;
        bool written = false;
    };
    std::vector<Variable> variables;
    std::vector<NumericRepeatExpr> exprs;
    std::vector<NumericRepeatStmt> stmts;
    std::vector<bool> integerVars;
    // All integer carriers and intermediate expressions are proven
    // <= UINT32_MAX throughout the whole 64-step scheduler chunk.
    bool integer32Mode = false;
    bool integer32SignedSafe = false;
    bool integer32SignedPreferred = false;
    bool fastFloat = false;
};

std::optional<NumericRepeatPlan> numericRepeatPlan(Block* loop, bool forceFloat = false) {
    const char* enabled = std::getenv("XYO_JIT_NUMERIC_REPEAT_SSA");
    const bool explicitlyEnabled =
        forceFloat || (enabled && enabled[0] == '1');
    if ((enabled && enabled[0] == '0' && !forceFloat) ||
        runtime.blockCountEnabled || std::getenv("XYO_PROF"))
        return std::nullopt;
    const BlockInput* sub = loop->inputs.find("SUBSTACK");
    if (!sub || sub->block.empty()) return std::nullopt;
    NumericRepeatPlan plan;
    const auto resolve = [&](const BlockField* f) -> std::optional<unsigned> {
        if (!f || f->id.empty() || !f->value.isString() ||
            !f->value.asStrPtr() || !target) return std::nullopt;
        const auto* variable = target->lookupVariableById(f->id);
        if (!variable || variable->type != Variable::kScalarType)
            return std::nullopt;
        for (unsigned i = 0; i < plan.variables.size(); ++i) {
            const auto* prev = plan.variables[i].field;
            if (prev->id == f->id)
                return sameNamedField(prev, f) ? std::optional<unsigned>(i)
                                               : std::nullopt;
            if (sameNamedField(prev, f)) return std::nullopt;
        }
        if (plan.variables.size() >= 16) return std::nullopt;
        plan.variables.push_back({f, false});
        return unsigned(plan.variables.size() - 1);
    };
    std::function<std::optional<int>(Block*, unsigned)> parse;
    parse = [&](Block* node, unsigned depth) -> std::optional<int> {
        if (!node || depth > 10 || plan.exprs.size() >= 128)
            return std::nullopt;
        NumericRepeatExpr result;
        using K = NumericRepeatExpr::Kind;
        if (node->opcode == "data_variable") {
            const auto v = resolve(findField(node, "VARIABLE"));
            if (!v) return std::nullopt;
            result.kind = K::Variable;
            result.variable = *v;
        } else if (node->shadow &&
                   (node->opcode == "math_number" ||
                    node->opcode == "math_integer" ||
                    node->opcode == "math_whole_number" ||
                    node->opcode == "math_positive_number" ||
                    node->opcode == "math_angle") &&
                   node->fields.size() == 1) {
            const double d = cast::toNumberOrNaN(
                node->fields.begin()->second.value);
            if (!std::isfinite(d)) return std::nullopt;
            result.number = d;
        } else if (node->opcode == "operator_mathop" ||
                   node->opcode == "operator_round") {
            if (node->opcode == "operator_round") {
                result.kind = K::Round;
            } else {
                const auto* field = findField(node, "OPERATOR");
                if (!field || !field->value.isString()) return std::nullopt;
                const std::string name = SStr::toUtf8(
                    cast::toString(field->value));
                static const std::map<std::string, int32_t> kFunctions = {
                    {"abs", 0}, {"floor", 1}, {"ceiling", 2}, {"sqrt", 3},
                    {"sin", 4}, {"cos", 5}, {"atan", 9}, {"ln", 10},
                    {"e ^", 12}, {"10 ^", 13},
                };
                const auto it = kFunctions.find(name);
                if (it == kFunctions.end()) return std::nullopt;
                result.kind = it->second == 3 ? K::Sqrt : K::MathOp;
                result.mathFn = it->second;
            }
            const auto child = parse(transientChild(node, "NUM"), depth + 1);
            if (!child) return std::nullopt;
            result.left = *child;
        } else {
            const auto& op = node->opcode;
            if (op == "operator_add") result.kind = K::Add;
            else if (op == "operator_subtract") result.kind = K::Sub;
            else if (op == "operator_multiply") result.kind = K::Mul;
            else if (op == "operator_divide") result.kind = K::Div;
            else if (op == "operator_mod") result.kind = K::Mod;
            else return std::nullopt;
            const auto lhs = parse(transientChild(node, "NUM1"), depth + 1);
            const auto rhs = parse(transientChild(node, "NUM2"), depth + 1);
            if (!lhs || !rhs) return std::nullopt;
            result.left = *lhs;
            result.right = *rhs;
        }
        plan.exprs.push_back(result);
        return int(plan.exprs.size() - 1);
    };
    Block* node = getBlock(sub->block);
    while (node) {
        if (plan.stmts.size() >= 20) return std::nullopt;
        const bool change = node->opcode == "data_changevariableby";
        if (!change && node->opcode != "data_setvariableto")
            return std::nullopt;
        const auto dst = resolve(findField(node, "VARIABLE"));
        Block* rhs = transientChild(node, "VALUE");
        if (!dst || !rhs) return std::nullopt;
        // A bare reporter/number may preserve its original textual tag
        // when assigned. Only arithmetic operators guarantee a Number.
        if (!change && rhs->opcode != "operator_add" &&
            rhs->opcode != "operator_subtract" &&
            rhs->opcode != "operator_multiply" &&
            rhs->opcode != "operator_divide" &&
            rhs->opcode != "operator_mod" &&
            rhs->opcode != "operator_mathop" &&
            rhs->opcode != "operator_round" &&
            // A plain variable assignment copies the exact numeric bits;
            // only the numeric-until path may admit this additional shape.
            !(forceFloat && rhs->opcode == "data_variable"))
            return std::nullopt;
        const auto expression = parse(rhs, 0);
        if (!expression) return std::nullopt;
        plan.stmts.push_back({*dst, *expression, change});
        plan.variables[*dst].written = true;
        if (node->next.empty()) break;
        node = getBlock(node->next);
    }
    if (!node || plan.stmts.empty()) return std::nullopt;

    // A costly, side-effect-free floating recurrence can use plain double
    // PHIs and defer Scratch boxing to each 64-tick checkpoint. Runtime
    // guards and per-iteration NaN bailout preserve the original semantics.
    // Restrict the fast path to multi-statement loops with expensive pure
    // mathematical reporters, where removing repeated tag decoding can
    // outweigh checkpoint overhead.
    const char* floatMode = std::getenv("XYO_JIT_NUMERIC_REPEAT_FLOAT");
    const bool hasCostlyOp = std::any_of(
        plan.exprs.begin(), plan.exprs.end(), [](const auto& expr) {
            using K = NumericRepeatExpr::Kind;
            return expr.kind == K::Sqrt || expr.kind == K::MathOp ||
                   expr.kind == K::Round;
        });
    if ((!floatMode || floatMode[0] != '0') && hasCostlyOp &&
        plan.stmts.size() >= 2) {
        plan.fastFloat = true;
        return plan;
    }

    // Optional exact nonnegative integer carriers for a subset of variables.
    // A strict least-greatest fixed-point classifies only variables whose
    // every write preserves the nonnegative integer invariant. Remaining
    // variables keep the ordinary IEEE f64/boxed SSA representation.
    const char* intMode = std::getenv("XYO_JIT_NUMERIC_REPEAT_INT");
    const bool tryInteger = (!intMode || intMode[0] != '0') &&
        std::any_of(plan.exprs.begin(), plan.exprs.end(), [](const auto& e) {
            return e.kind == NumericRepeatExpr::Kind::Mod;
        });
    if (!tryInteger)
        return explicitlyEnabled ? std::optional(std::move(plan))
                                 : std::nullopt;
    using K = NumericRepeatExpr::Kind;
    std::vector<bool> integers(plan.variables.size(), true);
    const auto integerExpr = [&](const auto& self, int index) -> bool {
        const auto& e = plan.exprs[size_t(index)];
        if (e.kind == K::Variable) return integers[e.variable];
        if (e.kind == K::Number)
            return e.number >= 0.0 && e.number <= 1000000000.0 &&
                   !std::signbit(e.number) &&
                   std::trunc(e.number) == e.number;
        if (e.kind == K::Add || e.kind == K::Mul)
            return self(self, e.left) && self(self, e.right);
        if (e.kind == K::Mod) {
            const auto& d = plan.exprs[size_t(e.right)];
            return d.kind == K::Number && d.number >= 1.0 &&
                   self(self, e.right) && self(self, e.left);
        }
        return false;
    };
    bool changed;
    do {
        changed = false;
        for (const auto& stmt : plan.stmts) {
            if (integers[stmt.variable] &&
                !integerExpr(integerExpr, stmt.expression)) {
                integers[stmt.variable] = false;
                changed = true;
            }
        }
    } while (changed);
    if (std::none_of(integers.begin(), integers.end(),
                     [](bool yes) { return yes; }))
        return explicitlyEnabled ? std::optional(std::move(plan))
                                 : std::nullopt;

    // Bound every intermediate and variable across a complete 64-tick
    // scheduler chunk. Use a generous yet exact-in-double interval.
    // If a proof fails, use the existing float-only SSA plan unchanged.
    constexpr long double kEntryLimit = 2097152.0L;
    constexpr long double kExactLimit = 281474976710656.0L;
    std::vector<long double> bound(plan.variables.size(), kEntryLimit);
    // An i32-typed PHI is legal only if EVERY integer intermediate and
    // EVERY value written to an integer carrier fits in unsigned i32.
    // We record violations across all 64 speculative iterations.
    bool allIntegerOpsFitU32 = true;
    bool allIntegerOpsFitS32 = true;
    constexpr long double kU32Max = 4294967295.0L;
    constexpr long double kS32Max = 2147483647.0L;
    const auto boundExpr = [&](const auto& self, int index)
        -> std::optional<long double> {
        const auto& e = plan.exprs[size_t(index)];
        if (e.kind == K::Variable)
            return integers[e.variable]
                ? std::optional(bound[e.variable]) : std::nullopt;
        if (e.kind == K::Number)
            return integerExpr(integerExpr, index)
                ? std::optional(static_cast<long double>(e.number))
                : std::nullopt;
        if (e.kind == K::Add || e.kind == K::Mul) {
            auto a = self(self, e.left), b = self(self, e.right);
            if (!a || !b) return std::nullopt;
            long double result = e.kind == K::Add ? *a + *b : *a * *b;
            if (result > kU32Max) allIntegerOpsFitU32 = false;
            if (result > kS32Max) allIntegerOpsFitS32 = false;
            return result <= kExactLimit ? std::optional(result)
                                         : std::nullopt;
        }
        if (e.kind == K::Mod) {
            const auto dividendBound = self(self, e.left);
            if (!dividendBound) return std::nullopt;
            const auto& divisor = plan.exprs[size_t(e.right)];
            if (divisor.kind != K::Number || divisor.number < 1.0 ||
                std::trunc(divisor.number) != divisor.number)
                return std::nullopt;
            if (*dividendBound > kU32Max ||
                divisor.number > double(kU32Max))
                allIntegerOpsFitU32 = false;
            if (*dividendBound > kS32Max ||
                divisor.number > double(kS32Max))
                allIntegerOpsFitS32 = false;
            return static_cast<long double>(divisor.number - 1.0);
        }
        return std::nullopt;
    };
    bool safe = true;
    for (int iter = 0; iter < 64 && safe; ++iter) {
        for (const auto& stmt : plan.stmts) {
            if (!integers[stmt.variable]) continue;
            auto value = boundExpr(boundExpr, stmt.expression);
            if (!value) { safe = false; break; }
            long double next = stmt.change
                ? bound[stmt.variable] + *value : *value;
            if (next > kU32Max) allIntegerOpsFitU32 = false;
            if (next > kS32Max) allIntegerOpsFitS32 = false;
            if (next > kExactLimit) { safe = false; break; }
            bound[stmt.variable] = next;
        }
    }
    // A proven integer that is never used in an integer-modulo recurrence
    // is not worth changing the compiled loop by default.
    const auto hasMod = [&](const auto& self, int index) -> bool {
        const auto& e = plan.exprs[size_t(index)];
        if (e.kind == K::Mod) return true;
        if (e.kind == K::Add || e.kind == K::Sub ||
            e.kind == K::Mul || e.kind == K::Div)
            return self(self, e.left) || self(self, e.right);
        if (e.kind == K::Sqrt) return self(self, e.left);
        return false;
    };
    bool useful = false;
    for (const auto& stmt : plan.stmts)
        if (integers[stmt.variable] &&
            hasMod(hasMod, stmt.expression))
            useful = true;
    if (safe && useful) {
        plan.integerVars = std::move(integers);
        const char* narrow = std::getenv("XYO_JIT_NUMERIC_REPEAT_U32_MOD");
        plan.integer32Mode = allIntegerOpsFitU32 &&
            (!narrow || narrow[0] != '0');
        plan.integer32SignedSafe = plan.integer32Mode &&
            allIntegerOpsFitS32;
        // LLVM's signed-remainder lowering is faster for small
        // single-modulus recurrences, but is substantially slower
        // when a loop has many independent constant remainders.
        // Use a structural, per-loop cost heuristic (not project names).
        const size_t modSites = std::count_if(
            plan.exprs.begin(), plan.exprs.end(), [](const auto& e) {
                return e.kind == K::Mod;
            });
        plan.integer32SignedPreferred =
            plan.integer32SignedSafe && modSites == 1;
    }
    if (!explicitlyEnabled && plan.integerVars.empty())
        return std::nullopt;
    return plan;
}

// Keep boxed state and its already-coerced numerical shadow together.
// Arithmetic results are produced by the common Scratch lowering so NaN
// canonicalization and signed-zero behavior exactly match the normal JIT.
struct NativeNumericValue {
    llvm::Value* bits;
    llvm::Value* number;
    llvm::Value* integer = nullptr;  // positive exact i64 when proven
};
NativeNumericValue numericFromInteger(llvm::Value* n) {
    llvm::Value* d = b.CreateUIToFP(n, f64t);
    return {b.CreateBitCast(d, i64t), d, n};
}
NativeNumericValue emitNumericRepeatExpr(
    const NumericRepeatPlan& plan, int index,
    const std::vector<NativeNumericValue>& state,
    bool integerMode) {
    using K = NumericRepeatExpr::Kind;
    const auto& e = plan.exprs[size_t(index)];
    if (e.kind == K::Variable) return state[e.variable];
    if (e.kind == K::Number) {
        if (integerMode && e.number >= 0 &&
            !std::signbit(e.number) &&
            std::trunc(e.number) == e.number &&
            e.number <= 1000000000.0)
            return numericFromInteger(plan.integer32Mode
                ? ci32(uint32_t(e.number)) : ci64(uint64_t(e.number)));
        return {b.CreateBitCast(cf64(e.number), i64t), cf64(e.number)};
    }
    NativeNumericValue lhs =
        emitNumericRepeatExpr(plan, e.left, state, integerMode);
    if (e.kind == K::Sqrt || e.kind == K::MathOp ||
        e.kind == K::Round) {
        llvm::Value* number = e.kind == K::Round
            ? emitJsRound(lhs.number)
            : emitMathop(e.kind == K::Sqrt ? 3 : e.mathFn, lhs.number);
        llvm::Value* bits = emitCanon(number);
        return {bits, emitToNumF64(bits)};
    }
    NativeNumericValue rhs =
        emitNumericRepeatExpr(plan, e.right, state, integerMode);
    // Exact nonnegative SSA values may use native integer instructions and
    // unsigned constant remainder. The plan proves every intermediate and
    // each 64-step variable bound below 2^48, so converting to double is
    // exact. This path never observes -0 or nonnumeric values.
    if (integerMode && lhs.integer && rhs.integer) {
        if (e.kind == K::Add)
            return numericFromInteger(
                b.CreateAdd(lhs.integer, rhs.integer, "rep.int.add"));
        if (e.kind == K::Mul)
            return numericFromInteger(
                b.CreateMul(lhs.integer, rhs.integer, "rep.int.mul"));
        if (e.kind == K::Mod) {
            const char* signedMod =
                std::getenv("XYO_JIT_NUMERIC_REPEAT_SIGNED_MOD");
            const bool chooseSigned = signedMod
                ? signedMod[0] == '1'
                : plan.integer32SignedPreferred;
            if (plan.integer32SignedSafe && chooseSigned)
                return numericFromInteger(
                    b.CreateSRem(lhs.integer, rhs.integer, "rep.int.smod"));
            return numericFromInteger(
                b.CreateURem(lhs.integer, rhs.integer, "rep.int.mod"));
        }
    }
    AOp opcode;
    switch (e.kind) {
    case K::Add: opcode = AOp::Add; break;
    case K::Sub: opcode = AOp::Sub; break;
    case K::Mul: opcode = AOp::Mul; break;
    case K::Div: opcode = AOp::Div; break;
    case K::Mod: opcode = AOp::Mod; break;
    default: return {ci64(val::kNan), cf64(0.0)};
    }
    llvm::Value* bits = emitArithNumeric(lhs.number, rhs.number, opcode);
    return {bits, emitToNumF64(bits)};
}
#include "numeric_repeat_float_ssa.inc"

void emitNumericRepeatChunk(const NumericRepeatPlan& plan,
                            llvm::Value* remaining32,
                            AllocaInst* countdown, BasicBlock* loopHead) {
    if (plan.fastFloat) {
        emitFloatRepeatChunk(plan, remaining32, countdown, loopHead);
        return;
    }
    BasicBlock* fast = newBB("rep.numeric.fast");
    BasicBlock* slow = newBB("rep.numeric.slow");
    llvm::Value* valid = b.CreateICmpNE(curYieldWarp, ci32(0));
    std::vector<NativeNumericValue> initial;
    for (size_t index = 0; index < plan.variables.size(); ++index) {
        const auto& var = plan.variables[index];
        llvm::Value* bits = emitVarGet(var.field);
        // No unsafe coercion from a String/Boolean/NaN to a raw Number:
        // such inputs continue through the unchanged normal JIT body.
        llvm::Value* plain = b.CreateICmpSLT(bits, ci64(val::kNan));
        llvm::Value* number = b.CreateBitCast(bits, f64t);
        valid = b.CreateAnd(valid,
            b.CreateAnd(plain, b.CreateFCmpORD(number, number)));
        llvm::Value* integer = nullptr;
        if (!plan.integerVars.empty() && plan.integerVars[index]) {
            llvm::Value* eligible = b.CreateAnd(
                b.CreateFCmpOGE(number, cf64(0.0)),
                b.CreateAnd(
                    b.CreateFCmpOLE(number, cf64(2097152.0)),
                    b.CreateFCmpOEQ(number,
                        b.CreateUnaryIntrinsic(Intrinsic::trunc, number))));
            eligible = b.CreateAnd(eligible, b.CreateICmpNE(
                bits, ci64(0x8000000000000000ull)));
            valid = b.CreateAnd(valid, eligible);
            integer = b.CreateFPToUI(
                b.CreateSelect(eligible, number, cf64(0.0)),
                plan.integer32Mode ? i32t : i64t);
        }
        initial.push_back({bits, number, integer});
    }
    b.CreateCondBr(valid, fast, slow);
    b.SetInsertPoint(fast);
    llvm::Value* ticks = b.CreateLoad(i64t, curYieldCnt);
    llvm::Value* untilPoll = yieldToPoll(ticks);
    llvm::Value* available = b.CreateZExt(remaining32, i64t);
    llvm::Value* chunk = b.CreateSelect(
        b.CreateICmpULT(available, untilPoll), available, untilPoll);
    BasicBlock* preheader = b.GetInsertBlock();
    BasicBlock* nativeHead = newBB("rep.numeric.head");
    BasicBlock* nativeExit = newBB("rep.numeric.exit");
    b.CreateBr(nativeHead);
    b.SetInsertPoint(nativeHead);
    // The scheduler limits a native chunk to at most 64 iterations.
    // The counter therefore needs only 32 bits; the 64-bit scheduler
    // tick and yield semantics remain unchanged.
    const char* narrowIter =
        std::getenv("XYO_JIT_NUMERIC_REPEAT_I32_ITER");
    const bool useI32Iter = !narrowIter || narrowIter[0] != '0';
    auto* iter = b.CreatePHI(
        useI32Iter ? i32t : i64t, 2, "rep.numeric.iter");
    iter->addIncoming(useI32Iter ? ci32(0) : ci64(0), preheader);
    std::vector<llvm::PHINode*> rawCarries, numericCarries, integerCarries;
    std::vector<NativeNumericValue> state;
    for (size_t i = 0; i < initial.size(); ++i) {
        const auto& start = initial[i];
        if (start.integer) {
            auto* carry = b.CreatePHI(
                plan.integer32Mode ? i32t : i64t, 2, "rep.numeric.int");
            carry->addIncoming(start.integer, preheader);
            rawCarries.push_back(nullptr);
            numericCarries.push_back(nullptr);
            integerCarries.push_back(carry);
        } else {
            auto* raw = b.CreatePHI(i64t, 2, "rep.numeric.raw");
            auto* numeric = b.CreatePHI(f64t, 2, "rep.numeric.number");
            raw->addIncoming(start.bits, preheader);
            numeric->addIncoming(start.number, preheader);
            rawCarries.push_back(raw);
            numericCarries.push_back(numeric);
            integerCarries.push_back(nullptr);
        }
    }
    // A loop header must contain *only* PHI nodes before the first
    // non-PHI instruction. numericFromInteger() emits uitofp/bitcast, so
    // construct every carry PHI first and materialize numeric views only
    // afterwards. With multiple integer variables the old interleaving
    // produced invalid LLVM IR (caught by AOT verifyModule).
    for (size_t i = 0; i < initial.size(); ++i) {
        if (integerCarries[i])
            state.push_back(numericFromInteger(integerCarries[i]));
        else
            state.push_back({rawCarries[i], numericCarries[i], nullptr});
    }
    for (const auto& stmt : plan.stmts) {
        const bool integerMode = !plan.integerVars.empty() &&
            plan.integerVars[stmt.variable];
        NativeNumericValue result = emitNumericRepeatExpr(
            plan, stmt.expression, state, integerMode);
        if (stmt.change) {
            if (integerMode && state[stmt.variable].integer && result.integer) {
                result = numericFromInteger(b.CreateAdd(
                    state[stmt.variable].integer, result.integer,
                    "rep.int.change"));
            } else {
                llvm::Value* bits = emitArithNumeric(
                    state[stmt.variable].number, result.number, AOp::Add);
                result = {bits, emitToNumF64(bits), nullptr};
            }
        }
        state[stmt.variable] = result;
    }
    BasicBlock* tail = b.GetInsertBlock();
    llvm::Value* nextIter = b.CreateAdd(
        iter, useI32Iter ? ci32(1) : ci64(1));
    iter->addIncoming(nextIter, tail);
    for (size_t i = 0; i < rawCarries.size(); ++i) {
        if (integerCarries[i])
            integerCarries[i]->addIncoming(state[i].integer, tail);
        else {
            rawCarries[i]->addIncoming(state[i].bits, tail);
            numericCarries[i]->addIncoming(state[i].number, tail);
        }
    }
    llvm::Value* limit = useI32Iter
        ? b.CreateTrunc(chunk, i32t) : chunk;
    b.CreateCondBr(b.CreateICmpULT(nextIter, limit), nativeHead, nativeExit);
    b.SetInsertPoint(nativeExit);
    for (size_t i = 0; i < state.size(); ++i) {
        if (!plan.variables[i].written) continue;
        const auto* field = plan.variables[i].field;
        if (PromotedVar* p = findPromoted(field); p && p->numeric) {
            llvm::Value* ok = b.CreateICmpNE(state[i].bits, ci64(val::kNan));
            storeNumericPair(*p, state[i].bits, state[i].number, ok);
        } else {
            emitVarSet(field, state[i].bits);
        }
    }
    b.CreateStore(
        b.CreateSub(remaining32, b.CreateTrunc(chunk, i32t)), countdown);
    emitSsaChunkScheduler(ticks, chunk, untilPoll, true);
    b.CreateBr(loopHead);
    b.SetInsertPoint(slow);
}
