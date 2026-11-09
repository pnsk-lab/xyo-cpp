// xyo-cpp: スクリプト → wasm コンパイラの実装（Phase C）。
// 仕様は wasm_compile.hpp のコメントを参照。
#include "wasm_compile.hpp"
#include "../ir/execution_abi.hpp"
#include "../ir/numeric.hpp"
#include "../ir/diamond.hpp"
#include "../ir/loop.hpp"
#include "../ir/guard.hpp"
#include "../ir/slot_access.hpp"
#include "../ir/value_abi.hpp"
#include "../ir/scratch_script.hpp"
#include "../ir/scratch_effect.hpp"
#include "../ir/scratch_pipeline.hpp"

#include "core/cast.hpp"
#include "core/sstr.hpp"
#include "core/value.hpp"
#include "model/blocks.hpp"

#include <algorithm>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include "wasm_emit.hpp"

namespace xyo::wasm {

namespace {

using xyo::val::kNan;

// ローカル配置:
//   0 = slots (i32, 引数)
//   1 = bits  (i64 スクラッチ)
//   2 = num   (f64 スクラッチ)
//   3 = tmp   (f64 スクラッチ)
constexpr uint32_t kBits = 1, kNum = 2, kTmp = 3, kFirstFree = 4;

class Gen {
public:
    Gen(const Program& p, Code& c, uint32_t hostHelper)
        : p_(p), c_(c), hostHelper_(hostHelper) {}

    void run() {
        // An inlined warp procedure may contain dynamic nested repeats.
        // Their iteration count is limited per speculative WASM segment:
        // exceeding the budget bails and rolls the *whole segment* back
        // before the interpreter retries with normal scheduling semantics.
        loopFuel_ = nextLocal(ValType::I32);
        c_.i32Const(kMaxNestedIterations);
        c_.localSet(loopFuel_);
        emitStmts(p_.main);
        c_.i32Const(0);   // 完走
    }

    uint32_t nextLocal(ValType t) {
        c_.local(t);
        return next_++;
    }

private:
    // A stack i32 truth predicate is consumed; on failure the module exits
    // before any state mutation in the guarded speculative region.
    void requireGuard(ir::GuardReason reason) {
        struct Lowerer {
            Code& code;
            void require(int, const ir::Guard& guard) {
                assert(guard.failure == ir::GuardFailure::Bail);
                code.i32Eqz();
                code.if_();
                code.i32Const(1);
                code.return_();
                code.end();
            }
        } lowerer{c_};
        ir::lowerGuard(lowerer, {reason, ir::GuardFailure::Bail}, 0);
    }

    // Raw tagged-value slot ABI shared with the LLVM backend. The source
    // representation is not coerced; numeric consumers still use a guard.
    void emitRawSlotLoad(uint32_t slot) {
        struct Lowerer {
            Code& code;
            uint32_t base;
            int loadSlot(uint32_t index) {
                code.localGet(base);
                code.i64Load(3, index * ir::kSlotBytes);
                return 0; // value is on the WASM operand stack
            }
        } backend{c_, 0};
        ir::lowerSlotLoad(backend, slot);
    }

    void emitNumericBitsGuard() {
        // Input is kBits, not an F64; the guard's failure path must
        // keep Scratch's string/boolean conversion in the interpreter.
        struct Lowerer {
            Code& code;
            int isNumericBits(int) {
                code.localGet(kBits);
                code.i64Const(48);
                code.i64ShrU();
                code.i64Const(0xFFF8);
                code.i64And();
                code.i64Const(0x7FF8);
                code.i64Eq();
                code.i32Eqz();
                code.localGet(kBits);
                code.i64Const(int64_t(xyo::val::kNan));
                code.i64Eq();
                code.i32Or();
                return 0; // guarded predicate is on the operand stack
            }
        } backend{c_};
        ir::lowerNumericSlotGuard(backend, 0);
        requireGuard(ir::GuardReason::NumericValue);
    }

    // Fast ToNumber for all immediate Scratch tags. Heap strings must bail
    // into the host interpreter's UTF-16 number parser. Exactly the same
    // contract as ir::toNumberImmediate; the result is f64 on the stack.
    void emitImmediateToNumber() {
        struct Lowerer {
            Gen& gen;
            void immediateNumber(int) {
                auto& c = gen.c_;
                c.localGet(kBits);
                c.i64Const(int64_t(val::kTrue));
                c.i64Eq();
                c.if_();
                c.f64Const(1.0);
                c.localSet(kNum);
                c.else_();
                // False/empty/undefined/color all coerce to numeric zero.
                // Heap strings must not silently coerce; compare their tag
                // before the fallback path.
                c.localGet(kBits);
                c.i64Const(int64_t(val::kFalse));
                c.i64Eq();
                c.localGet(kBits);
                c.i64Const(int64_t(val::kEmptyStr));
                c.i64Eq();
                c.i32Or();
                c.localGet(kBits);
                c.i64Const(int64_t(val::kUndefined));
                c.i64Eq();
                c.i32Or();
                c.localGet(kBits);
                c.i64Const(48);
                c.i64ShrU();
                c.i64Const(0x7FFA);
                c.i64Eq();
                c.i32Or();
                c.if_();
                c.f64Const(0.0);
                c.localSet(kNum);
                c.else_();
                // String parsing is delegated to the exact Scratch host
                // parser when running in a transactional HotSpot segment.
                // Standalone modules have no host math ABI and must bail.
                c.localGet(kBits);
                c.i64Const(48);
                c.i64ShrU();
                c.i64Const(0x7FF9);
                c.i64Eq();
                c.if_();
                if (gen.p_.useHostMath) {
                    const auto scratch = uint32_t(
                        (gen.p_.slotIds.size() + 1) * ir::kSlotBytes);
                    c.localGet(0);
                    c.localGet(kBits);
                    c.i64Store(3, scratch);
                    c.i32Const(5);
                    c.localGet(0);
                    c.i32Const(int32_t(scratch));
                    c.i32Add();
                    c.i32Const(0);
                    c.call(gen.hostHelper_);
                    c.i32Eqz();
                    gen.requireGuard(ir::GuardReason::NumericValue);
                    c.localGet(0);
                    c.f64Load(3, scratch);
                    c.localSet(kNum);
                } else {
                    gen.bail();
                }
                c.else_();
                gen.emitNumericBitsGuard();
                c.localGet(kBits);
                c.f64ReinterpretI64();
                gen.nanToZero();
                c.localSet(kNum);
                c.end(); // heap-string or numeric value
                c.end(); // immediate zero or slower value
                c.end(); // boolean true or other value
                c.localGet(kNum);
            }
        } lowerer{*this};
        ir::lowerImmediateNumber(lowerer, 0);
    }

    void emitRawSlotStore(uint32_t slot) {
        // The tagged value was already staged in kBits by the caller.
        // Avoid an unnecessary local.get/local.set roundtrip.
        struct Lowerer {
            Code& code;
            void storeSlot(uint32_t index, int) {
                code.localGet(0);
                code.localGet(kBits);
                code.i64Store(3, index * ir::kSlotBytes);
            }
        } backend{c_};
        ir::lowerSlotStore(backend, slot, 0);
    }

    void bail() {
        c_.i32Const(ir::AtomicWarpExecutionAbi::status(
            ir::AtomicWarpStatus::Rollback));
        c_.return_();
    }

    /// f64 を 1 つ消費し、NaN を 0 に落として 1 つ残す。
    void nanToZero() {
        c_.localSet(kTmp);                                  // x
        c_.localGet(kTmp);                                  // v1 = x
        c_.f64Const(0.0);                                   // v2 = 0
        c_.localGet(kTmp);
        c_.localGet(kTmp);
        c_.f64Eq();                                         // cond = (x==x)
        c_.select();                                        // x か 0
    }

    /// f64 を 1 つ消費して slots[var] へ格納（NaN は kNan へ正規化）。
    void storeF64(uint32_t var) {
        c_.localSet(kTmp);
        c_.localGet(kTmp);
        c_.localGet(kTmp);
        c_.f64Ne();                                         // isnan
        c_.if_();
        c_.i64Const(int64_t(kNan));
        c_.localSet(kBits);
        c_.else_();
        c_.localGet(kTmp);
        c_.i64ReinterpretF64();
        c_.localSet(kBits);
        c_.end();
        emitRawSlotStore(var);
    }

    // Preserve the Value tag at mixed-type host call boundaries. Ordinary
    // arithmetic uses f64, but comparisons, strings and assignments cannot
    // silently turn boolean false into the number 0.
    void emitTaggedBinaryHost(const Expr& e, int32_t helperId,
                              bool returnsFloat) {
        if (!p_.useHostMath) {
            bail();
            if (returnsFloat) c_.f64Const(0);
            else c_.i64Const(int64_t(val::kEmptyStr));
            return;
        }
        const uint32_t left = nextLocal(ValType::I64);
        const uint32_t right = nextLocal(ValType::I64);
        const uint32_t scratch = uint32_t(
            (p_.slotIds.size() + 1) * ir::kSlotBytes);
        // Scratch evaluates both inputs once before invoking the primitive.
        // Nested joins may overwrite the scratch buffer; stage locally first.
        emitTaggedExpr(e.a);
        c_.localSet(left);
        emitTaggedExpr(e.b);
        c_.localSet(right);
        c_.localGet(0);
        c_.localGet(left);
        c_.i64Store(3, scratch);
        c_.localGet(0);
        c_.localGet(right);
        c_.i64Store(3, scratch + 8);
        c_.i32Const(helperId);
        c_.localGet(0);
        c_.i32Const(int32_t(scratch));
        c_.i32Add();
        c_.i32Const(0);
        c_.call(hostHelper_);
        c_.i32Eqz();
        requireGuard(ir::GuardReason::NumericValue);
        c_.localGet(0);
        if (returnsFloat) c_.f64Load(3, scratch);
        else c_.i64Load(3, scratch);
    }

    void emitTaggedExpr(uint32_t idx) {
        const Expr& e = p_.exprs[idx];
        if (e.k == EK::ConstText) {
            if (!p_.useHostMath) {
                bail();
                c_.i64Const(int64_t(val::kEmptyStr));
                return;
            }
            const uint32_t scratch = uint32_t(
                (p_.slotIds.size() + 1) * ir::kSlotBytes);
            c_.i32Const(21);
            c_.localGet(0);
            c_.i32Const(int32_t(scratch));
            c_.i32Add();
            c_.i32Const(int32_t(e.var));
            c_.call(hostHelper_);
            c_.i32Eqz();
            requireGuard(ir::GuardReason::NumericValue);
            c_.localGet(0);
            c_.i64Load(3, scratch);
            return;
        }
        if (e.k == EK::StringJoin || e.k == EK::StringLetter) {
            emitTaggedBinaryHost(e, e.k == EK::StringJoin ? 18 : 19, false);
            return;
        }
        if (e.k == EK::ListContents) {
            if (!p_.useHostMath) {
                bail();
                c_.i64Const(int64_t(val::kEmptyStr));
                return;
            }
            const uint32_t scratch = uint32_t(
                (p_.slotIds.size() + 1) * ir::kSlotBytes);
            c_.i32Const(26);
            c_.localGet(0);
            c_.i32Const(int32_t(scratch));
            c_.i32Add();
            c_.i32Const(int32_t(e.var));
            c_.call(hostHelper_);
            c_.i32Eqz();
            requireGuard(ir::GuardReason::NumericValue);
            c_.localGet(0);
            c_.i64Load(3, scratch);
            return;
        }
        if (e.k == EK::ListGet) {
            if (!p_.useHostMath) {
                bail();
                c_.i64Const(int64_t(val::kEmptyStr));
                return;
            }
            // Read the staged list journal and return the original tagged
            // Value. A numeric-only read would destroy strings/booleans
            // when the list item is assigned or joined.
            const uint32_t scratch = uint32_t(
                (p_.slotIds.size() + 1) * ir::kSlotBytes);
            const uint32_t index = nextLocal(ValType::I64);
            emitTaggedExpr(e.a);
            c_.localSet(index);
            c_.localGet(0);
            c_.localGet(index);
            c_.i64Store(3, scratch);
            c_.i32Const(23);
            c_.localGet(0);
            c_.i32Const(int32_t(scratch));
            c_.i32Add();
            c_.i32Const(int32_t(e.var));
            c_.call(hostHelper_);
            c_.i32Eqz();
            requireGuard(ir::GuardReason::NumericValue);
            c_.localGet(0);
            c_.i64Load(3, scratch);
            return;
        }
        if (e.k == EK::ProcedureArgument) {
            const auto it = argumentLocals_.find(e.var);
            if (it == argumentLocals_.end()) {
                bail();
                c_.i64Const(int64_t(val::kUndefined));
            } else {
                c_.localGet(it->second);
            }
            return;
        }
        if (e.k == EK::VarGet) {
            emitRawSlotLoad(e.var);
            return;
        }
        if (e.k == EK::ConstNum) {
            if (std::isnan(e.num)) c_.i64Const(int64_t(val::kNan));
            else {
                c_.f64Const(e.num);
                c_.i64ReinterpretF64();
            }
            return;
        }
        if (e.k == EK::LessThan || e.k == EK::GreaterThan ||
            e.k == EK::Equals || e.k == EK::And || e.k == EK::Or ||
            e.k == EK::Not || e.k == EK::ListContains ||
            e.k == EK::StringContains) {
            emitExpr(idx);
            truthy();
            c_.if_();
            c_.i64Const(int64_t(val::kTrue));
            c_.localSet(kBits);
            c_.else_();
            c_.i64Const(int64_t(val::kFalse));
            c_.localSet(kBits);
            c_.end();
            c_.localGet(kBits);
            return;
        }
        emitExpr(idx);
        // Every computed NaN must use the canonical Scratch numeric NaN
        // tag. A raw IEEE NaN payload in tagged slots could masquerade as
        // an invalid heap Value at the runtime helper boundary.
        c_.localSet(kTmp);
        c_.localGet(kTmp);
        c_.localGet(kTmp);
        c_.f64Ne();
        c_.if_();
        c_.i64Const(int64_t(val::kNan));
        c_.localSet(kBits);
        c_.else_();
        c_.localGet(kTmp);
        c_.i64ReinterpretF64();
        c_.localSet(kBits);
        c_.end();
        c_.localGet(kBits);
    }

    void emitComparableNumberPredicate(uint32_t local) {
        // Value::isNumber except NaN: Scratch compares canonical NaN by
        // string fallback rather than IEEE relational arithmetic.
        c_.localGet(local);
        c_.i64Const(48);
        c_.i64ShrU();
        c_.i64Const(0xFFF8);
        c_.i64And();
        c_.i64Const(0x7FF8);
        c_.i64Eq();
        c_.i32Eqz();
        c_.localGet(local);
        c_.i64Const(int64_t(val::kNan));
        c_.i64Ne();
        c_.i32And();
    }

    // Return an i32 Scratch truth value. Boolean logic and control_if
    // cannot use numeric ToNumber for a string input: "hello" is truthy
    // even though its numeric conversion is 0.
    void emitTruthyExpr(uint32_t idx) {
        const EK kind = p_.exprs[idx].k;
        const bool tagged = kind == EK::VarGet ||
            kind == EK::ProcedureArgument ||
            kind == EK::ListGet || kind == EK::ListContents ||
            kind == EK::ConstText || kind == EK::StringJoin ||
            kind == EK::StringLetter;
        if (!tagged || !p_.useHostMath) {
            emitExpr(idx);
            truthy();
            return;
        }
        const uint32_t result = nextLocal(ValType::I32);
        const uint32_t scratch = uint32_t(
            (p_.slotIds.size() + 1) * ir::kSlotBytes);
        emitTaggedExpr(idx);
        c_.localSet(kBits);
        // Numeric values are overwhelmingly common inside a loop. Avoid
        // calling the host for their ordinary f64 truthiness.
        c_.localGet(kBits);
        c_.i64Const(48);
        c_.i64ShrU();
        c_.i64Const(0xFFF8);
        c_.i64And();
        c_.i64Const(0x7FF8);
        c_.i64Eq();
        c_.i32Eqz();
        c_.localGet(kBits);
        c_.i64Const(int64_t(val::kNan));
        c_.i64Eq();
        c_.i32Or();
        c_.if_();
        c_.localGet(kBits);
        c_.f64ReinterpretI64();
        truthy();
        c_.localSet(result);
        c_.else_();
        c_.localGet(0);
        c_.localGet(kBits);
        c_.i64Store(3, scratch);
        c_.i32Const(22);
        c_.localGet(0);
        c_.i32Const(int32_t(scratch));
        c_.i32Add();
        c_.i32Const(0);
        c_.call(hostHelper_);
        c_.i32Eqz();
        requireGuard(ir::GuardReason::NumericValue);
        c_.localGet(0);
        c_.f64Load(3, scratch);
        truthy();
        c_.localSet(result);
        c_.end();
        c_.localGet(result);
    }

    void emitExpr(uint32_t idx) {
        const Expr& e = p_.exprs[idx];
        switch (e.k) {
        case EK::ConstNum:
            c_.f64Const(e.num);
            return;
        case EK::ConstText:
        case EK::StringJoin:
        case EK::StringLetter:
        case EK::ListContents: {
            emitTaggedExpr(idx);
            c_.localSet(kBits);
            emitImmediateToNumber();
            return;
        }
        case EK::StringContains:
            emitTaggedBinaryHost(e, 20, true);
            return;
        case EK::StringLengthValue: {
            if (!p_.useHostMath) {
                bail();
                c_.f64Const(0);
                return;
            }
            const uint32_t scratch = uint32_t(
                (p_.slotIds.size() + 1) * ir::kSlotBytes);
            emitTaggedExpr(e.a);
            c_.localSet(kBits);
            c_.localGet(0);
            c_.localGet(kBits);
            c_.i64Store(3, scratch);
            c_.i32Const(16);
            c_.localGet(0);
            c_.i32Const(int32_t(scratch));
            c_.i32Add();
            c_.i32Const(0);
            c_.call(hostHelper_);
            c_.i32Eqz();
            requireGuard(ir::GuardReason::NumericValue);
            c_.localGet(0);
            c_.f64Load(3, scratch);
            return;
        }
        case EK::StringLength: {
            if (!p_.useHostMath) {
                bail();
                c_.f64Const(0.0);
                return;
            }
            // Pass the original tagged Value, not a lossy f64 conversion.
            // The owning Target::Variable keeps a heap-string pointer rooted
            // while the host helper computes its UTF-16 length.
            const uint32_t scratch = uint32_t(
                (p_.slotIds.size() + 1) * sizeof(uint64_t));
            emitRawSlotLoad(e.var);
            c_.localSet(kBits);
            c_.localGet(0);
            c_.localGet(kBits);
            c_.i64Store(3, scratch);
            c_.i32Const(16);
            c_.localGet(0);
            c_.i32Const(int32_t(scratch));
            c_.i32Add();
            c_.i32Const(0);
            c_.call(hostHelper_);
            c_.i32Eqz();
            requireGuard(ir::GuardReason::NumericValue);
            c_.localGet(0);
            c_.f64Load(3, scratch);
            return;
        }
        case EK::ProcedureArgument:
            // Arguments are captured as exact Scratch Value bits (i64).
            // Only arithmetic consumers perform Scratch ToNumber coercion.
            emitTaggedExpr(idx);
            c_.localSet(kBits);
            emitImmediateToNumber();
            return;
        case EK::DaysSince2000: {
            if (!p_.useHostMath) {
                bail();
                c_.f64Const(0.0);
                return;
            }
            const uint32_t scratch = uint32_t(
                (p_.slotIds.size() + 1) * sizeof(uint64_t));
            c_.i32Const(15);
            c_.localGet(0);
            c_.i32Const(int32_t(scratch));
            c_.i32Add();
            c_.i32Const(0);
            c_.call(hostHelper_);
            c_.i32Eqz();
            requireGuard(ir::GuardReason::NumericValue);
            c_.localGet(0);
            c_.f64Load(3, scratch);
            return;
        }
        case EK::ListLength:
        case EK::ListContains:
        case EK::ListIndexOf: {
            if (!p_.useHostMath) {
                bail();
                c_.f64Const(0.0);
                return;
            }
            const uint32_t scratch = uint32_t(
                (p_.slotIds.size() + 1) * ir::kSlotBytes);
            const uint32_t needle = nextLocal(ValType::I64);
            if (e.k == EK::ListLength) c_.i64Const(0);
            else emitTaggedExpr(e.a);
            c_.localSet(needle);
            c_.localGet(0);
            c_.localGet(needle);
            c_.i64Store(3, scratch);
            // Mixed-type contains uses Scratch's SameValueZero/equality
            // semantics; indexOf uses Scratch equality alone. The numeric
            // fast path remains inside NumericListTransaction for the
            // existing dedicated numeric list ABI tests.
            c_.i32Const(e.k == EK::ListLength ? 10 :
                        e.k == EK::ListContains ? 24 : 25);
            c_.localGet(0);
            c_.i32Const(int32_t(scratch));
            c_.i32Add();
            c_.i32Const(int32_t(e.var));
            c_.call(hostHelper_);
            c_.i32Eqz();
            requireGuard(ir::GuardReason::NumericValue);
            c_.localGet(0);
            c_.f64Load(3, scratch);
            return;
        }
        case EK::ListGet: {
            emitTaggedExpr(idx);
            c_.localSet(kBits);
            emitImmediateToNumber();
            return;
        }
        case EK::VarGet: {
            emitRawSlotLoad(e.var);
            c_.localSet(kBits);
            // Numeric consumers use Scratch Cast.toNumber, not a strict
            // number-only guard. This handles booleans, colors, empty
            // strings and exact host UTF-16 string parsing. Ordered/equality
            // comparisons separately guard their direct operands.
            emitImmediateToNumber();
            return;
        }
        case EK::Add:
        case EK::Sub:
        case EK::Mul:
        case EK::Div: {
            // Scratch coercion precedes the backend-neutral F64 SSA graph.
            const uint32_t lhs = nextLocal(ValType::F64);
            const uint32_t rhs = nextLocal(ValType::F64);
            emitExpr(e.a);
            nanToZero();
            c_.localSet(lhs);
            emitExpr(e.b);
            nanToZero();
            c_.localSet(rhs);
            const auto op = e.k == EK::Add ? ir::Op::Add :
                            e.k == EK::Sub ? ir::Op::Sub :
                            e.k == EK::Mul ? ir::Op::Mul : ir::Op::Div;
            const ir::Function& graph = ir::binaryFunction(op);
            struct Lowerer {
                Code& code;
                uint32_t args[2];
                uint32_t next;
                uint32_t parameter(uint32_t index) { return args[index]; }
                uint32_t binary(ir::Op op, uint32_t a, uint32_t b) {
                    const uint32_t result = next++;
                    code.local(ValType::F64);
                    code.localGet(a);
                    code.localGet(b);
                    switch (op) {
                    case ir::Op::Add: code.f64Add(); break;
                    case ir::Op::Sub: code.f64Sub(); break;
                    case ir::Op::Mul: code.f64Mul(); break;
                    case ir::Op::Div: code.f64Div(); break;
                    default: code.unreachable(); break;
                    }
                    code.localSet(result);
                    return result;
                }
            } lowerer{c_, {lhs, rhs}, next_};
            const uint32_t result = ir::lower(graph, lowerer, 2);
            next_ = lowerer.next;
            c_.localGet(result);
            return;
        }
        case EK::Mod: {
            if (p_.useHostMath) {
                // WebAssembly has no floating-point remainder instruction.
                // Subtracting trunc(n/m)*m is NOT equivalent to std::fmod:
                // for (-10,-0.02) it can change the answer. Use the exact
                // Scratch host runtime implementation through env.h instead.
                const uint32_t lhs = nextLocal(ValType::F64);
                const uint32_t rhs = nextLocal(ValType::F64);
                emitExpr(e.a);
                nanToZero();
                c_.localSet(lhs);
                emitExpr(e.b);
                nanToZero();
                c_.localSet(rhs);
                // Runtime reserves slots[n] for a synthetic loop count and
                // slots[n+1],slots[n+2] for this helper's f64 operands.
                const uint32_t scratch = uint32_t(
                    (p_.slotIds.size() + 1) * sizeof(uint64_t));
                c_.localGet(0);
                c_.localGet(lhs);
                c_.f64Store(3, scratch);
                c_.localGet(0);
                c_.localGet(rhs);
                c_.f64Store(3, scratch + 8);
                c_.i32Const(3); // exact mod opcode
                c_.localGet(0);
                c_.i32Const(int32_t(scratch));
                c_.i32Add();
                c_.i32Const(0);
                c_.call(hostHelper_);
                c_.drop();
                c_.localGet(0);
                c_.f64Load(3, scratch);
                return;
            }
            // Standalone WASM has no host helper. Retain the integer-only
            // inline path with strict guards; for fractional/huge values
            // bail explicitly rather than emit an imprecise answer.
            // Scratch: r = n % m; if (r / m < 0) r += m;（JS の % そのもの
            // ではない。負の剰余を m 側へ寄せる）。
            const uint32_t la = nextLocal(ValType::F64),
                           lb = nextLocal(ValType::F64),
                           lt = nextLocal(ValType::F64);
            emitExpr(e.a);
            nanToZero();
            emitExpr(e.b);
            nanToZero();
            c_.localSet(lb);
            c_.localSet(la);
            // Quotient-rounded subtraction is reliable only for small
            // integral inputs. Any other case needs the runtime fmod
            // helper (or an interpreter fallback).
            for (uint32_t v : {la, lb}) {
                c_.localGet(v);
                c_.localGet(v);
                c_.f64Trunc();
                c_.f64Ne();
                c_.if_();
                bail();
                c_.end();
                c_.localGet(v);
                c_.f64Abs();
                c_.f64Const(1048576.0);
                c_.f64Ge();
                c_.if_();
                bail();
                c_.end();
            }
            c_.localGet(lb);
            c_.f64Const(0.0);
            c_.f64Eq();
            c_.if_();
            bail();
            c_.end();
            c_.localGet(la);
            c_.localGet(lb);
            c_.f64Div();
            c_.f64Trunc();
            c_.localGet(lb);
            c_.f64Mul();
            c_.localSet(lt);
            // lt = a - trunc(a/b)*b  (= JS %)
            c_.localGet(la);
            c_.localGet(lt);
            c_.f64Sub();
            c_.localSet(lt);
            // select(lt/lb < 0, lt+lb, lt)
            c_.localGet(lt);
            c_.localGet(lb);
            c_.f64Add();
            c_.localGet(lt);
            c_.localGet(lt);
            c_.localGet(lb);
            c_.f64Div();
            c_.f64Const(0.0);
            c_.f64Lt();
            c_.select();
            return;
        }
        case EK::LessThan:
        case EK::GreaterThan:
        case EK::Equals: {
            if (p_.useHostMath) {
                // Keep the numeric JIT fast path fully inside WebAssembly;
                // use the original Scratch comparison helper only if one
                // Value has a nonnumeric tag or canonical NaN. This avoids
                // the high JS/C++ host-call cost in hot numeric loops while
                // preserving lexical string comparisons and typed booleans.
                const uint32_t left = nextLocal(ValType::I64);
                const uint32_t right = nextLocal(ValType::I64);
                const uint32_t result = nextLocal(ValType::F64);
                const uint32_t scratch = uint32_t(
                    (p_.slotIds.size() + 1) * ir::kSlotBytes);
                emitTaggedExpr(e.a);
                c_.localSet(left);
                emitTaggedExpr(e.b);
                c_.localSet(right);
                emitComparableNumberPredicate(left);
                emitComparableNumberPredicate(right);
                c_.i32And();
                c_.if_();
                c_.localGet(left);
                c_.f64ReinterpretI64();
                c_.localGet(right);
                c_.f64ReinterpretI64();
                if (e.k == EK::LessThan) c_.f64Lt();
                else if (e.k == EK::GreaterThan) c_.f64Gt();
                else c_.f64Eq();
                c_.f64ConvertI32U();
                c_.localSet(result);
                c_.else_();
                c_.localGet(0);
                c_.localGet(left);
                c_.i64Store(3, scratch);
                c_.localGet(0);
                c_.localGet(right);
                c_.i64Store(3, scratch + 8);
                c_.i32Const(17);
                c_.localGet(0);
                c_.i32Const(int32_t(scratch));
                c_.i32Add();
                c_.i32Const(e.k == EK::LessThan ? 0 :
                            e.k == EK::GreaterThan ? 1 : 2);
                c_.call(hostHelper_);
                c_.i32Eqz();
                requireGuard(ir::GuardReason::OrderedComparison);
                c_.localGet(0);
                c_.f64Load(3, scratch);
                c_.localSet(result);
                c_.end();
                c_.localGet(result);
                return;
            }
            const uint32_t a = nextLocal(ValType::F64);
            const uint32_t b = nextLocal(ValType::F64);
            // Scratch comparisons may fall back to lexicographic string
            // ordering. Number-coercing a direct string variable to 0
            // would conflate e.g. "apple" and "banana". Check the CURRENT
            // speculative slot, not its type when the script started.
            const auto numericOperand = [&](uint32_t index) {
                if (index >= p_.exprs.size()) return;
                const auto& child = p_.exprs[index];
                if (child.k == EK::VarGet) {
                    emitRawSlotLoad(child.var);
                    c_.localSet(kBits);
                    emitNumericBitsGuard();
                }
            };
            numericOperand(e.a);
            numericOperand(e.b);
            emitExpr(e.a);
            c_.localSet(a);
            emitExpr(e.b);
            c_.localSet(b);
            // Scratch Cast.compare falls back to string comparison for NaN.
            // We cannot represent that fast path, so bail and replay via the
            // interpreter rather than silently using IEEE NaN comparisons.
            c_.localGet(a);
            c_.localGet(a);
            c_.f64Eq();
            c_.localGet(b);
            c_.localGet(b);
            c_.f64Eq();
            c_.i32And();
            requireGuard(ir::GuardReason::OrderedComparison);
            if (e.k == EK::Equals) {
                c_.localGet(a);
                c_.localGet(b);
                c_.f64Eq();
                c_.f64ConvertI32U();
                return;
            }
            const ir::Op op = e.k == EK::LessThan
                ? ir::Op::LessThan : ir::Op::GreaterThan;
            const ir::Function& graph = ir::binaryFunction(op);
            struct Lowerer {
                Code& code;
                uint32_t args[2];
                uint32_t next;
                uint32_t parameter(uint32_t i) { return args[i]; }
                uint32_t binary(ir::Op op, uint32_t lhs, uint32_t rhs) {
                    const uint32_t result = next++;
                    code.local(op == ir::Op::LessThan ||
                               op == ir::Op::GreaterThan
                                   ? ValType::I32 : ValType::F64);
                    code.localGet(lhs);
                    code.localGet(rhs);
                    if (op == ir::Op::LessThan) code.f64Lt();
                    else if (op == ir::Op::GreaterThan) code.f64Gt();
                    else code.unreachable();
                    code.localSet(result);
                    return result;
                }
            } lowerer{c_, {a, b}, next_};
            const uint32_t result = ir::lower(graph, lowerer, 2);
            next_ = lowerer.next;
            c_.localGet(result);
            c_.f64ConvertI32U();
            return;
        }
        case EK::And:
        case EK::Or: {
            emitTruthyExpr(e.a);
            emitTruthyExpr(e.b);
            if (e.k == EK::And) c_.i32And();
            else c_.i32Or();
            c_.f64ConvertI32U();
            return;
        }
        case EK::Not: {
            emitTruthyExpr(e.a);
            c_.i32Eqz();
            c_.f64ConvertI32U();
            return;
        }
        case EK::Neg:
            c_.f64Const(0.0);
            emitExpr(e.a);
            c_.f64Sub();
            return;
        case EK::MathOp: {
            // The primitive's NUM argument is cast to Scratch Number first.
            emitExpr(e.a);
            nanToZero();
            switch (e.var) {
            case 0: c_.f64Abs(); return;
            case 1: c_.f64Floor(); return;
            case 2: c_.f64Ceil(); return;
            case 3: c_.f64Sqrt(); return;
            case 255: c_.drop(); c_.f64Const(0.0); return;
            default: break;
            }
            // The remaining operations use V8-compatible math and Scratch
            // rounding. A standalone module without env.h must bail, not
            // silently use libc/libm approximations.
            if (!p_.useHostMath) {
                c_.drop();
                bail();
                c_.f64Const(0.0); // unreachable stack type after return
                return;
            }
            const uint32_t x = nextLocal(ValType::F64);
            const uint32_t scratch = uint32_t(
                (p_.slotIds.size() + 1) * sizeof(uint64_t));
            c_.localSet(x);
            c_.localGet(0);
            c_.localGet(x);
            c_.f64Store(3, scratch);
            c_.i32Const(4); // Scratch mathop runtime helper
            c_.localGet(0);
            c_.i32Const(int32_t(scratch));
            c_.i32Add();
            c_.i32Const(int32_t(e.var));
            c_.call(hostHelper_);
            c_.drop();
            c_.localGet(0);
            c_.f64Load(3, scratch);
            return;
        }
        case EK::Round:
            // Scratch's operator_round uses num(arg), which turns NaN into
            // zero BEFORE calling JS Math.round. Directly rounding the
            // intermediate f64 would incorrectly store NaN.
            emitExpr(e.a);
            nanToZero();
            emitJsRoundF64();
            return;
        }
    }

    /// スタック上の f64 を JS Math.round し、f64 を 1 つ残す。
    /// floor(x+0.5) だけでは 0.49999999999999994 が 1 になり、-0 も失う。
    // Numeric Scratch truth: zero and NaN are false; all other numbers
    // including +/-Infinity are true. Consumes f64, leaves i32.
    void truthy() {
        c_.localSet(kTmp);
        c_.localGet(kTmp);
        c_.f64Const(0.0);
        c_.f64Ne();
        c_.localGet(kTmp);
        c_.localGet(kTmp);
        c_.f64Eq();
        c_.i32And();
    }

    void emitJsRoundF64() {
        const uint32_t a = nextLocal(ValType::F64),
                       r = nextLocal(ValType::F64);
        c_.localSet(a);
        c_.localGet(a);
        c_.f64Const(0.5);
        c_.f64Add();
        c_.f64Floor();
        c_.localSet(r);
        // |a| >= 2^52 -> a
        c_.localGet(a);
        c_.localGet(r);
        c_.localGet(a);
        c_.f64Const(4503599627370496.0);
        c_.f64Ge();
        c_.localGet(a);
        c_.f64Const(-4503599627370496.0);
        c_.f64Le();
        c_.i32Or();
        c_.select();
        c_.localSet(r);
        // 0 <= a < 0.5 -> +0
        c_.f64Const(0.0);
        c_.localGet(r);
        c_.localGet(a);
        c_.i64ReinterpretF64();
        c_.i64Const(0);
        c_.i64LtS();
        c_.i32Eqz();
        c_.localGet(a);
        c_.f64Const(0.5);
        c_.f64Lt();
        c_.i32And();
        c_.select();
        c_.localSet(r);
        // -0.5 <= a < 0 -> -0
        c_.f64Const(-0.0);
        c_.localGet(r);
        c_.localGet(a);
        c_.i64ReinterpretF64();
        c_.i64Const(0);
        c_.i64LtS();
        c_.localGet(a);
        c_.f64Const(-0.5);
        c_.f64Ge();
        c_.i32And();
        c_.select();
        c_.localSet(r);
        c_.localGet(r);
    }

    void emitStmt(uint32_t idx) {
        const Stmt& s = p_.stmts[idx];
        switch (s.k) {
        case SK::If:
            // Keep single-arm conditionals compact: no unnecessary else.
            emitTruthyExpr(s.expr);
            c_.if_();
            emitStmts(s.body);
            c_.end();
            return;
        case SK::IfElse: {
            emitTruthyExpr(s.expr);
            // wasm is structurally nested: the two CFG edges map to
            // if/else/end, with no native Phi requirement for void bodies.
            struct DiamondLowerer {
                Code& code;
                void branch(int, ir::BlockId, ir::BlockId, ir::BlockId) {
                    code.if_();
                }
                void otherwise(ir::BlockId) { code.else_(); }
                void finish(ir::BlockId) { code.end(); }
            } lowered{c_};
            static const ir::Diamond diamond;
            ir::lowerDiamondVoid(
                diamond, lowered, 0,
                [&]() { emitStmts(s.body); },
                [&]() {
                    if (s.k == SK::IfElse) emitStmts(s.elseBody);
                });
            return;
        }
        case SK::ListAppend:
        case SK::ListReplace:
        case SK::ListDelete:
        case SK::ListDeleteAll:
        case SK::ListInsert: {
            if (!p_.useHostMath) { bail(); return; }
            const uint32_t scratch = uint32_t(
                (p_.slotIds.size() + 1) * sizeof(uint64_t));
            // Scratch's input evaluation precedes any side effect. Keep
            // index and value in distinct locals: expression lowering may
            // reuse kTmp, so the helper arguments are written afterwards.
            const uint32_t index = nextLocal(ValType::I64);
            const uint32_t value = nextLocal(ValType::I64);
            if (s.k == SK::ListAppend || s.k == SK::ListDeleteAll)
                c_.i64Const(0);
            else
                emitTaggedExpr(s.expr);
            c_.localSet(index);
            if (s.k == SK::ListDelete || s.k == SK::ListDeleteAll) {
                c_.i64Const(0);
            } else {
                // Scratch lists store exact Value tags. The transaction
                // journal owns tentative numeric/string/boolean values;
                // the GC is deferred until a successful segment commit.
                emitTaggedExpr(s.extraExpr);
            }
            c_.localSet(value);
            c_.localGet(0);
            c_.localGet(index);
            c_.i64Store(3, scratch);
            c_.localGet(0);
            c_.localGet(value);
            c_.i64Store(3, scratch + 8);
            c_.i32Const(s.k == SK::ListAppend ? 7 :
                        s.k == SK::ListReplace ? 8 :
                        s.k == SK::ListDelete ? 9 :
                        s.k == SK::ListDeleteAll ? 13 : 14);
            c_.localGet(0);
            c_.i32Const(int32_t(scratch));
            c_.i32Add();
            c_.i32Const(int32_t(s.var));
            c_.call(hostHelper_);
            c_.i32Eqz();
            requireGuard(ir::GuardReason::NumericValue);
            return;
        }
        case SK::InlineProcedure: {
            // Evaluate all arguments before entering the callee. Scratch
            // procedure arguments are captured values, not lazy expressions:
            // the callee may modify the variables used by an argument.
            if (s.argumentExprs.size() != s.argumentSlots.size()) {
                bail();
                return;
            }
            for (size_t i = 0; i < s.argumentExprs.size(); ++i) {
                // Argument evaluation is eager exactly once. Capture the
                // tagged Value, *not* a converted double: Scratch procedures
                // can pass strings, booleans and numeric-looking strings
                // onward to variables, lists and string reporters unchanged.
                emitTaggedExpr(s.argumentExprs[i]);
                const uint32_t local = nextLocal(ValType::I64);
                c_.localSet(local);
                argumentLocals_[s.argumentSlots[i]] = local;
            }
            emitStmts(s.body);
            for (uint32_t arg : s.argumentSlots)
                argumentLocals_.erase(arg);
            return;
        }
        case SK::SetVar:
            emitExpr(s.expr);
            storeF64(s.var);
            return;
        case SK::SetVarValue:
            emitTaggedExpr(s.expr);
            c_.localSet(kBits);
            emitRawSlotStore(s.var);
            return;
        case SK::SetVarRaw:
            // Scratch set-variable from another variable is an *exact*
            // Value copy. Numeric coercion here would destroy strings,
            // booleans, colors and -0/NaN identity.
            emitRawSlotLoad(s.extraExpr);
            c_.localSet(kBits);
            emitRawSlotStore(s.var);
            return;
        case SK::SetVarBoolean:
            // Boolean reporters in Scratch are tagged true/false Values,
            // not numeric 1/0. Preserve this distinction on writeback.
            emitTruthyExpr(s.expr);
            c_.if_();
            c_.i64Const(int64_t(val::kTrue));
            c_.localSet(kBits);
            c_.else_();
            c_.i64Const(int64_t(val::kFalse));
            c_.localSet(kBits);
            c_.end();
            emitRawSlotStore(s.var);
            return;
        case SK::ChangeVar: {
            emitRawSlotLoad(s.var);
            c_.localSet(kBits);
            emitImmediateToNumber();
            emitExpr(s.expr);
            nanToZero();
            c_.f64Add();
            storeF64(s.var);
            return;
        }
        case SK::WarpReturnPcStack: {
            const size_t nArgs = s.argumentSlots.size();
            const size_t nSites = s.returnSites.size();
            if (!p_.allowBoundedConditionalLoop || nSites == 0 ||
                nSites > 2 || nArgs > 2 || s.elseBody.empty()) {
                bail();
                return;
            }
            for (uint32_t slot : s.argumentSlots) {
                if (argumentLocals_.find(slot) == argumentLocals_.end()) {
                    bail();
                    return;
                }
            }
            for (const auto& site : s.returnSites) {
                if (site.before.empty() ||
                    site.nextArguments.size() != nArgs ||
                    site.condition >= p_.exprs.size()) {
                    bail();
                    return;
                }
            }

            // The host provides a private buffer of copied scalar slots,
            // three scratch values, then 1024 frames. Never write into
            // shared WebAssembly memory outside that reservation.
            constexpr uint32_t kCapacity =
                ir::AtomicWarpExecutionAbi::kMaxRecursionFrames;
            const uint32_t stride = ir::CallFrameAbi::byteOffset(
                ir::CallFrameAbi::frameWords(uint32_t(nArgs), 0));
            const uint32_t byteOffset =
                ir::AtomicWarpExecutionAbi::frameOffsetBytes(
                    uint32_t(p_.slotIds.size()));
            const uint32_t pc = nextLocal(ValType::I32);
            const uint32_t depth = nextLocal(ValType::I32);
            const uint32_t frameAddress = nextLocal(ValType::I32);
            const auto addressForDepth = [&]() {
                c_.localGet(0);
                c_.i32Const(int32_t(byteOffset));
                c_.i32Add();
                c_.localGet(depth);
                c_.i32Const(int32_t(stride));
                c_.i32Mul();
                c_.i32Add();
                c_.localSet(frameAddress);
            };
            c_.i32Const(0);
            c_.localSet(pc);
            c_.i32Const(0);
            c_.localSet(depth);
            c_.block();
            c_.loop();
            for (size_t i = 0; i < nSites; ++i) {
                const auto& site = s.returnSites[i];
                c_.localGet(pc);
                c_.i32Const(int32_t(i));
                c_.i32Eq();
                c_.if_();
                if (i == 0) {
                    // Every entered recursive frame spends shared fuel.
                    // Nested loops spend the very same fuel. 1024 pushes
                    // fit exactly into the host-reserved frame array.
                    c_.localGet(loopFuel_);
                    c_.i32Eqz();
                    c_.if_();
                    bail();
                    c_.end();
                    c_.localGet(loopFuel_);
                    c_.i32Const(1);
                    c_.i32Sub();
                    c_.localSet(loopFuel_);
                }
                emitStmts(site.before);
                emitTruthyExpr(site.condition);
                c_.if_();
                c_.localGet(depth);
                c_.i32Const(int32_t(kCapacity));
                c_.i32LtU();
                c_.i32Eqz();
                c_.if_();
                bail();
                c_.end();
                addressForDepth();
                // The same CallFrameAbi header is understood by native
                // and WASM schedulers. For this restricted single-proc
                // warp recursion, procedure ID 1 denotes the callee,
                // resume PC is the callee entry and there are no SSA
                // locals beyond the saved numeric formal arguments.
                const auto storeFrameWord = [&](uint32_t field,
                                                uint64_t value) {
                    c_.localGet(frameAddress);
                    c_.i64Const(std::bit_cast<int64_t>(value));
                    c_.i64Store(3, ir::CallFrameAbi::byteOffset(field));
                };
                storeFrameWord(ir::CallFrameAbi::kProcedureIdWord, 1);
                storeFrameWord(ir::CallFrameAbi::kReturnPcWord, i + 1);
                storeFrameWord(ir::CallFrameAbi::kResumePcWord, 0);
                storeFrameWord(ir::CallFrameAbi::kArgumentCountWord, nArgs);
                storeFrameWord(ir::CallFrameAbi::kLocalCountWord, 0);
                for (size_t arg = 0; arg < nArgs; ++arg) {
                    c_.localGet(frameAddress);
                    c_.localGet(argumentLocals_.at(s.argumentSlots[arg]));
                    c_.i64Store(3, ir::CallFrameAbi::byteOffset(
                        ir::CallFrameAbi::argumentWord(uint32_t(arg))));
                }
                // Preserve simultaneous argument evaluation. In
                // particular f(a,b) -> f(b,a+b) reads the old a,b.
                std::vector<uint32_t> nextArgs;
                nextArgs.reserve(nArgs);
                for (const uint32_t expr : site.nextArguments) {
                    emitTaggedExpr(expr);
                    const uint32_t local = nextLocal(ValType::I64);
                    c_.localSet(local);
                    nextArgs.push_back(local);
                }
                for (size_t arg = 0; arg < nArgs; ++arg) {
                    c_.localGet(nextArgs[arg]);
                    c_.localSet(argumentLocals_.at(s.argumentSlots[arg]));
                }
                c_.localGet(depth);
                c_.i32Const(1);
                c_.i32Add();
                c_.localSet(depth);
                c_.i32Const(0);
                c_.localSet(pc); // enter the recursive procedure
                c_.else_();
                c_.i32Const(int32_t(i + 1));
                c_.localSet(pc); // continue in this frame
                c_.end();
                c_.end();
            }
            c_.localGet(pc);
            c_.i32Const(int32_t(nSites));
            c_.i32Eq();
            c_.if_();
            emitStmts(s.elseBody);
            c_.localGet(depth);
            c_.i32Eqz();
            c_.if_();
            c_.i32Const(int32_t(nSites + 1));
            c_.localSet(pc);
            c_.else_();
            c_.localGet(depth);
            c_.i32Const(1);
            c_.i32Sub();
            c_.localSet(depth);
            addressForDepth();
            c_.localGet(frameAddress);
            c_.i64Load(3, ir::CallFrameAbi::byteOffset(
                ir::CallFrameAbi::kReturnPcWord));
            c_.i32WrapI64();
            c_.localSet(pc); // restore the caller's return PC
            for (size_t arg = 0; arg < nArgs; ++arg) {
                c_.localGet(frameAddress);
                c_.i64Load(3, ir::CallFrameAbi::byteOffset(
                    ir::CallFrameAbi::argumentWord(uint32_t(arg))));
                c_.localSet(argumentLocals_.at(s.argumentSlots[arg]));
            }
            c_.end();
            c_.end();
            c_.localGet(pc);
            c_.i32Const(int32_t(nSites + 1));
            c_.i32Eq();
            c_.brIf(1); // leave the loop and the enclosing block
            c_.br(0);
            c_.end();
            c_.end();
            return;
        }
        case SK::WarpReturnStack: {
            if (!p_.allowBoundedConditionalLoop || s.body.empty() ||
                s.elseBody.empty() || !s.argumentExprs.empty() ||
                !s.argumentSlots.empty()) {
                bail();
                return;
            }
            // The verified canonical recursion has exactly one return
            // program counter: the suffix following its self-recursive IF.
            // Represent the LIFO continuation stack compactly as a depth.
            // The first loop pushes an identical suffix continuation for
            // each entered frame; the unwind loop pops and executes it in
            // reverse order. More general call sites need an actual PC
            // stack, and are rejected by the source-IR verifier.
            const uint32_t returnDepth = nextLocal(ValType::I32);
            c_.i32Const(0);
            c_.localSet(returnDepth);
            c_.block();
            c_.loop();
            c_.localGet(loopFuel_);
            c_.i32Eqz();
            c_.if_();
            bail(); // Roll back the *entire* speculative scalar transaction.
            c_.end();
            c_.localGet(loopFuel_);
            c_.i32Const(1);
            c_.i32Sub();
            c_.localSet(loopFuel_);
            c_.localGet(returnDepth);
            c_.i32Const(1);
            c_.i32Add();
            c_.localSet(returnDepth); // push return continuation
            emitStmts(s.body);
            emitTruthyExpr(s.expr);
            c_.brIf(0); // recurse, keeping pushed return continuation
            c_.end();
            c_.end();
            // Unwind the call frames. The deepest frame executes its
            // suffix first, matching Scratch's recursive return order.
            c_.block();
            c_.loop();
            c_.localGet(returnDepth);
            c_.i32Eqz();
            c_.brIf(1);
            emitStmts(s.elseBody);
            c_.localGet(returnDepth);
            c_.i32Const(1);
            c_.i32Sub();
            c_.localSet(returnDepth); // pop
            c_.br(0);
            c_.end();
            c_.end();
            return;
        }
        case SK::WarpTailRecurse: {
            if (!p_.allowBoundedConditionalLoop ||
                s.argumentExprs.size() != s.argumentSlots.size()) {
                bail();
                return;
            }
            for (uint32_t slot : s.argumentSlots) {
                if (argumentLocals_.find(slot) == argumentLocals_.end()) {
                    bail();
                    return;
                }
            }
            // A verified numeric warp function whose last statement is
            // "if (cond) call self" is equivalent to a do/while.
            // Every procedure entry consumes the same shared fuel as
            // the surrounding numeric control tree. Because only private
            // scalar slots are touched, exhaustion discards all progress
            // and restores the interpreter's original recursive call.
            c_.loop();
            c_.localGet(loopFuel_);
            c_.i32Eqz();
            c_.if_();
            bail();
            c_.end();
            c_.localGet(loopFuel_);
            c_.i32Const(1);
            c_.i32Sub();
            c_.localSet(loopFuel_);
            emitStmts(s.body);
            emitTruthyExpr(s.expr);
            if (s.argumentExprs.empty()) {
                c_.brIf(0);
            } else {
                // Recursive call arguments are evaluated eagerly, from
                // the OLD frame. Store them in temporary i64 registers
                // first, then update the persistent formal argument
                // registers and branch back to the loop header.
                c_.if_();
                std::vector<uint32_t> nextArguments;
                nextArguments.reserve(s.argumentExprs.size());
                for (const uint32_t argExpr : s.argumentExprs) {
                    emitTaggedExpr(argExpr);
                    const uint32_t fresh = nextLocal(ValType::I64);
                    c_.localSet(fresh);
                    nextArguments.push_back(fresh);
                }
                for (size_t i = 0; i < nextArguments.size(); ++i) {
                    c_.localGet(nextArguments[i]);
                    c_.localSet(argumentLocals_.at(s.argumentSlots[i]));
                }
                c_.br(1);
                c_.end();
            }
            c_.end();
            return;
        }
        case SK::Until:
        case SK::While: {
            if (!p_.allowBoundedConditionalLoop) {
                // Normal entrypoints have no scheduler continuation for
                // these loops. Never change their previous bailout contract.
                bail();
                return;
            }
            // Only the nested warp-only verifier enables this lowering.
            // A bounded, side-effect-free numeric condition and body are
            // evaluated from speculative scalar slots. The top-level loop
            // is atomic: a guard failure leaves all variables unchanged.
            //
            // block exit
            //   loop header
            //     if (until: condition true / while: condition false)
            //         br exit
            //     if fuel exhausted: return bail (no commit)
            //     body; fuel--; br header
            //   end
            // end
            c_.block();
            c_.loop();
            emitTruthyExpr(s.expr);
            if (s.k == SK::While) c_.i32Eqz();
            c_.brIf(1);
            c_.localGet(loopFuel_);
            c_.i32Eqz();
            c_.if_();
            bail();
            c_.end();
            c_.localGet(loopFuel_);
            c_.i32Const(1);
            c_.i32Sub();
            c_.localSet(loopFuel_);
            emitStmts(s.body);
            c_.br(0);
            c_.end();
            c_.end();
            return;
        }
        case SK::ForEach: {
            if (!p_.allowBoundedConditionalLoop) {
                bail();
                return;
            }
            // Scratch control_for_each owns an internal iteration counter
            // independent of the named variable (the body may modify it).
            // Re-evaluate its VALUE reporter on *each* header check just
            // like the interpreter. On every entered iteration, publish
            // counter+1 to the speculative scalar slot before the body.
            const uint32_t iteration = nextLocal(ValType::F64);
            c_.f64Const(0.0);
            c_.localSet(iteration);
            c_.block();
            c_.loop();
            c_.localGet(iteration);
            emitExpr(s.expr);
            c_.f64Lt();
            c_.i32Eqz();
            c_.brIf(1);
            c_.localGet(loopFuel_);
            c_.i32Eqz();
            c_.if_();
            bail();
            c_.end();
            c_.localGet(loopFuel_);
            c_.i32Const(1);
            c_.i32Sub();
            c_.localSet(loopFuel_);
            c_.localGet(iteration);
            c_.f64Const(1.0);
            c_.f64Add();
            c_.localTee(iteration);
            storeF64(s.var);
            emitStmts(s.body);
            c_.br(0);
            c_.end();
            c_.end();
            return;
        }
        case SK::Forever:
            // Cannot complete in a finite bounded transaction.
            bail();
            return;
        case SK::Repeat: {
            emitExpr(s.expr);
            nanToZero();
            c_.localSet(kTmp);
            c_.localGet(kTmp);
            c_.f64Const(2147483647.0);
            c_.f64Le();
            requireGuard(ir::GuardReason::LoopCount);
            // d = Math.round(x)
            c_.localGet(kTmp);
            emitJsRoundF64();
            c_.localSet(kTmp);
            // A large count would consume the bounded loop fuel and then
            // discard the entire transaction after wasting 1024 iterations.
            // Bail *before* executing any body operation; the interpreter
            // will perform the unbounded loop with its exact tick budget.
            c_.localGet(kTmp);
            c_.f64Const(double(kMaxNestedIterations));
            c_.f64Le();
            requireGuard(ir::GuardReason::LoopCount);
            const uint32_t ln = nextLocal(ValType::I32),
                           li = nextLocal(ValType::I32);
            c_.localGet(kTmp);
            c_.f64Const(0.0);
            c_.f64Gt();
            c_.if_();
            c_.localGet(kTmp);
            c_.i32TruncF64S();
            c_.localSet(ln);
            c_.else_();
            c_.i32Const(0);
            c_.localSet(ln);
            c_.end();
            c_.i32Const(0);
            c_.localSet(li);
            // A backedge Phi requires a nonempty loop. Zero repetitions are
            // excluded before entering; loop-carried i32 local represents
            // the induction Phi in the WebAssembly stack machine.
            c_.localGet(ln);
            c_.i32Const(0);
            c_.i32GtS();
            c_.if_();
            struct LoopLowerer {
                Code& code;
                void begin(ir::BlockId) { code.loop(); }
                void latch(int, ir::BlockId, ir::BlockId) { code.brIf(0); }
                void finish(ir::BlockId) { code.end(); }
            } loopLowerer{c_};
            static const ir::BackedgeLoop loop(ir::Type::I32);
            ir::lowerBackedgeLoop(loop, loopLowerer, [&]() {
                c_.localGet(loopFuel_);
                c_.i32Eqz();
                c_.if_();
                bail();
                c_.end();
                c_.localGet(loopFuel_);
                c_.i32Const(1);
                c_.i32Sub();
                c_.localSet(loopFuel_);
                emitStmts(s.body);
                c_.localGet(li);
                c_.i32Const(1);
                c_.i32Add();
                c_.localSet(li);
                c_.localGet(li);
                c_.localGet(ln);
                c_.i32LtS();
                return 0; // the i32 condition remains on the WASM stack
            });
            c_.end(); // guarded nonempty loop
            return;
        }
        }
    }

    void emitStmts(const std::vector<uint32_t>& list) {
        for (uint32_t id : list) emitStmt(id);
    }

    const Program& p_;
    Code& c_;
    uint32_t hostHelper_ = 0;
    static constexpr int32_t kMaxNestedIterations = 1024;
    uint32_t next_ = kFirstFree;
    uint32_t loopFuel_ = 0;
    std::unordered_map<uint32_t, uint32_t> argumentLocals_;
};

} // namespace

std::vector<uint8_t> compileProgram(const Program& p) {
    // These loops cannot be emitted as one synchronous standalone function
    // without changing Scratch's scheduling behavior. Reject them rather
    // than silently treating them as an empty/one-shot instruction.
    std::vector<uint32_t> pending = p.main;
    std::unordered_set<uint32_t> visited;
    while (!pending.empty()) {
        const uint32_t index = pending.back();
        pending.pop_back();
        if (index >= p.stmts.size()) return {};
        if (!visited.insert(index).second) continue;
        const Stmt& stmt = p.stmts[index];
        if ((stmt.k == SK::Until || stmt.k == SK::While) &&
            !p.allowBoundedConditionalLoop) return {};
        if (stmt.k == SK::ForEach &&
            !p.allowBoundedConditionalLoop) return {};
        if ((stmt.k == SK::WarpTailRecurse ||
             stmt.k == SK::WarpReturnStack ||
             stmt.k == SK::WarpReturnPcStack) &&
            !p.allowBoundedConditionalLoop) return {};
        if (stmt.k == SK::Forever) return {};
        for (const auto& site : stmt.returnSites)
            pending.insert(pending.end(), site.before.begin(),
                           site.before.end());
        pending.insert(pending.end(), stmt.body.begin(), stmt.body.end());
        pending.insert(pending.end(), stmt.elseBody.begin(),
                       stmt.elseBody.end());
    }
    Module m;
    const uint32_t t = m.type({ValType::I32}, {ValType::I32});
    m.importMemory("env", "memory", 256);
    uint32_t hostHelper = 0;
    if (p.useHostMath) {
        const uint32_t helperType =
            m.type({ValType::I32, ValType::I32, ValType::I32},
                   {ValType::I32});
        hostHelper = m.importFunc("env", "h", helperType);
    }
    const uint32_t f = m.addFunc(t);
    m.exportFunc("f", f);
    Code& c = m.code(f);
    c.local(ValType::I64);   // 1
    c.local(ValType::F64);   // 2
    c.local(ValType::F64);   // 3

    Gen g(p, c, hostHelper);
    g.run();
    c.end();
    return m.finish();
}

// --- ブロック木 → Program ----------------------------------------------

namespace {

bool isNumShadow(const std::string& op) {
    return op == "math_number" || op == "math_integer" ||
           op == "math_whole_number" || op == "math_positive_number" ||
           op == "math_angle";
}

struct Lower {
    const ir::ScratchScript& b;
    const ir::ScratchEffects& effects;
    Program& p;
    std::map<std::string, uint32_t> vars;
    std::map<std::string, uint32_t> lists;
    // The argument names are local to the currently inlined procedure.
    // Each call allocates fresh register IDs to prevent lexical aliasing.
    std::map<std::u16string, uint32_t> activeArgumentSlots;
    std::unordered_set<std::string> activeProcedures;
    std::vector<std::string> activeWarpProcedures;
    uint32_t nextArgumentSlot = 0;
    size_t inlinedCalls = 0;
    size_t depth = 0;
    size_t visited = 0;
    struct DepthGuard {
        size_t& count;
        explicit DepthGuard(size_t& n) : count(n) { ++count; }
        ~DepthGuard() { --count; }
    };

    uint32_t slotFor(const std::string& id) {
        auto it = vars.find(id);
        if (it != vars.end()) return it->second;
        const uint32_t s = uint32_t(vars.size());
        vars.emplace(id, s);
        p.slotIds.push_back(id);
        if (s + 1 > p.nSlots) p.nSlots = s + 1;
        return s;
    }

    uint32_t listFor(const std::string& id) {
        const auto it = lists.find(id);
        if (it != lists.end()) return it->second;
        const uint32_t index = uint32_t(lists.size());
        lists.emplace(id, index);
        p.listIds.push_back(id);
        return index;
    }

    const Block* procedureDefinition(const std::string& proccode,
                                     const Block*& prototype) const {
        const Block* best = nullptr;
        prototype = nullptr;
        for (const auto& candidate : b.nodes) {
            if (candidate.opcode != "procedures_definition") continue;
            const BlockInput* input = candidate.inputs.find("custom_block");
            const Block* proto =
                input ? b.getBlock(input->block) : nullptr;
            if (!proto || !proto->mutation ||
                proto->mutation->attr("proccode") != proccode)
                continue;
            if (!best || candidate.seq < best->seq) {
                best = &candidate;
                prototype = proto;
            }
        }
        return best;
    }

    const Block* inputOf(const Block* blk, const char* name) {
        const auto* input = effects.inputOf(blk->id, name);
        if (!input) return nullptr;
        const std::string id = input->selectedId();
        return id.empty() ? nullptr : b.getBlock(id);
    }

    // Recognize only canonical numeric-argument *tail* recursion:
    //   pure numeric prefix;
    //   if (pure numeric condition) call this same warp procedure
    // with the recursive call as the only and last statement in the
    // final IF body. Non-tail recursion needs a real return stack and
    // is deliberately left for the interpreter.
    const Block* tailRecursiveIf(const Block* definition,
                                 const std::string& proccode,
                                 const Mutation& signature) const {
        if (!definition || definition->next.empty()) return nullptr;
        std::unordered_set<std::string> visitedBlocks;
        const Block* last = nullptr;
        std::string id = definition->next;
        while (!id.empty()) {
            if (visitedBlocks.size() >= 128 ||
                !visitedBlocks.insert(id).second) return nullptr;
            last = b.getBlock(id);
            if (!last) return nullptr;
            id = last->next;
        }
        if (visitedBlocks.size() < 2 || !last ||
            last->opcode != "control_if") return nullptr;
        const BlockInput* trueBranch = last->inputs.find("SUBSTACK");
        if (!trueBranch || trueBranch->block.empty()) return nullptr;
        const Block* recursive = b.getBlock(trueBranch->block);
        if (!recursive || recursive->opcode != "procedures_call" ||
            !recursive->mutation || recursive->mutation->isReporterCall() ||
            recursive->mutation->attr("proccode") != proccode ||
            !recursive->next.empty() ||
            recursive->inputs.items.size() != signature.argumentIds.size())
            return nullptr;
        for (const auto& id : signature.argumentIds)
            if (!recursive->inputs.find(id)) return nullptr;
        return last;
    }

    // One-return-site non-tail recursion:
    //   prefix; if (cond) call self(); suffix;
    // Reject any call arguments and calls with work inside the IF branch.
    // Every entered frame has the same return PC and can therefore use
    // a compact bounded LIFO continuation-depth representation.
    const Block* nonTailRecursiveIf(
        const Block* definition, const std::string& proccode) const {
        if (!definition || definition->next.empty()) return nullptr;
        std::unordered_set<std::string> seen;
        for (std::string id = definition->next; !id.empty();) {
            if (seen.size() >= 128 || !seen.insert(id).second)
                return nullptr;
            const Block* candidate = b.getBlock(id);
            if (!candidate) return nullptr;
            if (candidate->opcode == "control_if" &&
                !candidate->next.empty()) {
                const auto* branch = candidate->inputs.find("SUBSTACK");
                const Block* child = branch && !branch->block.empty()
                    ? b.getBlock(branch->block) : nullptr;
                if (child && child->opcode == "procedures_call" &&
                    child->mutation && !child->mutation->isReporterCall() &&
                    child->mutation->attr("proccode") == proccode &&
                    child->next.empty() && child->inputs.items.empty())
                    return candidate;
            }
            id = candidate->next;
        }
        return nullptr;
    }

    // Only top-level IF blocks containing one exact self call are eligible.
    // The enclosing numeric procedure may have up to two return PCs.
    std::vector<const Block*> recursiveReturnSites(
        const Block* definition, const std::string& code,
        const Mutation& signature) const {
        std::vector<const Block*> sites;
        if (!definition) return {};
        std::unordered_set<std::string> seen;
        for (std::string id = definition->next; !id.empty();) {
            if (seen.size() >= 128 || !seen.insert(id).second)
                return {};
            const Block* block = b.getBlock(id);
            if (!block) return {};
            if (block->opcode == "control_if") {
                const auto* branch = block->inputs.find("SUBSTACK");
                const Block* call = branch && !branch->block.empty()
                    ? b.getBlock(branch->block) : nullptr;
                if (call && call->opcode == "procedures_call" &&
                    call->mutation && !call->mutation->isReporterCall() &&
                    call->mutation->attr("proccode") == code &&
                    call->next.empty() &&
                    call->inputs.items.size() == signature.argumentIds.size()) {
                    bool complete = true;
                    for (const auto& argId : signature.argumentIds)
                        if (!call->inputs.find(argId)) complete = false;
                    if (!complete) return {};
                    sites.push_back(block);
                    if (sites.size() > 2) return {};
                }
            }
            id = block->next;
        }
        return sites;
    }

    bool lowerPcRecursiveWarp(
        const Block* definition, const Mutation& signature,
        const std::vector<uint32_t>& slots,
        const std::vector<const Block*>& sites,
        std::vector<uint32_t>& outBody) {
        if (!definition || sites.empty() || sites.size() > 2 ||
            slots.size() != signature.argumentIds.size() ||
            slots.size() > 2)
            return false;
        Stmt stack;
        stack.k = SK::WarpReturnPcStack;
        stack.inlineWarp = true;
        stack.argumentSlots = slots;
        std::string cursor = definition->next;
        for (const Block* siteBlock : sites) {
            WarpReturnSite site;
            std::unordered_set<std::string> visitedBlocks;
            while (cursor != siteBlock->id) {
                if (cursor.empty() || !visitedBlocks.insert(cursor).second)
                    return false;
                const Block* source = b.getBlock(cursor);
                if (!source || ++visited > 100000) return false;
                uint32_t statement = 0;
                if (!stmt(source, statement)) return false;
                site.before.push_back(statement);
                cursor = source->next;
            }
            if (site.before.empty() ||
                !expr(inputOf(siteBlock, "CONDITION"), site.condition))
                return false;
            const auto* input = siteBlock->inputs.find("SUBSTACK");
            const Block* call = input && !input->block.empty()
                ? b.getBlock(input->block) : nullptr;
            if (!call) return false;
            for (const auto& argId : signature.argumentIds) {
                uint32_t exprId = 0;
                if (!expr(inputOf(call, argId.c_str()), exprId))
                    return false;
                site.nextArguments.push_back(exprId);
            }
            stack.returnSites.push_back(std::move(site));
            cursor = siteBlock->next;
        }
        std::unordered_set<std::string> visitedSuffix;
        while (!cursor.empty()) {
            if (!visitedSuffix.insert(cursor).second)
                return false;
            const Block* source = b.getBlock(cursor);
            if (!source || ++visited > 100000) return false;
            uint32_t statement = 0;
            if (!stmt(source, statement)) return false;
            stack.elseBody.push_back(statement);
            cursor = source->next;
        }
        if (stack.elseBody.empty()) return false;
        outBody.push_back(p.stmt(std::move(stack)));
        return true;
    }

    bool lowerNonTailRecursiveWarp(
        const Block* definition, const Block* recursiveIf,
        std::vector<uint32_t>& outBody) {
        if (!definition || !recursiveIf) return false;
        Stmt frames;
        frames.k = SK::WarpReturnStack;
        frames.inlineWarp = true;
        for (std::string cursor = definition->next;
             cursor != recursiveIf->id;) {
            const Block* source = b.getBlock(cursor);
            if (!source || ++visited > 100000) return false;
            uint32_t statement = 0;
            if (!stmt(source, statement)) return false;
            frames.body.push_back(statement);
            cursor = source->next;
            if (cursor.empty()) return false;
        }
        if (frames.body.empty() ||
            !expr(inputOf(recursiveIf, "CONDITION"), frames.expr))
            return false;
        for (std::string cursor = recursiveIf->next; !cursor.empty();) {
            const Block* source = b.getBlock(cursor);
            if (!source || ++visited > 100000) return false;
            uint32_t statement = 0;
            if (!stmt(source, statement)) return false;
            frames.elseBody.push_back(statement);
            cursor = source->next;
        }
        if (frames.elseBody.empty()) return false;
        outBody.push_back(p.stmt(std::move(frames)));
        return true;
    }

    bool lowerTailRecursiveWarp(const Block* definition,
                                const Block* tail,
                                const Mutation& signature,
                                const std::vector<uint32_t>& argumentSlots,
                                std::vector<uint32_t>& outBody) {
        if (!definition || !tail) return false;
        Stmt recur;
        recur.k = SK::WarpTailRecurse;
        recur.inlineWarp = true;
        std::string cursor = definition->next;
        while (cursor != tail->id) {
            const Block* source = b.getBlock(cursor);
            if (!source || ++visited > 100000) return false;
            uint32_t statement = 0;
            if (!stmt(source, statement)) return false;
            recur.body.push_back(statement);
            cursor = source->next;
            if (cursor.empty()) return false;
        }
        if (recur.body.empty() ||
            !expr(inputOf(tail, "CONDITION"), recur.expr))
            return false;
        // Capture the next recursive frame's argument expressions while
        // the original formal argument slots are still in scope. The
        // emitter evaluates all values before changing any argument local.
        const auto* branch = tail->inputs.find("SUBSTACK");
        const Block* recursive = branch ? b.getBlock(branch->block) : nullptr;
        if (!recursive ||
            argumentSlots.size() != signature.argumentIds.size())
            return false;
        for (size_t i = 0; i < signature.argumentIds.size(); ++i) {
            uint32_t nextArg = 0;
            if (!expr(inputOf(recursive, signature.argumentIds[i].c_str()),
                      nextArg))
                return false;
            recur.argumentExprs.push_back(nextArg);
            recur.argumentSlots.push_back(argumentSlots[i]);
        }
        outBody.push_back(p.stmt(std::move(recur)));
        return true;
    }

    bool expr(const Block* blk, uint32_t& out) {
        DepthGuard guard(depth);
        if (depth > 512 || ++visited > 100000 || !blk) return false;
        const std::string& op = blk->opcode;
        if (isNumShadow(op)) {
            auto it = blk->fields.find("NUM");
            if (it == blk->fields.end()) return false;
            out = p.exprNum(cast::toNumber(it->second.value));
            return true;
        }
        if (op == "text") {
            const auto it = blk->fields.find("TEXT");
            if (it == blk->fields.end()) return false;
            Expr literal;
            literal.k = EK::ConstText;
            literal.var = uint32_t(p.stringLiterals.size());
            p.stringLiterals.emplace_back(cast::toString(it->second.value)->view());
            out = p.expr(literal);
            return true;
        }
        if (op == "argument_reporter_string_number" ||
            op == "argument_reporter_boolean") {
            const auto it = blk->fields.find("VALUE");
            if (it == blk->fields.end() || !it->second.value.isString())
                return false;
            const SStr* name = it->second.value.asStrPtr();
            if (!name) return false;
            const auto arg = activeArgumentSlots.find(
                std::u16string(name->view()));
            if (arg == activeArgumentSlots.end()) return false;
            Expr read;
            read.k = EK::ProcedureArgument;
            read.var = arg->second;
            out = p.expr(read);
            return true;
        }
        if (op == "sensing_dayssince2000") {
            Expr clock;
            clock.k = EK::DaysSince2000;
            out = p.expr(clock);
            return true;
        }
        if (op == "operator_length") {
            const Block* input = inputOf(blk, "STRING");
            if (!input) return false;
            // Selected constant shadows have immutable UTF-16 text. The
            // mutable variable case is evaluated by the runtime host helper,
            // preserving Scratch's string formatting for numeric values.
            if (input->opcode == "text" || isNumShadow(input->opcode)) {
                const auto it = input->fields.find(
                    input->opcode == "text" ? "TEXT" : "NUM");
                if (it == input->fields.end()) return false;
                out = p.exprNum(double(
                    cast::toString(it->second.value)->view().size()));
                return true;
            }
            if (input->opcode != "data_variable") {
                uint32_t source;
                if (!expr(input, source)) return false;
                Expr valueLength;
                valueLength.k = EK::StringLengthValue;
                valueLength.a = source;
                out = p.expr(valueLength);
                return true;
            }
            const auto it = input->fields.find("VARIABLE");
            if (it == input->fields.end() || it->second.id.empty())
                return false;
            Expr query;
            query.k = EK::StringLength;
            query.var = slotFor(it->second.id);
            out = p.expr(query);
            return true;
        }
        if (op == "data_lengthoflist" ||
            op == "data_listcontainsitem" ||
            op == "data_itemnumoflist") {
            const auto it = blk->fields.find("LIST");
            if (it == blk->fields.end() || it->second.id.empty())
                return false;
            Expr query;
            query.k = op == "data_lengthoflist" ? EK::ListLength :
                      op == "data_listcontainsitem" ? EK::ListContains :
                      EK::ListIndexOf;
            query.var = listFor(it->second.id);
            if (query.k != EK::ListLength &&
                !expr(inputOf(blk, "ITEM"), query.a))
                return false;
            out = p.expr(query);
            return true;
        }
        if (op == "data_listcontents") {
            const auto it = blk->fields.find("LIST");
            if (it == blk->fields.end() || it->second.id.empty())
                return false;
            Expr reporter;
            reporter.k = EK::ListContents;
            reporter.var = listFor(it->second.id);
            out = p.expr(reporter);
            return true;
        }
        if (op == "data_itemoflist") {
            const auto it = blk->fields.find("LIST");
            if (it == blk->fields.end() || it->second.id.empty())
                return false;
            uint32_t indexExpr = 0;
            if (!expr(inputOf(blk, "INDEX"), indexExpr)) return false;
            Expr item;
            item.k = EK::ListGet;
            item.var = listFor(it->second.id);
            item.a = indexExpr;
            out = p.expr(item);
            return true;
        }
        if (op == "data_variable") {
            auto it = blk->fields.find("VARIABLE");
            if (it == blk->fields.end() || it->second.id.empty()) return false;
            out = p.exprVar(slotFor(it->second.id));
            return true;
        }
        if (op == "operator_mathop") {
            auto field = blk->fields.find("OPERATOR");
            if (field == blk->fields.end() || !field->second.value.isString())
                return false;
            const SStr* str = field->second.value.asStrPtr();
            if (!str) return false;
            std::string name = SStr::toUtf8(str->view());
            for (char& ch : name)
                if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
            static constexpr const char* names[] = {
                "abs", "floor", "ceiling", "sqrt", "sin", "cos", "tan",
                "asin", "acos", "atan", "ln", "log", "e ^", "10 ^"
            };
            uint32_t fn = 255;
            for (uint32_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
                if (name == names[i]) { fn = i; break; }
            uint32_t arg;
            if (!expr(inputOf(blk, "NUM"), arg)) return false;
            Expr item;
            item.k = EK::MathOp;
            item.a = arg;
            item.var = fn;
            out = p.expr(item);
            return true;
        }
        if (op == "operator_join" || op == "operator_letter_of" ||
            op == "operator_contains") {
            const char* left = op == "operator_letter_of" ? "LETTER" : "STRING1";
            const char* right = op == "operator_letter_of" ? "STRING" : "STRING2";
            uint32_t a = 0, bb = 0;
            if (!expr(inputOf(blk, left), a) ||
                !expr(inputOf(blk, right), bb)) return false;
            out = p.exprBin(op == "operator_join" ? EK::StringJoin :
                            op == "operator_letter_of" ? EK::StringLetter :
                            EK::StringContains, a, bb);
            return true;
        }
        if (op == "operator_round" || op == "operator_not") {
            uint32_t a;
            if (!expr(inputOf(blk, op == "operator_round" ? "NUM" : "OPERAND"), a))
                return false;
            out = p.exprUn(op == "operator_round" ? EK::Round : EK::Not, a);
            return true;
        }
        EK k;
        if (op == "operator_add") k = EK::Add;
        else if (op == "operator_subtract") k = EK::Sub;
        else if (op == "operator_multiply") k = EK::Mul;
        else if (op == "operator_divide") k = EK::Div;
        else if (op == "operator_mod") k = EK::Mod;
        else if (op == "operator_lt") k = EK::LessThan;
        else if (op == "operator_gt") k = EK::GreaterThan;
        else if (op == "operator_equals") k = EK::Equals;
        else if (op == "operator_and") k = EK::And;
        else if (op == "operator_or") k = EK::Or;
        else return false;
        const bool operand = k == EK::LessThan || k == EK::GreaterThan ||
                             k == EK::Equals || k == EK::And || k == EK::Or;
        uint32_t a, bb;
        if (!expr(inputOf(blk, operand ? "OPERAND1" : "NUM1"), a)) return false;
        if (!expr(inputOf(blk, operand ? "OPERAND2" : "NUM2"), bb)) return false;
        out = p.exprBin(k, a, bb);
        return true;
    }

    // Scratch set-variable preserves the *original* Value tag, while
    // arithmetic in this numeric WASM subset produces f64. Never compile
    // a set whose immediate reporter could produce a boolean/string tag:
    // converting false to 0 breaks later comparisons and list operations.
    // These assignments are executed by the interpreter in hybrid scripts.
    bool numericAssignmentSource(const Block* source) const {
        if (!source) return false;
        const std::string& op = source->opcode;
        return isNumShadow(op) ||
            op == "operator_add" || op == "operator_subtract" ||
            op == "operator_multiply" || op == "operator_divide" ||
            op == "operator_mod" || op == "operator_round" ||
            op == "operator_mathop" || op == "data_lengthoflist" ||
            // ListGet's host bridge checks numericOnly and numeric Value
            // tags before returning a number; a mixed-type list bails.
            op == "data_itemoflist" || op == "data_itemnumoflist" ||
            op == "operator_length" ||
            op == "sensing_dayssince2000";
    }

    bool setVar(const Block* blk, SK kind, uint32_t& out) {
        const Block* source = inputOf(blk, "VALUE");
        if (!source) return false;
        const auto it = blk->fields.find("VARIABLE");
        if (it == blk->fields.end() || it->second.id.empty()) return false;

        Stmt statement;
        statement.k = kind;
        statement.var = slotFor(it->second.id);
        if (kind == SK::SetVar &&
            (source->opcode == "operator_join" ||
             source->opcode == "operator_letter_of" ||
             source->opcode == "data_itemoflist" ||
             source->opcode == "data_listcontents" ||
             source->opcode == "argument_reporter_string_number" ||
             source->opcode == "text")) {
            statement.k = SK::SetVarValue;
        }
        if (kind == SK::SetVar &&
            source->opcode == "data_variable") {
            const auto sourceIt = source->fields.find("VARIABLE");
            if (sourceIt == source->fields.end() ||
                sourceIt->second.id.empty())
                return false;
            statement.k = SK::SetVarRaw;
            statement.extraExpr = slotFor(sourceIt->second.id);
            out = p.stmt(std::move(statement));
            return true;
        }
        if (kind == SK::SetVar &&
            (source->opcode == "operator_lt" ||
             source->opcode == "operator_gt" ||
             source->opcode == "operator_equals" ||
             source->opcode == "operator_and" ||
             source->opcode == "operator_or" ||
             source->opcode == "operator_not" ||
             source->opcode == "data_listcontainsitem" ||
             source->opcode == "operator_contains" ||
             source->opcode == "argument_reporter_boolean")) {
            statement.k = SK::SetVarBoolean;
        } else if (kind == SK::SetVar &&
                   statement.k != SK::SetVarValue &&
                   !numericAssignmentSource(source)) {
            return false;
        }
        if (!expr(source, statement.expr)) return false;
        out = p.stmt(std::move(statement));
        return true;
    }

    bool stmt(const Block* blk, uint32_t& out) {
        if (!blk) return false;
        if (blk->opcode == "procedures_call") {
            if (!blk->mutation || blk->mutation->isReporterCall())
                return false;
            const std::string proccode = blk->mutation->attr("proccode");
            const Block* prototype = nullptr;
            const Block* definition =
                procedureDefinition(proccode, prototype);
            // Non-warp procedures can yield at a nested control boundary;
            // recursive calls and very large expansions are not inline-safe.
            if (!definition || !prototype || !prototype->mutation ||
                ++inlinedCalls > 64 ||
                activeProcedures.count(proccode))
                return false;
            const bool warp = prototype->mutation->warp();
            // A non-warp leaf may be inlined only at a non-warp call site.
            // An ancestor warp stack has its own procedure-entry deadline
            // check, which an inline expansion cannot reproduce.
            if (!warp && !activeWarpProcedures.empty()) return false;
            const Mutation& signature = *prototype->mutation;
            signature.parseArgs();
            if (signature.argumentIds.size() != signature.argumentNames.size() ||
                signature.argumentIds.size() > 16)
                return false;
            Stmt inlined;
            inlined.k = SK::InlineProcedure;
            inlined.inlineWarp = warp;
            inlined.argumentExprs.reserve(signature.argumentIds.size());
            inlined.argumentSlots.reserve(signature.argumentIds.size());
            for (size_t i = 0; i < signature.argumentIds.size(); ++i) {
                uint32_t value;
                if (!expr(inputOf(blk, signature.argumentIds[i].c_str()),
                          value)) return false;
                inlined.argumentExprs.push_back(value);
                inlined.argumentSlots.push_back(nextArgumentSlot++);
            }
            const auto outerArgs = activeArgumentSlots;
            for (size_t i = 0; i < signature.argumentNames.size(); ++i)
                activeArgumentSlots[signature.argumentNames[i]] =
                    inlined.argumentSlots[i];
            activeProcedures.insert(proccode);
            if (warp) activeWarpProcedures.push_back(proccode);
            // Tail recursion accepts at most four numeric parameters.
            // Each initial argument must be an actual numeric literal,
            // so all recursive argument locals can remain numeric and
            // never keep unrooted GC-managed strings across iterations.
            const bool literalNumericArgument =
                signature.argumentIds.size() <= 4 &&
                inlined.argumentExprs.size() == signature.argumentIds.size() &&
                std::all_of(
                    inlined.argumentExprs.begin(),
                    inlined.argumentExprs.end(),
                    [&](uint32_t expr) {
                        return expr < p.exprs.size() &&
                            p.exprs[expr].k == EK::ConstNum;
                    });
            const Block* tail = warp && literalNumericArgument
                ? tailRecursiveIf(definition, proccode, signature) : nullptr;
            // Non-tail recursion must restore the caller's return PC.
            // Only the zero-arg, one-site, numeric prefix/IF/suffix shape
            // is currently representable without a general frame stack.
            const Block* nonTail = warp && !tail &&
                signature.argumentIds.empty()
                ? nonTailRecursiveIf(definition, proccode) : nullptr;
            const auto pcSites = warp && !tail &&
                literalNumericArgument &&
                signature.argumentIds.size() <= 2
                ? recursiveReturnSites(definition, proccode, signature)
                : std::vector<const Block*>{};
            // Zero-argument, single-site recursion continues to use the
            // compact depth-only format. Other supported shapes need
            // explicit saved return PCs and argument frame values.
            const bool usePcStack = !pcSites.empty() &&
                (pcSites.size() > 1 || !signature.argumentIds.empty());
            const bool valid = tail
                ? lowerTailRecursiveWarp(
                    definition, tail, signature, inlined.argumentSlots,
                    inlined.body)
                : usePcStack
                ? lowerPcRecursiveWarp(
                    definition, signature, inlined.argumentSlots,
                    pcSites, inlined.body)
                : nonTail
                ? lowerNonTailRecursiveWarp(
                    definition, nonTail, inlined.body)
                : chain(definition->next, inlined.body);
            if (warp) activeWarpProcedures.pop_back();
            activeProcedures.erase(proccode);
            activeArgumentSlots = outerArgs;
            if (!valid) return false;
            if (!warp) {
                // Non-warp calls are synchronous unless their body actually
                // yields. Keep only pure straight-line/conditional leaves;
                // exclude nested calls and all loop forms, including repeat.
                const auto synchronous = [&](const auto& self,
                                             const std::vector<uint32_t>& ids)
                    -> bool {
                    for (uint32_t id : ids) {
                        if (id >= p.stmts.size()) return false;
                        const auto& child = p.stmts[id];
                        switch (child.k) {
                        case SK::SetVar: case SK::SetVarRaw:
                        case SK::SetVarValue:
                        case SK::SetVarBoolean: case SK::ChangeVar:
                        case SK::ListAppend: case SK::ListReplace:
                        case SK::ListDelete: case SK::ListDeleteAll:
                        case SK::ListInsert:
                            break;
                        case SK::If: case SK::IfElse:
                            if (!self(self, child.body) ||
                                !self(self, child.elseBody)) return false;
                            break;
                        default:
                            return false;
                        }
                    }
                    return true;
                };
                if (!synchronous(synchronous, inlined.body)) return false;
            }
            out = p.stmt(std::move(inlined));
            return true;
        }
        if (!blk) return false;
        if (blk->opcode == "data_addtolist" ||
            blk->opcode == "data_replaceitemoflist" ||
            blk->opcode == "data_deleteoflist" ||
            blk->opcode == "data_deletealloflist" ||
            blk->opcode == "data_insertatlist") {
            const auto list = blk->fields.find("LIST");
            if (list == blk->fields.end() || list->second.id.empty())
                return false;
            Stmt operation;
            operation.var = listFor(list->second.id);
            const bool append = blk->opcode == "data_addtolist";
            const bool replace = blk->opcode == "data_replaceitemoflist";
            const bool clear = blk->opcode == "data_deletealloflist";
            const bool insert = blk->opcode == "data_insertatlist";
            operation.k = append ? SK::ListAppend :
                          replace ? SK::ListReplace :
                          clear ? SK::ListDeleteAll :
                          insert ? SK::ListInsert : SK::ListDelete;
            // Accept any reporter handled by the shared Value lowering.
            // The item is evaluated only once and stored with its original
            // tag in the speculative list journal (numeric and strings).
            const Block* itemSource = inputOf(blk, "ITEM");
            if ((append || replace || insert) && !itemSource)
                return false;
            if (!append && !clear &&
                !expr(inputOf(blk, "INDEX"), operation.expr))
                return false;
            if ((append || replace || insert) &&
                !expr(inputOf(blk, "ITEM"), operation.extraExpr))
                return false;
            out = p.stmt(std::move(operation));
            return true;
        }
        if (blk->opcode == "data_setvariableto")
            return setVar(blk, SK::SetVar, out);
        if (blk->opcode == "data_changevariableby")
            return setVar(blk, SK::ChangeVar, out);
        if (blk->opcode == "control_if" ||
            blk->opcode == "control_if_else") {
            uint32_t e;
            if (!expr(inputOf(blk, "CONDITION"), e)) return false;
            Stmt s;
            s.k = blk->opcode == "control_if" ? SK::If : SK::IfElse;
            s.expr = e;
            const BlockInput* first = blk->inputs.find("SUBSTACK");
            if (first && !first->block.empty() &&
                !chain(first->block, s.body))
                return false;
            if (s.k == SK::IfElse) {
                const BlockInput* second = blk->inputs.find("SUBSTACK2");
                if (second && !second->block.empty() &&
                    !chain(second->block, s.elseBody))
                    return false;
            }
            out = p.stmt(std::move(s));
            return true;
        }
        if (blk->opcode == "control_repeat" ||
            blk->opcode == "control_repeat_until" ||
            blk->opcode == "control_while" ||
            blk->opcode == "control_forever" ||
            blk->opcode == "control_for_each") {
            const std::string& op = blk->opcode;
            Stmt s;
            if (op == "control_repeat") {
                s.k = SK::Repeat;
                if (!expr(inputOf(blk, "TIMES"), s.expr)) return false;
            } else if (op == "control_repeat_until") {
                s.k = SK::Until;
                if (!expr(inputOf(blk, "CONDITION"), s.expr)) return false;
            } else if (op == "control_while") {
                s.k = SK::While;
                if (!expr(inputOf(blk, "CONDITION"), s.expr)) return false;
            } else if (op == "control_for_each") {
                s.k = SK::ForEach;
                if (!expr(inputOf(blk, "VALUE"), s.expr)) return false;
                auto it = blk->fields.find("VARIABLE");
                if (it == blk->fields.end() || it->second.id.empty())
                    return false;
                s.var = slotFor(it->second.id);
            } else {
                s.k = SK::Forever;
            }
            const BlockInput* sub = blk->inputs.find("SUBSTACK");
            if (sub && !sub->block.empty() && !chain(sub->block, s.body))
                return false;
            out = p.stmt(std::move(s));
            return true;
        }
        return false;   // 非対応 → スクリプト全体を interp へ
    }

    bool chain(const std::string& id, std::vector<uint32_t>& out) {
        DepthGuard guard(depth);
        if (depth > 512) return false;
        std::unordered_set<std::string> seen;
        std::string cur = id;
        while (!cur.empty()) {
            if (++visited > 100000 || !seen.insert(cur).second) return false;
            const Block* blk = b.getBlock(cur);
            if (!blk) return false;
            uint32_t s;
            if (!stmt(blk, s)) return false;
            out.push_back(s);
            cur = blk->next;
        }
        return true;
    }
};

} // namespace

bool lowerCapturedScript(const ir::ScratchScript& script, Program& out) {
    if (!script.valid()) return false;
    const Block* top = script.getBlock(script.topId);
    if (!top) return false;
    const auto effectGraph = ir::ScratchEffects::lower(script);
    if (!effectGraph.valid) return false;
    Program candidate;
    Lower l{script, effectGraph, candidate};
    if (!l.chain(top->next, candidate.main)) return false;
    // Never publish a partially lowered module when an unsupported
    // effect/opcode requires the interpreter fallback.
    out = std::move(candidate);
    return true;
}

bool lowerCapturedStatement(const ir::ScratchScript& script,
                            const ir::ScratchEffects& effects,
                            const std::string& blockId, Program& out) {
    if (!script.valid() || !effects.valid || blockId.empty())
        return false;
    const Block* block = script.getBlock(blockId);
    if (!block) return false;
    Program candidate;
    Lower lowering{script, effects, candidate};
    uint32_t statement = 0;
    if (!lowering.stmt(block, statement)) return false;
    candidate.main.push_back(statement);
    out = std::move(candidate);
    return true;
}

bool lowerCapturedRange(const ir::ScratchScript& script,
                        const ir::ScratchEffects& effects,
                        const std::string& first, const std::string& pastEnd,
                        Program& out) {
    if (!script.valid() || !effects.valid || first.empty() ||
        first == pastEnd) return false;
    Program candidate;
    Lower lowering{script, effects, candidate};
    std::unordered_set<std::string> seen;
    std::string cursor = first;
    while (!cursor.empty() && cursor != pastEnd) {
        if (seen.size() >= 256 || !seen.insert(cursor).second)
            return false;
        const Block* block = script.getBlock(cursor);
        if (!block) return false;
        // Nested/non-warp scheduling needs interpreter frames. Only compile
        // complete top-level straight-line/conditional statements here.
        if (block->opcode == "control_repeat" ||
            block->opcode == "control_repeat_until" ||
            block->opcode == "control_while" ||
            block->opcode == "control_forever" ||
            block->opcode == "control_for_each")
            return false;
        uint32_t stmt = 0;
        if (!lowering.stmt(block, stmt)) return false;
        candidate.main.push_back(stmt);
        cursor = block->next;
    }
    if (cursor != pastEnd || candidate.main.empty()) return false;
    out = std::move(candidate);
    return true;
}

bool lowerScript(::xyo::Blocks& blocks, const ::xyo::Block* top,
                 Program& out) {
    if (!top) return false;
    const auto source = ir::ScratchCompilationUnit::capture(blocks, top);
    return source.valid() && lowerCapturedScript(source.script, out);
}

} // namespace xyo::wasm
