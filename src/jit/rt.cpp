// xyo-cpp: JIT 生成コードから呼ばれる extern "C" ランタイムヘルパ。
// jsexecute.js runtimeFunctions + cast/compare/list 系の移植。
// yield = fiber switch — C++ スタックが保持されるため、ヘルパ内部で
// 中断してもローカル状態は維持される（executeInCompatibilityLayer 相当）。
#include "jit.hpp"
#include "layq.hpp"
#include "aot_lazy_cache.hpp"
#include "rtprof.hpp"
#include "procedure_frame_state.hpp"
#include "rt_generated_ssa_roots.hpp"
#include "../ir/source_reporter_ssa_location_bridge.hpp"
#include "../ir/source_reporter_storage_owner_audit.hpp"
#include "../ir/source_reporter_input_slot_audit.hpp"
#include "../ir/source_reported_entry_destination_audit.hpp"
#include "../ir/source_scalar_prefix_audit.hpp"
#include "../ir/source_reported_entry_prefix_plan.hpp"
#include "../ir/source_reporter_input_roots.hpp"
#include "../ir/source_list_read_index_trace.hpp"
#include "../ir/source_typed_ssa.hpp"

#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <vector>

#include "../core/cast.hpp"
#include "../core/gc.hpp"
#include "../core/slist.hpp"
#include "../core/sstr.hpp"
#include "../core/v8math.hpp"
#include "../engine/block_utility.hpp"
#include "../engine/fiber.hpp"
#include "../engine/event_wait_dependency.hpp"
#include "../engine/runtime.hpp"
#include "../engine/sequencer.hpp"
#include "../engine/thread.hpp"
#include "../model/blocks.hpp"
#include "../model/target.hpp"
#include "../model/variable.hpp"
#include "../rt/runtime_helpers.hpp"

using namespace xyo;

namespace xyo {

} // namespace xyo

namespace {

Value toV(uint64_t bits) { Value v; v.bits = bits; return v; }
uint64_t toB(Value v) { return v.bits; }

BlockUtility& jitUtil(JitCtx* c) {
    // 並列 worker 対策として thread_local（並列適格スクリプトは
    // compat 経路に到達しないが、保険として隔離する）
    static thread_local BlockUtility util;
    util.sequencer = c->sequencer;
    util.thread = c->thread;
    return util;
}

// RT-001 experimental SOFT GC budget, default OFF. This is a
// scheduler-assisted, source-certified deopt request, not a hard
// allocation quota. A failing proof must NEVER collect a suspended
// native Fiber. Only enabled together with exact generated-SSA roots.
size_t nativeGcSoftBudgetBytes() {
    static const size_t budget = [] {
        const char* roots =
            std::getenv("XYO_RT_REQUIRE_GENERATED_SSA_ROOTS");
        const char* text = std::getenv("XYO_RT_GC_SOFT_BUDGET_MB");
        if (!roots || roots[0] != '1' || !text || !*text)
            return size_t{0};
        char* end = nullptr;
        const auto parsed = std::strtoull(text, &end, 10);
        if (!end || *end || parsed < 1 || parsed > 1024)
            return size_t{0};
        return size_t(parsed) << 20;
    }();
    return budget;
}

bool nativeGcSoftBudgetDue(JitCtx* c) {
    const size_t budget = nativeGcSoftBudgetBytes();
    return budget && c && c->thread &&
        !c->thread->nativeGcSoftBudgetAttempted &&
        Gc::get().bytesAllocated() >= budget;
}

bool isStuck(JitCtx* c) {
    // JS jsexecute: sequencer.timer.timeElapsed() > 500（tick 単位でリセット）。
    // dangerMode はスレッドごとの時間予算そのものが無いので時計も読まない。
    if (c->runtime->dangerMode) return false;
    return c->sequencer->timer.timeElapsed() > c->runtime->warpTimeMs;
}

/// 待機系（`wait`/`wait until`/`broadcast and wait` 等）の yield 判定。
/// 時間予算とは別物で、**他スレッドが進まないと待機が完了しない**ため、
/// danger モードでも待機中は譲る（そうしないと warp 内の待機はハングする）。
/// 通常モードの warp では従来通り stuck（500ms 窓）でのみ譲る。
bool waitShouldYield(JitCtx* c) {
    return !c->isWarp || c->runtime->dangerMode || isStuck(c);
}

#if !defined(XYO_DISABLE_RT_PROF)
/// var 系ヘルパ別カウンタ（XYO_PROF 時に内容を表示）
std::atomic<uint64_t> g_varCtrs[4] = {};
struct VarCtrDump {
    VarCtrDump() {
        if (!std::getenv("XYO_PROF")) return;
        std::atexit([]{
            std::fprintf(stderr,
                "[var] jitVar=%llu get=%llu set=%llu change=%llu\n",
                (unsigned long long)g_varCtrs[0].load(),
                (unsigned long long)g_varCtrs[1].load(),
                (unsigned long long)g_varCtrs[2].load(),
                (unsigned long long)g_varCtrs[3].load());
        });
    }
} g_varCtrDump;
#else
// Profiling counters and their process-startup environment checks are absent.
#endif

#if !defined(XYO_DISABLE_RT_PROF)
/// compat opcode 別呼出数（XYO_PROF 時）
std::unordered_map<std::string, uint64_t>& compatOps() {
    static auto* m = new std::unordered_map<std::string, uint64_t>;
    static int once = []{
        std::atexit([]{
            std::vector<std::pair<std::string,uint64_t>> v(
                compatOps().begin(), compatOps().end());
            std::sort(v.begin(), v.end(),
                      [](auto& a, auto& b){ return a.second > b.second; });
            for (auto& [k,n] : v)
                std::fprintf(stderr, "[compat] %s %llu\n", k.c_str(),
                             (unsigned long long)n);
        });
        return 0; }();
    (void)once;
    return *m;
}
#endif  // !XYO_DISABLE_RT_PROF

} // namespace

extern "C" {

// GC is stop-the-world; the collection counter is read only while a
// generated fiber is running. Never use a cached string pointer when a GC
// collection occurred during a yield, even if the boxed bits match (ABA).
uint64_t xyo_gc_epoch() {
    return static_cast<uint64_t>(Gc::get().collectCount());
}

// --- 基本 -------------------------------------------------------------

void xyo_yield(JitCtx* c) {
    // A soft budget request may only be published at an actual native
    // source-authenticated AfterLoopBody checkpoint with independently
    // retained generated-string SSA roots. This never authorizes an
    // arbitrary mid-statement or unrooted-pointer deoptimization.
    if (nativeGcSoftBudgetDue(c) && c->thread &&
        c->thread->nativeProcedureFrames &&
        c->thread->nativeProcedureFrames->enabled() &&
        c->thread->nativeProcedureFrames->sourceUnchanged() &&
        c->thread->nativeRtSsaCapture) {
        const auto checkpoint =
            c->thread->nativeProcedureFrames->sourceCheckpoint();
        if (checkpoint &&
            checkpoint->phase ==
                ir::SourceResumePhase::AfterLoopBody &&
            !checkpoint->loops.empty() &&
            checkpoint->frame.resumePc ==
                checkpoint->loops.back().sourcePc)
            c->thread->nativeGcSoftBudgetYieldDue = true;
    }
    // The optional scheduler-owned CallFrame mirror survives the native
    // Fiber switch. Verify that other scheduler activity did not corrupt
    // its logical return PCs or numeric argument snapshots.
    if (c->thread && c->thread->nativeProcedureFrames)
        c->thread->nativeProcedureFrames->beforeYield();
    // This is the only native JIT fiber suspension boundary. Commit the
    // whole slice before the host can observe/suspend/restart this thread.
    flushJitBlockCount(c);
#if !defined(XYO_DISABLE_RT_PROF)
    // This is the per-iteration hot path of non-warp Scratch scripts.
    // Avoid constructing two profiling guards when profiling is disabled.
    if (RTProf::get().enabled) {
        XYO_RT_PROF(kYield);
        c->fiber->yield();
        if (c->thread && c->thread->nativeProcedureFrames)
            c->thread->nativeProcedureFrames->afterYield();
        return;
    }
#endif
    c->fiber->yield();
    if (c->thread && c->thread->nativeProcedureFrames)
        c->thread->nativeProcedureFrames->afterYield();
}

int32_t xyo_is_stuck(JitCtx* c) { return isStuck(c) ? 1 : 0; }

/// 待機系の yield 判定（テスト/診断用）。
int32_t xyo_wait_should_yield(JitCtx* c) {
    return waitShouldYield(c) ? 1 : 0;
}

/// `wait until` ループ用の yield 点 — 待機中は danger でも譲る。
void xyo_wait_yield(JitCtx* c) {
    XYO_RT_PROF(kLoopY);
    if (waitShouldYield(c)) xyo_yield(c);
}

void xyo_wait_event_yield(JitCtx* c, uint64_t site) {
    XYO_RT_PROF(kLoopY);
    if (waitShouldYield(c)) {
        c->sequencer->armEventWait(*c->thread, static_cast<uintptr_t>(site));
        snapshotEventWaitDependency(*c->thread, static_cast<uintptr_t>(site));
        c->runtime->trackWaitDependencies(*c->thread);
        xyo_yield(c);
    }
}

// A wait may become true and return to the *same* site within a warp
// loop. An unchanged site ID alone cannot prove no Scratch work occurred.
void xyo_wait_event_done(JitCtx* c) {
    c->runtime->untrackWaitDependencies(*c->thread);
    ++c->thread->eventWaitExitCount;
    c->thread->eventWaitArmed = false;
    c->thread->eventWatchValid = false;
}

void xyo_loop_yield(JitCtx* c);

// An empty warp repeat has no observable body work. Preserve the exact
// (++yieldCheckN & 63) sampling phase while jumping directly to each check.
// Only called for nonnegative integral iteration counts <= 2^40.
// LAYQ: the quantum is re-derived per chunk from the scheduler-published
// hint (v2 push model) clamped to the pure cap — an empty body performs no
// calls and no allocation, so the pure cap is sound here (mirrors the
// body-class rule without needing the scope). Fixed mode uses the pinned
// entry quantum; off mode keeps 64. The phase math is quantum-agnostic
// (polls at least every q iterations). The stop-epoch check per chunk
// preserves the v2 <=1-quantum stop bound (xyo_loop_yield alone cannot
// observe stops); on mismatch we hand off, sync the epoch, and continue
// without skipping or duplicating iterations.
void xyo_empty_repeat_warp(JitCtx* c, double repetitions) {
    uint64_t left = static_cast<uint64_t>(repetitions);
    uint64_t counter = c->yieldCheckN;
    const LayqMode mode = layqMode();
    // The helper is a standalone scheduler (own chunk loop): apply the LAYQ
    // policy directly instead of inheriting a stale ctx quantum. An empty
    // body performs no calls and no allocation, so the pure cap is sound
    // (mirrors the body-class rule without needing the scope).
    const uint64_t pureCap =
        mode == LayqMode::Off ? 64 : layqConfig().qmaxPure;
    uint64_t observed = c->observedStopEpoch;
    while (left) {
        uint64_t q = 64;
        if (mode == LayqMode::Fixed) {
            const uint64_t pinned = layqConfig().qfixed;
            q = pinned < pureCap ? pinned : pureCap;
            if (q < layqConfig().qmin) q = layqConfig().qmin;
        } else if (mode == LayqMode::Adaptive) {
            const uint64_t hint = c->quantumHintPtr->load(
                std::memory_order_relaxed);
            q = hint < pureCap ? hint : pureCap;
        }
        // Belt-and-braces: quantum is always a power of two by
        // construction (entry/adaptation preserve it); fall back to 64
        // rather than breaking the phase math if it ever is not.
        if (q < 1 || (q & (q - 1)) != 0) q = 64;
        const uint64_t qmask = q - 1;
        const uint64_t untilCheck = q - (counter & qmask);
        if (left < untilCheck) {
            counter += left;
            break;
        }
        counter += untilCheck;
        left -= untilCheck;
        c->yieldCheckN = counter;
        if (mode != LayqMode::Off) {
            const uint64_t epoch = c->stopEpochPtr->load(
                std::memory_order_relaxed);
            if (epoch != observed) {
                xyo_yield(c);
                observed = epoch;
                c->observedStopEpoch = epoch;
            }
        }
        xyo_loop_yield(c);
    }
    c->yieldCheckN = counter;
}

/// loop 末尾の yield 点 — non-warp なら常に、warp なら stuck 時のみ
void xyo_loop_yield(JitCtx* c) {
    XYO_RT_PROF(kLoopY);
    if (!c->isWarp || nativeGcSoftBudgetDue(c) || isStuck(c))
        xyo_yield(c);
}

// Poll without switching fibers or changing Scratch variables. The JIT can
// keep promoted scalars/lists in SSA if this returns false, and spills them
// only before an actual scheduler handoff. Only call in a warp loop.
int32_t xyo_warp_yield_due(JitCtx* c) {
    XYO_RT_PROF(kLoopY);
    return nativeGcSoftBudgetDue(c) || isStuck(c) ? 1 : 0;
}

int32_t xyo_rt_gc_soft_budget_due(JitCtx* c) {
    return nativeGcSoftBudgetDue(c) ? 1 : 0;
}

// --- 値 ----------------------------------------------------------------

double xyo_to_num(uint64_t bits) {
    // The hot JIT helper should not have to reclassify a boxed Value in
    // cast::toNumber and again in its heap-string fallback. Signed-ordering
    // the NaN-box bits covers ordinary IEEE numbers (including negative
    // numbers and -0); canonical/positive NaN tags are handled below.
    if (std::bit_cast<int64_t>(bits) <
        std::bit_cast<int64_t>(val::kNan)) {
        double number;
        std::memcpy(&number, &bits, sizeof(number));
        return std::isnan(number) ? 0.0 : number;
    }
    if (bits == val::kTrue) return 1.0;
    if ((bits >> 48) != (val::kTagStr >> 48)) return 0.0;

    SStr* str = reinterpret_cast<SStr*>(bits & val::kPtrMask);
    if (!str) return 0.0; // Value::string(nullptr)
    const uint32_t state = str->loadNumState();
    if (state != 0) return state == 2 ? 0.0 : str->loadNumCache();

    // First use performs the full Scratch string parse and publishes the
    // string's existing atomic cache; this preserves comparison eligibility
    // (numState=1/2/3/4) for the JIT's guarded comparisons.
    return cast::parseNumberStringUncached(str);
}

// Called only after LLVM has proved the value is a non-null heap SStr
// and its numeric parse state was zero. Avoid duplicating all the boxed
// tag/cache checks of xyo_to_num on this cold path. Concurrent parsers are
// safe: SStr is immutable and publishNumCache uses release semantics.
double xyo_to_num_uncached(SStr* str) {
    return cast::parseNumberStringUncached(str);
}

int32_t xyo_truthy(uint64_t bits) { return cast::toBoolean(toV(bits)) ? 1 : 0; }

uint64_t xyo_num(double d) { return toB(Value::numCanonical(d)); }

uint64_t xyo_undefined() { return toB(Value::undefined()); }

uint64_t xyo_to_str(uint64_t bits) {
    XYO_RT_PROF(kCast);
    return toB(Value::string(cast::toString(toV(bits))));
}

uint64_t xyo_add(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kArith);
    return toB(Value::numCanonical(cast::toNumber(toV(a)) +
                                   cast::toNumber(toV(b))));
}
uint64_t xyo_sub(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kArith);
    return toB(Value::numCanonical(cast::toNumber(toV(a)) -
                                   cast::toNumber(toV(b))));
}
uint64_t xyo_mul(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kArith);
    return toB(Value::numCanonical(cast::toNumber(toV(a)) *
                                   cast::toNumber(toV(b))));
}
uint64_t xyo_div(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kArith);
    return toB(Value::numCanonical(cast::toNumber(toV(a)) /
                                   cast::toNumber(toV(b))));
}
uint64_t xyo_mod(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kArith);
    const double n = cast::toNumber(toV(a));
    const double m = cast::toNumber(toV(b));
    // scratch-vm: r = n % m; if (r / m < 0) r += m;（-0 は保持）
    double r = std::fmod(n, m);
    if (r / m < 0) r += m;
    return toB(Value::numCanonical(r));
}

double xyo_log(double x) {
    XYO_RT_PROF(kArith);
    return jsnum::jsLog(x);
}

double xyo_asin(double x) { return v8math::asin(x); }
double xyo_acos(double x) { return v8math::acos(x); }
double xyo_atan(double x) { return v8math::atan(x); }
double xyo_exp(double x) { return v8math::exp(x); }

// Scratch uses numeric order only when *both* operands are numeric
// strings; otherwise it compares case-folded text. SStr::numState caches
// exactly the required distinction (1: numeric, 2: NaN, 3: lexical zero,
// 4: Infinity literal). Do not change this if either cache is uninitialized.
static bool cachedStringOrder(uint64_t a, uint64_t b, int* order) {
    // Most comparisons are not string-vs-string. Reject them before even
    // accessing the once-initialized A/B switch or any string cache.
    if ((a >> 48) != (val::kTagStr >> 48) ||
        (b >> 48) != (val::kTagStr >> 48))
        return false;
    static const bool enabled = [] {
        const char* e = std::getenv("XYO_JIT_STRING_ORDER_CACHE");
        return !e || e[0] != '0';
    }();
    if (!enabled) return false;
    SStr* lhs = toV(a).asStrPtr();
    SStr* rhs = toV(b).asStrPtr();
    const uint32_t leftState = lhs->loadNumState();
    const uint32_t rightState = rhs->loadNumState();
    if (leftState == 0 || rightState == 0) return false;
    const bool leftNumeric = leftState == 1 || leftState == 4;
    const bool rightNumeric = rightState == 1 || rightState == 4;
    if (leftNumeric && rightNumeric) {
        const double x = lhs->loadNumCache();
        const double y = rhs->loadNumCache();
        *order = (x > y) - (x < y);
    } else {
        *order = compareStringsCaseInsensitive(lhs, rhs);
    }
    return true;
}

int32_t xyo_lt(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kArith);
    int order = 0;
    if (cachedStringOrder(a, b, &order)) return order < 0 ? 1 : 0;
    return cast::compare(toV(a), toV(b)) < 0 ? 1 : 0;
}
int32_t xyo_gt(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kArith);
    int order = 0;
    if (cachedStringOrder(a, b, &order)) return order > 0 ? 1 : 0;
    return cast::compare(toV(a), toV(b)) > 0 ? 1 : 0;
}
static int valTag(Value v) {
    if (v.isNumber()) return 0;
    if (v.isBool()) return 1;
    if (v.isString()) return v.asStrPtr() ? 2 : 3;
    if (v.isColor()) return 4;
    return 5;  // undefined 他
}

int32_t xyo_eq(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kArith);
    if (a == b) return 1;   // 同 bits は常に等値（kNan 含む）
    {
        Value va = toV(a), vb = toV(b);
        // 数値同士は compare の文字列/ゼロ判定を全部飛ばす。
        // 描画ループの色比較などホットパスで支配的（voxelRenderer の
        // display は 1 フレームに数千回）。
        if (va.isNumber() && vb.isNumber()) {
            double x = va.asNumberUnchecked();
            double y = vb.asNumberUnchecked();
            return x == y ? 1 : 0;
        }
        if (va.isString() && vb.isString() && va.asStrPtr() &&
            vb.asStrPtr()) {
            // numState は文字列内容の純関数 — 異なる state は
            // 必ず異なる内容 = 非等値（compare の文字列比較をスキップ）。
            // ただし Infinity リテラル（state4）は "infinity" 等の
            // case 違い（state2）と等しいためショートカット不可。
            uint32_t s1 = va.asStrPtr()->loadNumState();
            uint32_t s2 = vb.asStrPtr()->loadNumState();
            if (s1 && s2 && s1 != s2 && s1 != 4 && s2 != 4) return 0;
            // Both immutable strings have already been parsed and proved
            // nonnumeric. The generic cast::compare would inspect/convert
            // them again before reaching exactly this case-insensitive
            // lexical comparison. Retain the generic path for numeric
            // strings, whitespace-only zero and Infinity.
            static const bool directNonnumericEq = [] {
                const char* setting = std::getenv("XYO_JIT_NONNUMERIC_STR_EQ");
                return !setting || setting[0] != '0';
            }();
            if (directNonnumericEq && s1 == 2 && s2 == 2)
                return compareStringsCaseInsensitive(
                    va.asStrPtr(), vb.asStrPtr()) == 0 ? 1 : 0;
        }
    }
    return cast::compare(toV(a), toV(b)) == 0 ? 1 : 0;
}
int32_t xyo_not(uint64_t a) { return !cast::toBoolean(toV(a)) ? 1 : 0; }
int32_t xyo_and(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kArith);
    return (cast::toBoolean(toV(a)) && cast::toBoolean(toV(b))) ? 1 : 0;
}
int32_t xyo_or(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kArith);
    return (cast::toBoolean(toV(a)) || cast::toBoolean(toV(b))) ? 1 : 0;
}

uint64_t xyo_random(JitCtx*, uint64_t from, uint64_t to) {
    XYO_RT_PROF(kArith);
    // scratch3_operators.random 準拠（NaN 伝播・isInt 型判定・low/high 交換）
    Value fv = toV(from), tv = toV(to);
    const double nFrom = cast::toNumber(fv), nTo = cast::toNumber(tv);
    const double low = nFrom <= nTo ? nFrom : nTo;
    const double high = nFrom <= nTo ? nTo : nFrom;
    if (low == high) return toB(Value::numCanonical(low));
    if (cast::isInt(fv) && cast::isInt(tv))
        return toB(Value::numCanonical(rt::randomInt(low, high)));
    return toB(Value::numCanonical(rt::randomFloat(low, high)));
}

double xyo_mathop(JitCtx*, int32_t fn, double x) {
    XYO_RT_PROF(kArith);
    // scratch3_operators.mathop / math-util.tan 準拠
    constexpr double kPi = 3.14159265358979323846;
    switch (fn) {
    case 0: return std::fabs(x);
    case 1: return std::floor(x);
    case 2: return std::ceil(x);
    case 3: return std::sqrt(x);
    case 4:
        if (x == 0.0) return 0.0;
        return jsnum::jsRound(v8math::sin(kPi * x / 180.0) * 1e10) / 1e10;
    case 5: return jsnum::jsRound(v8math::cos(kPi * x / 180.0) * 1e10) / 1e10;
    case 6: {
        // MathUtil.tan: %360、±90/270 は ±Infinity、他は 1e10 丸め
        double a = std::fmod(x, 360.0);
        if (a == 0.0) return 0.0;
        if (a == -270.0 || a == 90.0) return INFINITY;
        if (a == -90.0 || a == 270.0) return -INFINITY;
        return jsnum::jsRound(std::tan(kPi * a / 180.0) * 1e10) / 1e10;
    }
    case 7: return v8math::asin(x) * 180.0 / kPi;
    case 8: return v8math::acos(x) * 180.0 / kPi;
    case 9: return v8math::atan(x) * 180.0 / kPi;
    case 10: return jsnum::jsLog(x);
    case 11: return jsnum::jsLog(x) / 2.302585092994046;
    case 12: return v8math::exp(x);
    case 13: return std::pow(10.0, x);
    default: return 0.0;
    }
}

double xyo_round(double x) {
    XYO_RT_PROF(kArith);
    // Scratch round: JS Math.round（.5 は +Infinity 方向、-0 を保持）
    return jsnum::jsRound(x);
}

// --- 変数 --------------------------------------------------------------
// clone 対応のため実行時に ctx->target で解決する（JS lookupVariableById 相当）

// Guarded native variable resolution after an editor map mutation:
// the base experimental mode rebinds on every access; a separate
// fast opt-in validates the owner + both binding generations before
// borrowing any cached Variable*. Normal builds retain the old path.
bool nativeGuardVarSlots() {
    static const bool enabled = [] {
        const char* value = std::getenv("XYO_RT_GUARD_NATIVE_VAR_SLOTS");
        return value && value[0] == '1';
    }();
    return enabled;
}

Variable* resolveLiveNativeVariable(JitCtx* c, const char* id,
                                    const char16_t* name, int list) {
    if (!c || !c->target || !c->thread || !id || !name)
        return nullptr;
    return list ? c->target->lookupOrCreateList(id, name)
                : c->target->lookupOrCreateVariable(id, name);
}

// Lightweight stage-owner query for LLVM's speculative indexed guard.
// Stage replacements or target-vector reordering are possible during
 // editor callbacks; epoch equality alone cannot prove stage identity.
extern "C" const Target* xyo_varslot_guard_stage(JitCtx* c) {
    if (!c || !c->target) return nullptr;
    if (c->target->isStage) return nullptr;
    return c->runtime ? c->runtime->getTargetForStage() : nullptr;
}

// Further opt-in within the guarded per-use resolver. This DOES NOT
// hoist Variable* in LLVM: every use still calls a side-effecting host
// function, which first validates the original owner's full stamp.
bool nativeGuardVarSlotsFast() {
    static const bool enabled = [] {
        const char* value = std::getenv(
            "XYO_RT_GUARD_NATIVE_VAR_SLOTS_FAST");
        return nativeGuardVarSlots() && value && value[0] == '1';
    }();
    return enabled;
}

void refreshNativeVarCacheStamp(JitCtx* c,
                               const Target::VariableBindingStamp& stamp) {
    Thread& thread = *c->thread;
    if (thread.nativeVarCacheValid &&
        thread.nativeVarCacheTarget == c->target &&
        thread.nativeVarCacheStage == stamp.stage &&
        thread.nativeVarCacheLocalGeneration == stamp.localGeneration &&
        thread.nativeVarCacheStageGeneration == stamp.stageGeneration)
        return;
    // Never inspect any cached Variable* before clearing the map.
    // Globally unique generation values reject erase/re-create even
    // if the allocator returns the same address.
    thread.jitVarCache.clear();
    thread.nativeVarIndexedSlots.clear();
    thread.nativeVarInlineEntries = nullptr;
    thread.nativeVarInlineCount = 0;
    thread.nativeVarLocalGenerationAddress =
        c->target->variables.generationAddress();
    thread.nativeVarStageGenerationAddress = stamp.stage
        ? stamp.stage->variables.generationAddress() : nullptr;
    if (thread.nativeVarCacheValid)
        ++thread.nativeVarCacheInvalidations;
    thread.nativeVarCacheTarget = c->target;
    thread.nativeVarCacheStage = stamp.stage;
    thread.nativeVarCacheLocalGeneration = stamp.localGeneration;
    thread.nativeVarCacheStageGeneration = stamp.stageGeneration;
    thread.nativeVarCacheValid = true;
}

// Extra opt-in: the compiler already knows a dense slot index. Avoid
// hashing JitVarCacheKey at each use, while retaining the exact same
// stamp validation and returning a freshly bound Variable* on misses.
// The key check prevents a slot from another compiled entry / procedure
// aliasing the current (id, name, list-kind).
bool nativeGuardVarSlotsIndexed() {
    static const bool enabled = [] {
        const char* flag = std::getenv(
            "XYO_RT_GUARD_NATIVE_VAR_SLOTS_INDEXED");
        return nativeGuardVarSlotsFast() &&
               flag && flag[0] == '1';
    }();
    return enabled;
}

Variable* resolveGuardedIndexedVariable(
    JitCtx* c, uint64_t idx, const char* id,
    const char16_t* name, int list) {
    if (!c || !c->target || !c->thread || !id || !name ||
        idx > 65535u) return nullptr;
    Thread& thread = *c->thread;
    refreshNativeVarCacheStamp(c, c->target->variableBindingStamp());
    const JitVarCacheKey key{id, name, list};
    if (idx < thread.nativeVarIndexedSlots.size()) {
        const auto& cached = thread.nativeVarIndexedSlots[size_t(idx)];
        if (cached.variable && cached.key == key) {
            ++thread.nativeVarCacheHits;
            return cached.variable;
        }
    }
    ++thread.nativeVarCacheMisses;
    Variable* variable = resolveLiveNativeVariable(c, id, name, list);
    if (!variable) return nullptr;
    // A new Scratch variable may alter the stamp during resolution.
    // Re-check after lookup and clear stale epoch entries before insert.
    refreshNativeVarCacheStamp(c, c->target->variableBindingStamp());
    if (idx >= thread.nativeVarIndexedSlots.size())
        thread.nativeVarIndexedSlots.resize(size_t(idx) + 1);
    thread.nativeVarIndexedSlots[size_t(idx)] = {key, variable};
    // Store the new vector address ONLY after resize/slot assignment.
    thread.nativeVarInlineEntries = thread.nativeVarIndexedSlots.data();
    thread.nativeVarInlineCount = thread.nativeVarIndexedSlots.size();
    return variable;
}

Variable* resolveGuardedCachedVariable(JitCtx* c, const char* id,
                                       const char16_t* name, int list) {
    if (!c || !c->target || !c->thread || !id || !name)
        return nullptr;
    Thread& thread = *c->thread;
    refreshNativeVarCacheStamp(c, c->target->variableBindingStamp());
    const JitVarCacheKey key{id, name, list};
    if (const auto it = thread.jitVarCache.find(key);
        it != thread.jitVarCache.end()) {
        ++thread.nativeVarCacheHits;
        return it->second;
    }
    ++thread.nativeVarCacheMisses;
    Variable* variable = resolveLiveNativeVariable(c, id, name, list);
    if (!variable) return nullptr;
    // Resolving an absent Scratch variable may insert a new entry,
    // which advances the target/stage structural generation.
    // Establish the new stamp AFTER that insertion and reject the
    // preceding cache epoch conservatively.
    refreshNativeVarCacheStamp(c, c->target->variableBindingStamp());
    thread.jitVarCache.emplace(key, variable);
    return variable;
}

Variable* xyo_jitVar(JitCtx* c, const char* id, const char16_t* name,
                     int list) {
    XYO_RT_PROF(kVar);
#if !defined(XYO_DISABLE_RT_PROF)
    if (RTProf::get().enabled) ++g_varCtrs[0];
#endif
    if (nativeGuardVarSlots())
        return nativeGuardVarSlotsFast()
            ? resolveGuardedCachedVariable(c, id, name, list)
            : resolveLiveNativeVariable(c, id, name, list);
    auto& cache = c->thread->jitVarCache;
    JitVarCacheKey key{id, name, list};
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    Variable* v = list ? c->target->lookupOrCreateList(id, name)
                       : c->target->lookupOrCreateVariable(id, name);
    cache.emplace(key, v);
    return v;
}

void xyo_varslot_init(JitCtx* c, uint64_t idx, const char* id,
                      const char16_t* name, int32_t isList) {
    Thread* t = c->thread;
    if (idx >= t->jitVarSlots.size()) t->jitVarSlots.resize(size_t(idx) + 1);
    static const bool directShortSlots = [] {
        const char* env = std::getenv("XYO_JIT_SHORT_SLOT_DIRECT");
        return !env || env[0] != '0';
    }();
    const auto* compiled = directShortSlots && t->isCompiled
        ? static_cast<const CompiledScript*>(t->compiled) : nullptr;
    if (compiled && compiled->entry && compiled->shortSync &&
        !compiled->mayYield && !nativeGuardVarSlots() && t->jitVarCache.empty()) {
        // codegen deduplicates each (id, exact name, list-kind) slot and
        // initializes it once per entry. An existing ID can bind directly,
        // without a one-use helper hash node or an eager UTF-16 name copy.
        // Resolve afresh on the current clone/target; keep slot lifetimes
        // and guarded per-use resolution completely unchanged.
        XYO_RT_PROF(kVar);
#if !defined(XYO_DISABLE_RT_PROF)
        if (RTProf::get().enabled) ++g_varCtrs[0];
#endif
        Variable* variable = c->target->lookupVariableById(id);
        if (!variable) {
            // Name fallback/creation can alias a later initializer with the
            // same absent ID but a different name. Preserve the original
            // helper-cache binding in that case, and use the ordinary path
            // for all subsequent slots once the cache is nonempty.
            variable = isList ? c->target->lookupOrCreateList(id, name)
                              : c->target->lookupOrCreateVariable(id, name);
            t->jitVarCache.emplace(JitVarCacheKey{id, name, isList}, variable);
        }
        t->jitVarSlots[size_t(idx)] = variable;
    } else {
        t->jitVarSlots[size_t(idx)] = xyo_jitVar(c, id, name, isList);
    }
    c->varSlotData = t->jitVarSlots.data();
}

// Only emitted when XYO_RT_GUARD_NATIVE_VAR_SLOTS=1 at compile time.
// With FAST=1 each access checks the complete Target/stage generation
// before borrowing JitVarCache; otherwise it re-resolves unconditionally.
// Do not hoist the result across script yields or editor callbacks.
Variable* xyo_varslot_checked(JitCtx* c, uint64_t idx, const char* id,
                              const char16_t* name, int32_t isList) {
    Variable* resolved = nativeGuardVarSlotsIndexed()
        ? resolveGuardedIndexedVariable(c, idx, id, name, isList)
        : nativeGuardVarSlotsFast()
            ? resolveGuardedCachedVariable(c, id, name, isList)
            : resolveLiveNativeVariable(c, id, name, isList);
    if (!resolved) return nullptr;
    auto& slots = c->thread->jitVarSlots;
    if (idx >= slots.size()) slots.resize(size_t(idx) + 1);
    slots[size_t(idx)] = resolved;
    c->varSlotData = slots.data();
    return resolved;
}

uint64_t xyo_var_get(JitCtx* c, const char* id, const char16_t* name) {
    XYO_RT_PROF(kVar);
#if !defined(XYO_DISABLE_RT_PROF)
    if (RTProf::get().enabled) ++g_varCtrs[1];
#endif
    Variable* v = xyo_jitVar(c, id, name, false);
    return v ? toB(v->value) : toB(Value::undefined());
}

// Generated LLVM stores bypass xyo_var_set/xyo_var_change. This callback
// receives the currently resolved scalar Variable* from varSlot(), never a
// cached pointer from the reverse dependency registry.
void xyo_wait_notify_var_store(JitCtx* c, Variable* variable) {
    if (c && c->runtime && variable)
        c->runtime->notifyWaitVariableWrite(*variable);
}

void xyo_var_set(JitCtx* c, const char* id, const char16_t* name,
                 uint64_t value) {
    XYO_RT_PROF(kVar);
#if !defined(XYO_DISABLE_RT_PROF)
    if (RTProf::get().enabled) ++g_varCtrs[2];
#endif
    Variable* v = xyo_jitVar(c, id, name, false);
    if (v) {
        v->value = toV(value);
        c->runtime->notifyWaitVariableWrite(*v);
    }
}

void xyo_var_change(JitCtx* c, const char* id, const char16_t* name,
                    uint64_t value) {
    XYO_RT_PROF(kVar);
#if !defined(XYO_DISABLE_RT_PROF)
    if (RTProf::get().enabled) ++g_varCtrs[3];
#endif
    Variable* v = xyo_jitVar(c, id, name, false);
    if (v) {
        const double d = cast::toNumber(v->value) + cast::toNumber(toV(value));
        v->value = Value::numCanonical(d);
        c->runtime->notifyWaitVariableWrite(*v);
    }
}

// --- 手続き --------------------------------------------------------------

uint64_t xyo_arg_get(JitCtx* c, uint64_t index) {
    XYO_RT_PROF(kArg);
    return index < c->numArgs ? toB(c->args[index])
                              : toB(Value::undefined());
}

// --- 制御 -----------------------------------------------------------------

void xyo_rt_wait(JitCtx* c, double secs) {
    XYO_RT_PROF(kOther);
    // JS jsgen CONTROL_WAIT: timer start → redraw → yieldNotWarp →
    // while (elapsed < ms) yieldStuckOrNotWarp
    Timer t([c] { return c->runtime->currentMSecs; });
    t.start();
    const double ms = std::max(0.0, secs * 1000.0);
    c->runtime->redrawRequested = true;
    if (!c->isWarp) {
        // Same immediate yield semantics as the interpreter. A renderer
        // already ends its frame when redrawRequested is set.
        xyo_yield(c);
    }
    while (t.timeElapsed() < ms) {
        if (waitShouldYield(c)) {
            // Preserve warp's watchdog and the ordinary Scratch yield
            // order. STATUS_YIELD_TICK changes GUI receiver sequencing.
            xyo_yield(c);
        }
    }
}

void xyo_stop_all(JitCtx* c) {
    XYO_RT_PROF(kOther);
    c->runtime->stopAll();
    // 自スレッドも停止済み — 永久に中断（再 resume されない）
    c->runtime->transitionThread(*c->thread, STATUS_DONE);
    for (;;) xyo_yield(c);
}

void xyo_stop_others(JitCtx* c) {
    XYO_RT_PROF(kOther);
    c->runtime->stopForTarget(c->target, c->thread);
}

void xyo_stop_script(JitCtx* c) {
    XYO_RT_PROF(kOther);
    // 手続き内部からでも呼ばれうる — stopped フラグで呼出し側へ伝播し
    // エントリ関数まで巻き戻す（JS の stack クリアに相当）
    c->stopped = 1;
    c->runtime->transitionThread(*c->thread, STATUS_DONE);
    for (;;) xyo_yield(c);
}

void xyo_broadcast_v(JitCtx* c, uint64_t v) {
    XYO_RT_PROF(kBcast);
    // Own the name across startBroadcast: allocating/compiling a receiver
    // may collect the temporary SStr, invalidating a borrowed string_view.
    const std::u16string name(cast::toString(toV(v))->view());
    if (std::getenv("XYO_JIT_VERBOSE")) {
        std::string s; for (char16_t ch : name)
            s += ch < 128 ? char(ch) : '?';
        std::fprintf(stderr, "xyo-jit: broadcast '%s'\n", s.c_str());
    }
    c->sequencer->runtime.startBroadcast(name);
}

void xyo_broadcast_wait_v(JitCtx* c, uint64_t v) {
    XYO_RT_PROF(kOther);
    // event_broadcastandwait 相当: hats 起動 → 全スレッド終了まで待機
    Runtime* rt = c->runtime;
    // Broadcast subscriptions are already indexed by their uppercase
    // names. For ASCII inputs, canonicalizing through all Stage broadcast
    // variables has no effect on the recipient set, but adds a linear
    // search to every event. Preserve the old lookup for non-ASCII names:
    // Unicode upper/lower casing is not always reversible.
    std::u16string name(cast::toString(toV(v))->view());
    static const bool fastAscii = [] {
        const char* setting = std::getenv("XYO_BCAST_FAST_ASCII");
        return !setting || setting[0] != '0';
    }();
    const bool ascii = fastAscii &&
        rt->stageBroadcastNamesAsciiOnly() &&
        std::all_of(name.begin(), name.end(),
            [](char16_t ch) { return ch < 128; });
    Variable* var = nullptr;
    if (!ascii) {
        Target* stage = rt->getTargetForStage();
        var = stage ? stage->lookupBroadcastMsg("", name) : nullptr;
    }
    std::vector<Thread*> started =
        rt->startBroadcastAndWait(var ? var->name : name);
    // The original Fiber stack owns the join-list through every yield;
    // the journal is only involved in the first receiver launch.
    if (started.empty()) return;
    c->joinRefs = &started;   // 回収側から見えるように（完了スレッドを守る）
    for (;;) {
        bool waiting = false, allWaiting = true;
        for (Thread* t : started) {
            if (rt->containsThread(t)) {
                waiting = true;
                // Membership is already known. isWaitingThread() would
                // redundantly check runtime membership a second time.
                if (t->status != STATUS_PROMISE_WAIT &&
                    t->status != STATUS_YIELD_TICK &&
                    !t->stack.empty() && t->status != STATUS_DONE)
                    allWaiting = false;
            }
        }
        if (!waiting) break;
        if (!allWaiting && c->isWarp && !waitShouldYield(c)) continue;
        // yield / yieldTick — status はスレッドに立ててから fiber 中断
        c->thread->status =
            allWaiting ? STATUS_YIELD_TICK : STATUS_YIELD;
        xyo_yield(c);
    }
    c->joinRefs = nullptr;
    c->thread->status = STATUS_RUNNING;
}

double xyo_timer(JitCtx* c) {
    XYO_RT_PROF(kOther);
    return c->runtime->ioDevices.clock.projectTimer();
}

/// compat 層の入力書き込み（JS writeInputValue 相当）。
/// name はコンパイル時埋め込みの C 文字列（Blocks 所有の安定領域）。
// AOT metadata v3: a source-owned descriptor stays stable even after
// Scratch edits retire the underlying cache. After the first resolution,
// an acquire/release pointer avoids repeatedly locking on the hot path.
// The code generator calls this only for v3 BlockCached slots, never v1/v2.
extern "C" const BlockCached* xyo_rt_aot_cached(void* raw) {
    auto* descriptor = static_cast<AotLazyCachedSlot*>(raw);
    if (!descriptor || !descriptor->blocks) return nullptr;
    // Fail closed even for previously resolved descriptors: an old native
    // AOT function must not borrow a new BlockCached after source edits.
    // The host scheduler checks the same source stamp before execution.
    if (!descriptor->sourceRevision ||
        descriptor->blocks->revision() != descriptor->sourceRevision)
        return nullptr;
    if (const auto* cached =
            descriptor->resolved.load(std::memory_order_acquire))
        return cached;
    std::lock_guard lock(descriptor->mutex);
    // Recheck after acquiring the descriptor lock to avoid resolving from
    // a structurally changed Blocks graph at an editor safe point.
    if (!descriptor->sourceRevision ||
        descriptor->blocks->revision() != descriptor->sourceRevision)
        return nullptr;
    if (const auto* cached =
            descriptor->resolved.load(std::memory_order_relaxed))
        return cached;
    BlockCached* cached =
        descriptor->blocks->getCachedExecute(descriptor->blockId);
    if (cached)
        descriptor->resolved.store(cached, std::memory_order_release);
    return cached;
}

void xyo_rt_setarg(const BlockCached* op, const char* name, uint64_t v) {
    XYO_RT_PROF(kOther);
    ArgValues* av = const_cast<ArgValues*>(&op->argValues);
    if (std::strcmp(name, "BROADCAST_INPUT") == 0) {
        IdName& bo = av->refObj("BROADCAST_OPTION");
        bo.id.clear();
        SStr* s = cast::toString(toV(v));
        bo.name = s->view();
    } else {
        av->ref(name) = toV(v);
    }
}

// --- compat 層 -----------------------------------------------------------
// JS executeInCompatibilityLayer: blockFunction(inputs, blockUtility) を
// 直接呼び、YIELD/YIELD_TICK の間再評価する。promise は PROMISE_WAIT で
// waitQueue 経由解決。スタックフレームは fiber ローカルの合成 EC。

uint64_t xyo_rt_compat(JitCtx* c, const BlockCached* op) {
    XYO_RT_PROF(kCompat);
#if !defined(XYO_DISABLE_RT_PROF)
    static bool profOps = std::getenv("XYO_PROF") != nullptr;
    if (profOps) compatOps()[op->opcode]++;
#endif
    Thread* t = c->thread;
    BlockUtility& util = jitUtil(c);
    ExecContext ec;
    // プリミティブが stackFrame() を要求したとき合成 EC を返す
    util.jitCompatEc = &ec;
    // compat 経由の broadcast and wait は startedThreads をこのファイバ
    // ローカルの EC に持つ。完了スレッドの回収（Runtime::reclaimThreads）が
    // 生ポインタ参照を辿れるよう、呼出し中だけ公開する。
    c->joinRefs = &ec.startedThreads;

    Value rv = op->blockFunction
        ? op->blockFunction(op->argValues, util)
        : Value::undefined();
    for (;;) {
        if (t->status == STATUS_PROMISE_WAIT || t->status == STATUS_DONE) {
            util.jitCompatEc = nullptr;
            c->joinRefs = nullptr;
            xyo_yield(c);
            return toB(Value::emptyStr());
        }
        if (t->status != STATUS_YIELD && t->status != STATUS_YIELD_TICK) break;
        if (t->status == STATUS_YIELD) {
            t->status = STATUS_RUNNING;
            if (waitShouldYield(c)) xyo_yield(c);
        } else {
            xyo_yield(c);
        }
        if (c->blockCounter && op->blockFunction)
            ++c->pendingBlockCount;
        rv = op->blockFunction
            ? op->blockFunction(op->argValues, util)
            : Value::undefined();
    }
    util.jitCompatEc = nullptr;
    c->joinRefs = nullptr;
    return toB(rv);
}

// RT-001 opt-in root-only native checkpoint producer. A Scratch script
// with no procedures_call has no natural xyo_proc_frame_enter_* site,
// so the ordinary lazy mirror would never exist. Create the SAME
// validated source-root session at the compiled script entry, using
// the revision and 128-bit digest baked into LLVM when it was compiled.
// Never obtain a new provenance stamp from the live mutable Blocks.
void xyo_proc_frame_init_root_verified(
    JitCtx* c, uint64_t compiledRevision,
    uint64_t digestLo, uint64_t digestHi) {
    if (!c || !c->thread || !compiledRevision ||
        (digestLo == 0 && digestHi == 0)) return;
    Thread& thread = *c->thread;
    if (thread.nativeProcedureFrames || !thread.blockContainer) return;
    const Block* root = thread.blockContainer->getBlock(thread.topBlock);
    if (!root) return;
    const auto source = ir::ScratchCompilationUnit::capture(
        *thread.blockContainer, root);
    // The constructor independently compares the immutable compiler
    // identity with the live source and refuses a changed revision.
    // The same Thread may be restarted with a new compiled root. Begin
    // each verified root's RT scheduler-only checkpoint count at zero.
    thread.nativeRootLoopYieldCount = 0;
    thread.nativeRtSsaCapture.reset();
    thread.nativeProcedureFrames =
        std::make_unique<ir::NativeProcedureFrameSession>(
            source, thread.blockContainer, compiledRevision,
            ir::ScratchSourceDigest{digestLo, digestHi});
}

// RT-001: detach integer/floating SSA and raw argument words BEFORE
// xyo_loop_yield switches Fiber. Values are copied immediately; the
// alloca and metadata pointers are never retained. This is a diagnostic,
// not a general deopt environment, GC root, or permission to resume.
void xyo_rt_capture_llvm_ssa(
    JitCtx* c, const uint64_t* words, const uint64_t* descriptors,
    uint32_t count, uint32_t unsupported, uint32_t numericComplete,
    uint64_t compiledRevision, uint64_t digestLo, uint64_t digestHi,
    uint64_t siteId, uint32_t unresolvedPointers,
    uint32_t unresolvedOther, const uint32_t* pointerOrigins,
    uint32_t stackDerivedPointers, uint32_t distinctStackAllocas,
    const uint32_t* aliasArguments,
    const uint32_t* generatedStringSources) {
    constexpr uint32_t kMaxNumericWords = 256;
    if (!c || !c->thread || !c->thread->nativeProcedureFrames ||
        !c->thread->nativeProcedureFrames->enabled() ||
        !c->thread->nativeProcedureFrames->sourceUnchanged() ||
        !count || count > kMaxNumericWords || !words || !descriptors ||
        !compiledRevision || !siteId || (digestLo == 0 && digestHi == 0) ||
        !pointerOrigins || !aliasArguments ||
        !generatedStringSources ||
        distinctStackAllocas > stackDerivedPointers ||
        stackDerivedPointers > unresolvedPointers ||
        uint64_t(unresolvedPointers) + uint64_t(unresolvedOther) !=
            uint64_t(unsupported))
        return;
    // Compile-time constant metadata is an opaque borrowed pointer.
    // Read exactly seven small integer counters synchronously. Never
    // retain that pointer and never read any native pointer *values*.
    std::array<uint32_t, 7> originCounts{};
    uint64_t originSum = 0;
    for (size_t i = 0; i < originCounts.size(); ++i) {
        originCounts[i] = pointerOrigins[i];
        originSum += originCounts[i];
    }
    if (originSum != unresolvedPointers)
        return;
    const auto state = c->thread->nativeProcedureFrames->sourceCheckpoint();
    if (!state || state->phase != ir::SourceResumePhase::AfterLoopBody ||
        state->loops.empty() ||
        state->frame.resumePc != state->loops.back().sourcePc)
        return;
    // Test-only *real native Fiber* explicit GC call at a live
    // generated-string SSA checkpoint, BEFORE any source snapshot
    // handle is created. The opt-in native collection guard must
    // record the request rather than sweeping these unrooted words.
    // Never run this injection when the root experiment is disabled.
    static const bool testExplicitGc = [] {
        const char* v = std::getenv(
            "XYO_RT_TEST_EXPLICIT_GC_BEFORE_SSA_ROOT");
        const char* roots = std::getenv(
            "XYO_RT_REQUIRE_GENERATED_SSA_ROOTS");
        return v && v[0] == '1' && roots && roots[0] == '1';
    }();
    static std::atomic<bool> injectedExplicitGc{false};
    if (testExplicitGc &&
        std::any_of(generatedStringSources,
                    generatedStringSources + count,
                    [](uint32_t proof) { return proof == 1; }) &&
        !injectedExplicitGc.exchange(true, std::memory_order_acq_rel)) {
        auto& gc = Gc::get();
        const uint64_t before = gc.collectCount();
        gc.collect(); // Must be deferred by native-only RAII guard.
        std::fprintf(stderr,
            "[native-ssa-explicit-gc-guard] before=%llu after=%llu "
            "unchanged=%d pending=%d\n",
            static_cast<unsigned long long>(before),
            static_cast<unsigned long long>(gc.collectCount()),
            int(before == gc.collectCount()),
            int(gc.explicitCollectionPending()));
    }
    auto detached = std::make_unique<NativeRtSsaCapture>();
    detached->sourceRevision = compiledRevision;
    detached->digestLo = digestLo;
    detached->digestHi = digestHi;
    detached->sourcePc = state->frame.resumePc;
    detached->procedureId = state->frame.procedureId;
    detached->siteId = siteId;
    detached->unsupported = unsupported;
    detached->unresolvedPointers = unresolvedPointers;
    detached->unresolvedOther = unresolvedOther;
    detached->pointerOrigins = originCounts;
    detached->stackDerivedPointers = stackDerivedPointers;
    detached->distinctStackAllocas = distinctStackAllocas;
    detached->numericComplete = numericComplete == 1 && unsupported == 0;
    detached->descriptors.assign(descriptors, descriptors + count);
    detached->words.assign(words, words + count);
    detached->aliasArguments.assign(
        aliasArguments, aliasArguments + count);
    // The independent original compiler mapping is stored separately
    // from the runtime copy, not recovered from mutable SSA words.
    detached->compilerAliasArguments.assign(
        aliasArguments, aliasArguments + count);
    // Separate original + mutable flags for compiler-proven native
    // join-result i64 instructions. Never treat pointer/raw numeric
    // words as GC Values on the strength of this metadata alone.
    detached->generatedStrings.assign(
        generatedStringSources, generatedStringSources + count);
    detached->compilerGeneratedStrings.assign(
        generatedStringSources, generatedStringSources + count);
    // Reject malformed/inconsistent compiler metadata, even in the
    // diagnostic path. ID 0 is valid; widths must be native i1/i32/i64.
    for (size_t i = 0; i < count; ++i) {
        const auto kindWidth =
            uint32_t(detached->descriptors[i] >> 32);
        const uint32_t kind = kindWidth & ~uint32_t{255};
        const uint32_t width = kindWidth & uint32_t{255};
        const bool integer =
            (kind == 0 || kind == 0x200) &&
            (width == 1 || width == 32 || width == 64);
        const bool floating =
            (kind == 0x100 || kind == 0x300) &&
            (width == 32 || width == 64);
        if ((!integer && !floating) ||
            (width == 1 && detached->words[i] > 1) ||
            (width == 32 && detached->words[i] > UINT32_MAX))
            return;
        const uint32_t generated = detached->generatedStrings[i];
        if (generated > 1 || (generated &&
            (kind != 0 || width != 64 ||
             (uint32_t(detached->descriptors[i]) & 0x80000000u) ||
             detached->aliasArguments[i])))
            return;
        const uint32_t alias = detached->aliasArguments[i];
        if (alias) {
            // An exact native Scratch argument is always an i64.
            // Bare instruction SSA words need a separate compiler
            // PHI/SELECT identity proof; all other kinds are unsafe.
            if ((kind != 0 && kind != 0x200) || width != 64 ||
                !state->frame.procedureId ||
                alias > state->frame.arguments)
                return;
            if (kind == 0x200) {
                const uint32_t id =
                    uint32_t(detached->descriptors[i]);
                if (!(id & 0x80000000u) ||
                    (id & 0x7fffffffu) != alias)
                    return;
            } else if (
                uint32_t(detached->descriptors[i]) & 0x80000000u) {
                return;
            }
        } else if (kind == 0x200 && width == 64 &&
                   !state->frame.procedureId) {
            // Root scripts have no Scratch procedure arguments.
            return;
        }
        if (i && uint32_t(detached->descriptors[i]) <=
                     uint32_t(detached->descriptors[i - 1]))
            return;
    }
    detached->fingerprint = detached->computeFingerprint();

    // Compiler-origin expectation from the emitted LLVM site's
    // read-only descriptors, independent of the mutable runtime
    // snapshot. The original source frame supplies proc/resumePC.
    auto manifest = std::make_unique<ir::SourceTypedSsaManifest>();
    manifest->revision = compiledRevision;
    manifest->digestLo = digestLo;
    manifest->digestHi = digestHi;
    manifest->procedureId = state->frame.procedureId;
    manifest->resumePc = state->frame.resumePc;
    manifest->siteId = siteId;
    manifest->descriptors.assign(descriptors, descriptors + count);
    if (!manifest->valid() ||
        manifest->descriptors != detached->descriptors)
        return;
    // Update both together after successful validation: no new
    // checkpoint can borrow a previous site's manifest.
    detached->compilerManifest = std::move(*manifest);
    // Explicit opt-in: while the native Fiber is STILL suspended,
    // retain ONLY live i64 strings produced by trusted join helpers
    // or an all-arms-proven PHI/SELECT. Check exact allocated GC
    // object membership BEFORE registering any handle.
    // This cannot retroactively protect a string already collected
    // before the native source checkpoint.
    static const bool rootGenerated = [] {
        const char* v = std::getenv("XYO_RT_REQUIRE_GENERATED_SSA_ROOTS");
        return v && v[0] == '1';
    }();
    // IR-001: runtime evidence only. Compiler reporter-origin metadata
    // is owned by the CompiledScript. The detached SSA words and
    // independent source CallFrame roots are separately verified.
    static const bool auditReporter = [] {
        const char* flag =
            std::getenv("XYO_IR001_RUNTIME_REPORTER_AUDIT");
        const char* require =
            std::getenv("XYO_RT_REQUIRE_REPORTER_SSA_AUDIT");
        return (flag && flag[0] == '1') ||
               (require && require[0] == '1');
    }();
    // The GC audit's exact handle vector is mandatory even if no
    // generated strings are present. Never synthesize a rooted value
    // from a list/variable reporter's plain NaN-boxed i64.
    if ((rootGenerated || auditReporter) &&
        !rt::retainGeneratedSsaRoots(*detached)) return;
    if (auditReporter) {
        const auto* compiled = static_cast<const CompiledScript*>(
            c->thread->compiled);
        const auto* entry = compiled && compiled->nativeReporterSites
            ? compiled->nativeReporterSites->find(siteId) : nullptr;
        const auto image =
            c->thread->nativeProcedureFrames->exportSelfContained();
        const auto result = entry && image && image->frames
            ? ir::auditSourceReporterSsaLocations(
                entry->gap, entry->frozen, *detached->compilerManifest,
                *detached, *image->frames)
            : std::nullopt;
        const auto owners = result && c->thread->target && entry
            ? ir::auditCompiledReporterStorageOwners(
                entry->frozen.reporters, *c->thread->target)
            : std::nullopt;
        // Actual GC allocation membership + exact-root handle capture.
        // Native GC is deferred during the Fiber execution window.
        // This protects the historical source value but does not give
        // it an interpreter frame slot or replay-safe effect context.
        bool inputRoots = result && entry &&
            ir::retainSourceReporterInputStrings(
                *detached, entry->frozen, *result,
                *detached->compilerManifest);
        // Test-only negative path on a REAL live Scratch list read:
        // remove the exact handle after capture, before verification.
        // Source SSA words and current native Fiber remain untouched.
        if (inputRoots) {
            static const bool dropRootForTesting = [] {
                const char* flag = std::getenv(
                    "XYO_IR001_TEST_DROP_REPORTER_INPUT_ROOT");
                return flag && flag[0] == '1';
            }();
            if (dropRootForTesting) {
                for (auto& handle : detached->reporterInputHandles) {
                    if (!handle) continue;
                    if (detached->reporterInputRoots)
                        detached->reporterInputRoots->release(handle);
                    handle = {};
                    break;
                }
            }
            inputRoots = ir::verifySourceReporterInputStrings(
                *detached, entry->frozen, *detached->compilerManifest);
        }
        const auto inputSlots = inputRoots
            ? ir::auditReporterInputSlots(
                entry->frozen, *result, *detached->compilerManifest,
                *detached)
            : std::nullopt;
        // Observe real interpreter _ops/input destinations only.
        // Never write a parent ArgValues slot or restore StackFrame here.
        const auto destinations = inputSlots && entry &&
                c->thread->blockContainer
            ? ir::auditReporterInterpreterDestinations(
                entry->frozen, *inputSlots,
                *c->thread->blockContainer)
            : std::nullopt;
        std::fprintf(stderr,
            "[ir001-runtime-reported-entry-map] site=%llu "
            "mapped=%u unavailable=%u rootless=%u duplicate=%u "
            "incomplete_prefix=%u contiguous_prefix=%u "
            "missing_prefix_statements=%u input_count=%zu "
            "source_complete=0 executable=0\n",
            static_cast<unsigned long long>(siteId),
            destinations ? destinations->mapped : 0,
            destinations ? destinations->unavailable : 0,
            destinations ? destinations->rootlessValue : 0,
            destinations ? destinations->duplicatedSource : 0,
            destinations ? destinations->incompletePrefix : 0,
            destinations ? destinations->contiguousReportedPrefix : 0,
            destinations ? destinations->statementsWithMissingPrefix : 0,
            inputSlots ? inputSlots->inputs.size() : size_t{0});
        if (destinations) for (const auto& missing
                : destinations->missingPrefixOps) {
            std::fprintf(stderr,
                "[ir001-runtime-reported-entry-missing] site=%llu "
                "statement=%s op_id=%s opcode=%s position=%u "
                "source_complete=0 executable=0\n",
                static_cast<unsigned long long>(siteId),
                missing.statementBlock.c_str(), missing.opCachedId.c_str(),
                missing.opcode.c_str(), missing.operationIndex);
        }
        if (destinations) for (const auto& destination
                : destinations->destinations) {
            std::fprintf(stderr,
                "[ir001-runtime-reported-entry-slot] site=%llu "
                "reporter_pc=%llu op_id=%s parent=%s input=%s "
                "statement=%s position=%u ops=%u "
                "kind=%u source_complete=0 executable=0\n",
                static_cast<unsigned long long>(siteId),
                static_cast<unsigned long long>(destination.reporterPc),
                destination.reporterBlock.c_str(),
                destination.parentBlock.c_str(),
                destination.parentInput.c_str(),
                destination.containingStatement.c_str(),
                destination.statementOperationIndex,
                destination.statementOperationCount,
                unsigned(destination.valueKind));
        }
        auto* reads = c->thread->nativeSourceListReads.get();
        // Test-only duplicate read-origin injection. The LLVM SSA
        // word and Scratch project state are not modified; a duplicated
        // source evaluation becomes ambiguous and must be rejected.
        static const bool duplicateRead = [] {
            const char* flag = std::getenv(
                "XYO_IR001_TEST_DUPLICATE_LIST_EVAL");
            return flag && flag[0] == '1';
        }();
        if (reads && duplicateRead && !reads->pending.empty() &&
            reads->pending.size() < ir::SourceListReadEvents::kMaxEvents)
            reads->pending.push_back(reads->pending.back());
        if (entry && reads && detached->compilerManifest) {
            auto indexed = ir::auditListReadsAtCheckpoint(
                entry->frozen, *detached->compilerManifest,
                *detached, *reads);
            detached->reporterListReadEvidence =
                std::make_shared<ir::SourceListReadCheckpointAudit>(
                    std::move(indexed));
        }
        // The source PC dictionary is owned by the compiler, not
        // reconstructed from current mutable source or SSA values.
        // Record historical scalar reads even when LLVM has DCE'd the
        // first reporter's live SSA word before this checkpoint.
        auto* scalarReads = c->thread->nativeSourceScalarReads.get();
        if (destinations && scalarReads && image && image->symbols &&
            c->thread->blockContainer && entry) {
            const auto sourceId = [&](uint64_t pc) -> std::string_view {
                const auto id = image->symbols->block(pc);
                return id ? *id : std::string_view{};
            };
            auto historical = ir::auditHistoricalScalarPrefix(
                *destinations, *scalarReads,
                detached->reporterListReadEvidence.get(),
                *c->thread->blockContainer, entry->frozen.revision,
                sourceId);
            ir::auditLiveScalarReadWords(
                historical, entry->frozen, *detached->compilerManifest,
                *detached, *scalarReads, *c->thread->blockContainer,
                sourceId);
            static const bool dropScalarRootForTesting = [] {
                const char* flag =
                    std::getenv("XYO_IR001_TEST_DROP_SCALAR_READ_ROOT");
                return flag && flag[0] == '1';
            }();
            if (dropScalarRootForTesting && historical.roots)
                for (const auto& observed : historical.live) {
                    if (!observed.evaluated.resultRoot) continue;
                    historical.roots->release(
                        observed.evaluated.resultRoot);
                    break;
                }
            const bool exactHistoricalLive = ir::verifyLiveScalarReadWords(
                historical, entry->frozen, *detached->compilerManifest,
                *detached, *c->thread->blockContainer, sourceId);
            std::fprintf(stderr,
                "[ir001-runtime-scalar-root-check] site=%llu "
                "valid=%d live=%u source_complete=0 executable=0\n",
                static_cast<unsigned long long>(siteId),
                int(exactHistoricalLive), historical.matchedLive);
            detached->reporterScalarPrefixEvidence =
                std::make_shared<ir::SourceScalarPrefixAudit>(
                    std::move(historical));
        }
        if (scalarReads) scalarReads->clear();
        const auto& historical = detached->reporterScalarPrefixEvidence;
        if (destinations && historical && c->thread->blockContainer) {
            auto planned = ir::planEvaluatedReportedPrefix(
                *destinations, historical.get(),
                detached->reporterListReadEvidence.get(),
                *c->thread->blockContainer);
            detached->reporterPrefixPlan =
                std::make_shared<ir::SourceReportedPrefixPlan>(
                    std::move(planned));
        }
        const auto& prefix = detached->reporterPrefixPlan;
        std::fprintf(stderr,
            "[ir001-runtime-reported-prefix-plan] site=%llu "
            "captured=%u unsupported=%u missing=%u ambiguous=%u "
            "invalid_roots=%u order_bad=%u gaps=%u "
            "verified=%d executable=0\n",
            static_cast<unsigned long long>(siteId),
            prefix ? prefix->capturedOperations : 0,
            prefix ? prefix->unsupportedReporters : 0,
            prefix ? prefix->missingValues : 0,
            prefix ? prefix->ambiguousValues : 0,
            prefix ? prefix->invalidRoots : 0,
            prefix ? prefix->orderViolations : 0,
            prefix ? prefix->gapBeforeLast : 0,
            int(bool(prefix && destinations &&
                ir::verifyEvaluatedReportedPrefix(
                    *prefix, *destinations,
                    *c->thread->blockContainer))));
        std::fprintf(stderr,
            "[ir001-runtime-scalar-prefix] site=%llu "
            "matched=%u missing=%u ambiguous=%u wrong_owner=%u "
            "order_unproven=%u rejected=%u "
            "live=%u live_missing=%u live_ambiguous=%u live_invalid=%u "
            "expected=%zu source_complete=0 executable=0\n",
            static_cast<unsigned long long>(siteId),
            historical ? historical->matched : 0,
            historical ? historical->missing : 0,
            historical ? historical->ambiguous : 0,
            historical ? historical->mismatchedOwner : 0,
            historical ? historical->orderUnproven : 0,
            historical ? historical->rejected : 0,
            historical ? historical->matchedLive : 0,
            historical ? historical->missingLive : 0,
            historical ? historical->ambiguousLive : 0,
            historical ? historical->invalidLive : 0,
            destinations ? destinations->missingPrefixOps.size() : size_t{0});
        if (historical) for (const auto& observed : historical->live) {
            std::fprintf(stderr,
                "[ir001-runtime-live-scalar] site=%llu "
                "reporter_pc=%llu variable_id=%s seq=%llu "
                "rooted=%d source_complete=0 executable=0\n",
                static_cast<unsigned long long>(siteId),
                static_cast<unsigned long long>(
                    observed.evaluated.reporterPc),
                observed.evaluated.variableId.c_str(),
                static_cast<unsigned long long>(
                    observed.evaluated.sequence),
                int(bool(observed.evaluated.resultRoot)));
        }
        if (historical) for (const auto& item : historical->items) {
            std::fprintf(stderr,
                "[ir001-runtime-scalar-prefix-item] site=%llu "
                "op_id=%s variable_id=%s position=%u seq=%llu "
                "rooted=%d source_complete=0 executable=0\n",
                static_cast<unsigned long long>(siteId),
                item.missing.opCachedId.c_str(),
                item.evaluated.variableId.c_str(),
                item.missing.operationIndex,
                static_cast<unsigned long long>(item.evaluated.sequence),
                int(bool(item.evaluated.resultRoot)));
        }
        // The next physical checkpoint begins a new bounded history.
        // The detached snapshot owns the old event's string root provider.
        if (c->thread->nativeSourceListReads)
            c->thread->nativeSourceListReads->clear();
        const auto& indexed = detached->reporterListReadEvidence;
        if (indexed && entry) {
            for (const auto& event : indexed->evidence) {
                const auto it = std::find_if(
                    entry->frozen.reporters.begin(),
                    entry->frozen.reporters.end(),
                    [&](const auto& binding) {
                        return binding.reporterPc == event.reporterPc &&
                            binding.storage.opcode == "data_itemoflist";
                    });
                if (it == entry->frozen.reporters.end()) continue;
                std::fprintf(stderr,
                    "[ir001-runtime-list-index-item] site=%llu "
                    "reporter_pc=%llu list_id=%s index_source=%s "
                    "index0=%lld seq=%llu len_after=%u "
                    "storage_epoch_after=%llu rooted=%d "
                    "content_unchanged=0 executable=0\n",
                    static_cast<unsigned long long>(siteId),
                    static_cast<unsigned long long>(event.reporterPc),
                    event.listId.c_str(),
                    it->storage.indexBlock.c_str(),
                    static_cast<long long>(event.numericPosition
                        ? int64_t(*event.numericPosition) : int64_t(-1)),
                    static_cast<unsigned long long>(event.sequence),
                    event.lengthAfterRead,
                    static_cast<unsigned long long>(
                        event.storageEpochAfterRead),
                    int(bool(event.resultRoot)));
            }
        }
        std::fprintf(stderr,
            "[ir001-runtime-list-content-epoch] site=%llu "
            "stable_since_observer=%u changed_since_observer=%u "
            "untracked=%u content_unchanged=0 executable=0\n",
            static_cast<unsigned long long>(siteId),
            indexed ? indexed->epochStableSinceObserver : 0,
            indexed ? indexed->epochChangedSinceObserver : 0,
            indexed ? indexed->epochUntracked : 0);
        std::fprintf(stderr,
            "[ir001-runtime-list-index] site=%llu "
            "matched=%u numeric_index=%u missing=%u ambiguous=%u "
            "value_mismatch=%u rejected=%u "
            "content_unchanged=0 executable=0\n",
            static_cast<unsigned long long>(siteId),
            indexed ? indexed->matchedReads : 0,
            indexed ? indexed->indexPositions : 0,
            indexed ? indexed->missingReads : 0,
            indexed ? indexed->ambiguousReads : 0,
            indexed ? indexed->mismatchedValues : 0,
            indexed ? indexed->rejectedReads : 0);
        // Missing or ambiguous execution-time list reads cannot serve
        // as the origin of a supposedly matching source reporter SSA.
        // This refuses only the detached diagnostic snapshot, never the
        // still-running native Fiber.
        const uint32_t expectedListReads = entry
            ? uint32_t(std::count_if(
                entry->frozen.reporters.begin(),
                entry->frozen.reporters.end(),
                [](const auto& reporter) {
                    return reporter.origin ==
                        ir::SourceReporterOrigin::ListRead &&
                        reporter.storage.opcode == "data_itemoflist";
                }))
            : 0;
        const bool listReadProof = !expectedListReads ||
            (indexed && indexed->matchedReads == expectedListReads &&
             indexed->missingReads == 0 &&
             indexed->ambiguousReads == 0 &&
             indexed->mismatchedValues == 0 &&
             indexed->rejectedReads == 0 &&
             indexed->epochStableSinceObserver == expectedListReads &&
             indexed->epochChangedSinceObserver == 0 &&
             indexed->epochUntracked == 0);
        std::fprintf(stderr,
            "[ir001-runtime-list-read-proof] site=%llu "
            "expected=%u accepted=%d source_complete=0 executable=0\n",
            static_cast<unsigned long long>(siteId),
            expectedListReads, int(listReadProof));
        std::fprintf(stderr,
            "[ir001-runtime-reporter-inputs] site=%llu matched=%d "
            "inputs=%zu immediate=%u rooted_heap=%u unrooted_heap=%u "
            "nonboxed=%u invalid=%u ambiguous=%u "
            "restorable=0 executable=0\n",
            static_cast<unsigned long long>(siteId),
            int(bool(inputSlots)),
            inputSlots ? inputSlots->inputs.size() : size_t{0},
            inputSlots ? inputSlots->immediateCount : 0,
            inputSlots ? inputSlots->rootedHeapObservations : 0,
            inputSlots ? inputSlots->unrootedHeapObservations : 0,
            inputSlots ? inputSlots->nonBoxedCount : 0,
            inputSlots ? inputSlots->invalidBoxedCount : 0,
            inputSlots ? inputSlots->ambiguousInputs : 0);
        std::fprintf(stderr,
            "[ir001-runtime-reporter-owners] site=%llu "
            "matched=%d scalars=%u lists=%u missing=%u wrong_type=%u "
            "nonstorage=%u unresolved=%u "
            "value_restorable=0 executable=0\n",
            static_cast<unsigned long long>(siteId), int(bool(owners)),
            owners ? owners->typedScalarOwners : 0,
            owners ? owners->typedListOwners : 0,
            owners ? owners->missingOwners : 0,
            owners ? owners->wrongTypeOwners : 0,
            owners ? owners->nonStorageOrigins : 0,
            owners ? owners->unresolvedOrigins : 0);
        if (owners) {
            detached->reporterOwnerReadback = true;
            detached->reporterOwnerTarget = c->thread->target;
            detached->reporterOwnerStage = owners->epoch.stage;
            detached->reporterOwnerLocalGeneration =
                owners->epoch.localGeneration;
            detached->reporterOwnerStageGeneration =
                owners->epoch.stageGeneration;
        }
        std::fprintf(stderr,
            "[ir001-runtime-reporter-join] site=%llu pc=%llu "
            "matched=%d "
            "observed=%zu arguments=%u generated=%u "
            "heap_roots=%u unmapped=%u unsupported=%u "
            "source_complete=0 executable=0\n",
            static_cast<unsigned long long>(siteId),
            static_cast<unsigned long long>(state->frame.resumePc),
            int(bool(result)),
            result ? result->reporters.size() : size_t(0),
            result ? result->mappedArguments : 0,
            result ? result->mappedGeneratedStrings : 0,
            result ? result->verifiedHeapStringObservations : 0,
            result ? result->unresolvedReporters : 0,
            result ? result->unsupportedLlvmValues : 0);
        // Unmatched compiler slot means no trustworthy source
        // destination for this SSA copy. Never retire the Fiber.
        if (!result || !inputSlots || !listReadProof) return;
    }
    c->thread->nativeRtSsaCapture = std::move(detached);
}

// Optional native LLVM bridge to the same scheduler-owned CallFrame
// format used by the WebAssembly return-PC implementation. Generated
// calls pass the actual register arguments before the potentially-yielding
// Scratch procedure push. No GC handles are copied into the frame.
// New compiled-source-versioned native CallFrame ABI. The epoch comes
// from the immutable ScratchCompilationUnit captured before LLVM lowering;
// never derive it from live Blocks at the first callsite.
void xyo_proc_frame_enter_rev(
    JitCtx* c, const char* blockId, const char* proccode,
    const uint64_t* args, uint32_t count, uint64_t compiledRevision) {
    if (!c || !c->thread) return;
    Thread& thread = *c->thread;
    if (!thread.nativeProcedureFrames) {
        if (!thread.blockContainer) return;
        const Block* root =
            thread.blockContainer->getBlock(thread.topBlock);
        if (!root) return;
        const ir::ScratchCompilationUnit source =
            ir::ScratchCompilationUnit::capture(*thread.blockContainer, root);
        thread.nativeProcedureFrames =
            std::make_unique<ir::NativeProcedureFrameSession>(
                source, thread.blockContainer, compiledRevision);
    }
    thread.nativeProcedureFrames->enter(blockId, proccode, args, count);
}

// Full source content provenance is required before a native image may
// be executed under a different backend. The original live source is
// re-captured on export (not at every hot call/yield).
void xyo_proc_frame_enter_verified(
    JitCtx* c, const char* blockId, const char* proccode,
    const uint64_t* args, uint32_t count, uint64_t compiledRevision,
    uint64_t digestLo, uint64_t digestHi) {
    if (!c || !c->thread) return;
    Thread& thread = *c->thread;
    if (!thread.nativeProcedureFrames) {
        if (!thread.blockContainer) return;
        const Block* root =
            thread.blockContainer->getBlock(thread.topBlock);
        if (!root) return;
        const auto source = ir::ScratchCompilationUnit::capture(
            *thread.blockContainer, root);
        thread.nativeProcedureFrames =
            std::make_unique<ir::NativeProcedureFrameSession>(
                source, thread.blockContainer, compiledRevision,
                ir::ScratchSourceDigest{digestLo, digestHi});
    }
    thread.nativeProcedureFrames->enter(blockId, proccode, args, count);
}

// Verified callsite + lexical loop path on first root call. Native
// ProcedureFrameSession is created lazily by its first native callee;
// any earlier root repeat checkpoint had no live session to receive it.
// The LLVM code generator passes the *same* counter SSA bits used by
// its ordinary checkpoint helper, immediately before xyo_proc_push.
// Once a child frame exists the ordinary path helper already captured
// the parent, and this additional root-only publication is skipped.
void xyo_proc_frame_enter_verified_rootpath(
    JitCtx* c, const char* blockId, const char* proccode,
    const uint64_t* args, uint32_t count, uint64_t compiledRevision,
    uint64_t digestLo, uint64_t digestHi,
    const char* const* sourceLoopIds, const uint64_t* counterBits,
    uint32_t loopCount) {
    if (!c || !c->thread) return;
    Thread& thread = *c->thread;
    if (!thread.nativeProcedureFrames) {
        if (!thread.blockContainer) return;
        const Block* root =
            thread.blockContainer->getBlock(thread.topBlock);
        if (!root) return;
        const auto source = ir::ScratchCompilationUnit::capture(
            *thread.blockContainer, root);
        thread.nativeProcedureFrames =
            std::make_unique<ir::NativeProcedureFrameSession>(
                source, thread.blockContainer, compiledRevision,
                ir::ScratchSourceDigest{digestLo, digestHi});
    }
    auto& frame = *thread.nativeProcedureFrames;
    if (frame.enabled() && frame.depth() == 1)
        frame.checkpointPath(sourceLoopIds, counterBits, loopCount,
                             ir::SourceResumePhase::InsideLoopBody);
    frame.enter(blockId, proccode, args, count);
}

// Legacy opt-in instrumentation entrypoint. Frames can be observed but
// have no compile-time provenance, so the portable deoptimizer refuses
// to transfer them into an interpreter or a different backend.
void xyo_proc_frame_enter(JitCtx* c, const char* blockId,
                          const char* proccode, const uint64_t* args,
                          uint32_t count) {
    xyo_proc_frame_enter_rev(c, blockId, proccode, args, count, 0);
}

void xyo_proc_frame_leave(JitCtx* c) {
    if (!c || !c->thread || !c->thread->nativeProcedureFrames) return;
    c->thread->nativeProcedureFrames->leave();
    // RT-001-only probe of an actual native AfterCallee safe point.
    // Must run AFTER updating the portable frame, BEFORE the caller
    // executes any further Scratch statement. The production path is
    // unchanged when the opt-in test flag is not set.
    static const bool testYieldAfterCallee = [] {
        const char* value = std::getenv("XYO_RT_TEST_YIELD_AFTER_CALLEE");
        return value && value[0] == '1';
    }();
    if (testYieldAfterCallee &&
        c->thread->nativeProcedureFrames->enabled())
        xyo_yield(c);
}

// Each source statement expires the previous post-call or loop-latch
// checkpoint before any of its own reporters/effects can execute.
void xyo_proc_frame_statement_started(JitCtx* c, const char* blockId) {
    if (c && c->thread && c->thread->nativeProcedureFrames)
        c->thread->nativeProcedureFrames->statementStarted(blockId);
}

// Explicitly invalidate the clean procedure-entry phase once code inside
// the callee can execute. No SSA state is assumed resumable at this point.
void xyo_proc_frame_body_started(JitCtx* c) {
    if (c && c->thread && c->thread->nativeProcedureFrames)
        c->thread->nativeProcedureFrames->bodyStarted();
}

// Store the source-level LLVM loop checkpoint before suspending the Fiber.
void xyo_proc_frame_checkpoint(JitCtx* c, const char* blockId,
                               uint64_t counterBits) {
    if (c && c->thread && c->thread->nativeProcedureFrames)
        c->thread->nativeProcedureFrames->checkpoint(blockId, counterBits);
}

// One lexical hierarchy is published as a single pointer-free CallFrame
// checkpoint. The pointers themselves are LLVM native call arguments only;
// no host/Gc-managed address is stored in the shared numeric frame arena.
void xyo_proc_frame_checkpoint_path(JitCtx* c, const char* const* ids,
                                    const uint64_t* counters, uint32_t count) {
    if (c && c->thread && c->thread->nativeProcedureFrames)
        c->thread->nativeProcedureFrames->checkpointPath(ids, counters, count);
}

// The final argument is a versioned source-level phase, never a native PC.
// The old checkpoint helper remains for existing opt-in ABI clients.
void xyo_proc_frame_checkpoint_path_phase(
    JitCtx* c, const char* const* ids,
    const uint64_t* counters, uint32_t count, uint32_t phase) {
    if (!c || !c->thread || !c->thread->nativeProcedureFrames) return;
    if (phase != uint32_t(ir::SourceResumePhase::InsideLoopBody) &&
        phase != uint32_t(ir::SourceResumePhase::AfterLoopBody)) {
        c->thread->nativeProcedureFrames->invalidate("invalid-resume-phase");
        return;
    }
    // This prior LLVM numeric snapshot belongs only to its previous
    // checkpoint. A new source marker must publish a new detached SSA
    // snapshot before its associated Fiber yield, or fail closed.
    c->thread->nativeRtSsaCapture.reset();
    c->thread->nativeProcedureFrames->checkpointPath(
        ids, counters, count, static_cast<ir::SourceResumePhase>(phase));
}

/// JIT 手続き呼出し中に interpreter スタックへ残す呼出しフレーム。
/// JS では procedures_call のスタックフレームが呼出し中残り、
/// isRecursiveCall（同一 proccode 検出で再帰 yield）や warpMode
/// 継承がこれを参照する — ネイティブ呼出しでも同じ形を再現する。
void xyo_proc_push(JitCtx* c, const char* blockId, const BlockCached* op,
                   int32_t defWarp, const char* procedureCode,
                   int32_t nestedBranchDepth, int32_t mayRecurse) {
    XYO_RT_PROF(kProc);
    Thread* t = c->thread;
    // Scratch thread.isRecursiveCall examines only six ancestors, including
    // structural control blocks. Native JIT does not materialize those branch
    // frames, so account for their depth at this particular source callsite.
    bool recursive = false;
    if (mayRecurse && !defWarp && nestedBranchDepth <= 5) {
        int remaining = 6 - nestedBranchDepth;
        for (auto it = t->stackFrames.rbegin(); it != t->stackFrames.rend() &&
             remaining > 0; ++it, --remaining) {
            const BlockCached* frameOp = (*it)->op;
            const Block* block = frameOp && t->target && t->target->blocks
                ? t->target->blocks->getBlock(frameOp->id) : nullptr;
            if (block && block->opcode == "procedures_call" && block->mutation &&
                block->mutation->attr("proccode") == procedureCode) {
                recursive = true;
                break;
            }
        }
    }
    t->pushStack(blockId);
    if (StackFrame* sf = t->peekStackFrame()) {
        // JS stepToProcedure: warpMode は親から継承し、定義が warp なら
        // 昇格 — 非 warp 定義では降格しない（呼出し時点の isWarp は
        // まだ呼出し元の値なので定義側フラグと OR する）。
        sf->warpMode = c->isWarp != 0 || defWarp != 0;
        sf->op = op;
    }
    // Match Scratch Sequencer.stepToProcedure: yield *after* pushing the
    // procedure frame, then execute its body when this thread resumes.
    if (recursive) xyo_loop_yield(c);
}
void xyo_proc_pop(JitCtx* c) {
    Thread* t = c->thread;
    t->popStack();
    // JIT フレームはネイティブ呼出し境界内で完結（execute() の
    // currentStackFrame 等の外部生ポインタが跨がない）— reset して
    // 再利用プールへ移し、make_unique/destroy の往復を避ける。
    if (!t->trashFrames.empty() && t->framePool.size() < 256) {
        auto& f = t->trashFrames.back();
        f->reset();
        t->framePool.push_back(std::move(f));
    }
    t->trashFrames.pop_back();
}

/// compat 入力: ブロック id の報告値をインタプリタで評価する。
/// reporter は stack に積んで bounded stepThread で評価。
uint64_t xyo_rt_reporter(JitCtx* c, const char* blockId) {
    XYO_RT_PROF(kProc);
    Thread* t = c->thread;
    const size_t depth = t->stack.size();
    t->pushStack(blockId);
    while (t->stack.size() > depth) {
        if (t->status == STATUS_YIELD || t->status == STATUS_YIELD_TICK ||
            t->status == STATUS_PROMISE_WAIT) {
            t->status = STATUS_RUNNING;
            xyo_yield(c);
            continue;
        }
        if (t->status == STATUS_DONE) break;
        c->sequencer->stepThreadBound(*t, depth);
        // stepThreadBound は yield 境界（ブロックの u.yield / 非 warp ループ
        // 反復境界）で status を STATUS_RUNNING に戻したまま早期 return する。
        // 完了でも DONE/PROMISE_WAIT でもなく stack が残っている = yield 点に
        // 到達した → 他スレッドに実行を譲る。
        if (t->stack.size() > depth && t->status == STATUS_RUNNING)
            xyo_yield(c);
    }
    Value out = t->justReported;
    t->justReported = Value::undefined();
    return toB(out);
}

// Experimental native Fiber continuation for the Scratch Translate
// reporter. The reporter itself still runs through the interpreter, but
// after an async HTTP request this helper leaves PROMISE_WAIT intact and
// parks the *native* fiber. It resumes at the exact LLVM callsite, preserving
// the caller's SSA values and source order. Enabled only by
// XYO_JIT_PLAMO_FIBER=1; default remains conservative full-script fallback.
uint64_t xyo_rt_async_reporter(JitCtx* c, const char* blockId) {
    Thread* thread = c->thread;
    const size_t depth = thread->stack.size();
    thread->pushStack(blockId);
    bool tracedAsyncRoots = false;
    while (thread->stack.size() > depth) {
        if (thread->status == STATUS_PROMISE_WAIT) {
            // Confirm that the exact host-owned provider survives *inside*
            // the parked native Fiber, not merely before the helper call.
            if (!tracedAsyncRoots &&
                std::getenv("XYO_RT_TRACE_ASYNC_VAR_ROOTS")) {
                tracedAsyncRoots = true;
                size_t heapRoots = 0;
                for (const auto& owner : thread->nativeAsyncVariableRoots)
                    heapRoots += owner->size();
                std::fprintf(stderr,
                    "[native-async-var-wait] owners=%zu heap_roots=%zu "
                    "gc_safe=0\n",
                    thread->nativeAsyncVariableRoots.size(), heapRoots);
            }
            // External resolution is the ONLY authority that may flip
            // PROMISE_WAIT back to RUNNING. Clearing it before the Fiber
            // switch would re-execute the reporter and duplicate HTTP.
            xyo_yield(c);
            continue;
        }
        if (thread->status == STATUS_YIELD ||
            thread->status == STATUS_YIELD_TICK) {
            thread->status = STATUS_RUNNING;
            xyo_yield(c);
            continue;
        }
        if (thread->status == STATUS_DONE || thread->isKilled)
            break;
        c->sequencer->stepThreadBound(*thread, depth);
        if (thread->stack.size() > depth &&
            thread->status == STATUS_RUNNING)
            xyo_yield(c);
    }
    Value result = thread->justReported;
    thread->justReported = Value::undefined();
    return toB(result);
}

// Experimental exact pre-wait roots for verified live Variable::value
// LLVM i64 loads. The generated call passes a BORROWED native stack array:
// each word is copied to a Thread-owned exact GC handle before any yield.
// Never retain the passed raw array pointer or classify arbitrary SSA bits.
// Most importantly, this is NOT a certificate for collecting other live
// unclassified SSA, pointers, stacks or pending scheduler effects.
#if defined(__linux__) && defined(__x86_64__) && \
    (defined(__GNUC__) || defined(__clang__))
__attribute__((noinline))
#endif
uint64_t xyo_rt_async_reporter_rooted(
    JitCtx* c, const char* blockId, const uint64_t* words, uint32_t count) {
    if (!c || !c->thread || !blockId)
        return toB(Value::undefined());
    if (!words || !count || count > 64)
        return xyo_rt_async_reporter(c, blockId);
    Thread& thread = *c->thread;
    const uintptr_t previousReturnPc =
        thread.nativeAsyncReporterReturnPc;
    const uintptr_t previousFramePtr =
        thread.nativeAsyncReporterFramePtr;
    const uintptr_t previousInputBuffer =
        thread.nativeAsyncReporterInputBuffer;
    const uint32_t previousInputWords =
        thread.nativeAsyncReporterInputWords;
    // A reporter's *physical* caller address cannot be reconstructed
    // from LLVM IR when a Fiber switches deep inside its C++ helper.
    // Capture the active ABI call-return address while that frame is
    // on-stack. Only diagnostic: it does not prove which callee-saved or
    // volatile registers hold all Scratch GC values.
#if defined(__linux__) && defined(__x86_64__) && \
    (defined(__GNUC__) || defined(__clang__))
    if (const char* flag =
            std::getenv("XYO_RT_TRACE_ASYNC_CALLSITE_PC");
        flag && flag[0] == '1') {
        thread.nativeAsyncReporterReturnPc =
            reinterpret_cast<uintptr_t>(
                __builtin_extract_return_addr(__builtin_return_address(0)));
        thread.nativeAsyncReporterFramePtr =
            reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
        thread.nativeAsyncReporterInputBuffer =
            reinterpret_cast<uintptr_t>(words);
        thread.nativeAsyncReporterInputWords = count;
    }
#endif
    const auto depth = thread.nativeAsyncVariableRoots.size();
    auto owner = std::make_unique<ir::GcValueHandles>();
    Gc& gc = Gc::get();
    uint32_t rooted = 0, immediate = 0;
    bool valid = true;
    for (uint32_t i = 0; i < count; ++i) {
        const Value value{words[i]};
        if (!value.isString()) {
            ++immediate;
            continue;
        }
        if (value.isEmptyString()) {
            ++immediate;
            continue;
        }
        const uintptr_t address =
            reinterpret_cast<uintptr_t>(value.asStrPtr());
        if (address < sizeof(GcHeader)) {
            valid = false;
            break;
        }
        auto* header = reinterpret_cast<GcHeader*>(
            address - sizeof(GcHeader));
        if (!gc.isObject(header) || header->type != GcType::Str ||
            !owner->retain(value)) {
            valid = false;
            break;
        }
        ++rooted;
    }
    static const bool trace = [] {
        const char* v = std::getenv("XYO_RT_TRACE_ASYNC_VAR_ROOTS");
        return v && v[0] == '1';
    }();
    if (trace)
        std::fprintf(stderr,
            "[native-async-var-root] count=%u rooted=%u immediate=%u "
            "valid=%d gc_safe=0\n", count, rooted, immediate, int(valid));
    // The rest of the active compiled function may have unproven roots.
    // Retain verified strings if and only if all compiler-certified
    // words pass runtime membership validation; otherwise keep the
    // original protected-Fiber path without any GC authorization.
    if (valid) {
        thread.nativeAsyncVariableRoots.push_back(std::move(owner));
    } else {
        // A cancelled Fiber does not unwind C++ local destructors.
        // NEVER enter an async yield with a registered GC provider
        // owned solely by this helper's disposable native stack.
        owner.reset();
    }
    const uint64_t result = xyo_rt_async_reporter(c, blockId);
    // A cancelled Fiber is released without unwinding this frame.
    // Sequencer::retireThread / Thread::releaseFinished own that cleanup.
    if (valid && thread.nativeAsyncVariableRoots.size() == depth + 1)
        thread.nativeAsyncVariableRoots.pop_back();
    if (trace)
        std::fprintf(stderr,
            "[native-async-var-release] owners=%zu gc_safe=0\n",
            thread.nativeAsyncVariableRoots.size());
    thread.nativeAsyncReporterReturnPc = previousReturnPc;
    thread.nativeAsyncReporterFramePtr = previousFramePtr;
    thread.nativeAsyncReporterInputBuffer = previousInputBuffer;
    thread.nativeAsyncReporterInputWords = previousInputWords;
    return result;
}

// Load a live, GC-rooted procedure argument exactly once at the native
// continuation entry. This is NOT an independent root: the caller's original
// interpreter StackFrame continues to own the Value throughout the fiber.
// An undefined result is only a sentinel: codegen calls the full reporter at
// each actual use, so dynamic special-name fallbacks stay observable.
// Called only at the *post-body* native counted warp loop yield site,
// after all promoted scalar/list writes have been published. The generated
// loop decrements its own private iteration count AFTER this call, so
// remainingAfterBody is already adjusted by one. While the Fiber is
// suspended, the serial scheduler can discard it and restore exactly this
// count to the interpreter's still-rooted original loop frame.
void xyo_rt_nested_osr_yield_checkpoint(
    JitCtx* c, double remainingAfterBody) {
    if (!c || !c->thread || !c->thread->nestedOsrActive ||
        !c->thread->compiled)
        return;
    const auto* compiled =
        static_cast<const CompiledScript*>(c->thread->compiled);
    if (!compiled->nestedOsrInFlightGuard ||
        !std::isfinite(remainingAfterBody) ||
        remainingAfterBody < 0.0 ||
        remainingAfterBody > 1099511627776.0 ||
        std::floor(remainingAfterBody) != remainingAfterBody)
        return;
    c->nestedOsrDeoptRemaining = remainingAfterBody;
    c->nestedOsrDeoptInnerRemaining = 0.0;
    c->nestedOsrDeoptDeepRemaining = 0.0;
    c->nestedOsrDeoptFourthRemaining = 0.0;
    c->nestedOsrDeoptPhase = 1;
    c->nestedOsrDeoptReady = 1;
}

// OSR-011. Both source loops have completed their most recent BODY and
// neither native loop has executed its private decrement. A real scheduler
// yield is necessary before the host may inspect/act on these counters.
// The original Interpreter outer frame stays live and GC-roots all args.
void xyo_rt_nested_osr_inner_yield_checkpoint(
    JitCtx* c, double outerAfterBody, double innerAfterBody) {
    if (!c || !c->thread || !c->thread->nestedOsrActive ||
        !c->thread->compiled) return;
    const auto* compiled =
        static_cast<const CompiledScript*>(c->thread->compiled);
    const auto valid = [](double v) {
        return std::isfinite(v) && v >= 0.0 &&
               v <= 1099511627776.0 && std::floor(v) == v;
    };
    // Reset to fail closed on malformed checkpoints: no older completed
    // outer-body snapshot may authorize replay from a stale source PC.
    c->nestedOsrDeoptReady = 0;
    c->nestedOsrDeoptPhase = 0;
    // Scratch rounds the initial TIMES (half toward +infinity) before
    // initializing its interpreted private loop counter. Native LLVM uses
    // an f64 countdown and checks i >= 0.5 instead. At a completed-body
    // yield its counter has not yet decremented, so a dynamic initial
    // TIMES of 17.5, for example, produces an after-body 16.5. The
    // interpreter source latch requires 17, not a fractional 16.5.
    // The native loop can reach a body only with current >= 0.5,
    // therefore the last valid after-body value is exactly -0.5.
    if (!std::isfinite(innerAfterBody) ||
        innerAfterBody < -0.5 ||
        innerAfterBody > 1099511627776.0)
        return;
    const double normalizedInner = std::floor(innerAfterBody + 0.5);
    if (!compiled->nestedOsrInFlightGuard ||
        !compiled->nestedOsrInnerYieldGuard ||
        compiled->nestedOsrInnerLoopId.empty() ||
        !valid(outerAfterBody) || !valid(normalizedInner))
        return;
    c->nestedOsrDeoptRemaining = outerAfterBody;
    c->nestedOsrDeoptInnerRemaining = normalizedInner;
    c->nestedOsrDeoptDeepRemaining = 0.0;
    c->nestedOsrDeoptFourthRemaining = 0.0;
    c->nestedOsrDeoptPhase = 2;
    c->nestedOsrDeoptReady = 1;
}

// OSR-013. No source/reporter is re-evaluated here: the three values
// come from the corresponding live LLVM counted-loop private counters.
void xyo_rt_nested_osr_triple_yield_checkpoint(
    JitCtx* c, double outerAfterBody, double middleAfterBody,
    double deepAfterBody) {
    if (!c || !c->thread || !c->thread->nestedOsrActive ||
        !c->thread->compiled) return;
    const auto* compiled =
        static_cast<const CompiledScript*>(c->thread->compiled);
    // Invalidate older phase-1/2 checkpoints before validating this
    // actual leaf-body-completed scheduler yield.
    c->nestedOsrDeoptReady = 0;
    c->nestedOsrDeoptPhase = 0;
    if (!compiled->nestedOsrInFlightGuard ||
        !compiled->nestedOsrInnerYieldGuard ||
        compiled->nestedOsrInnerLoopId.empty() ||
        compiled->nestedOsrDeepInnerLoopId.empty()) return;
    const auto validOuter = [](double v) {
        return std::isfinite(v) && v >= 0.0 &&
               v <= 1099511627776.0 && std::floor(v) == v;
    };
    const auto normalizeInner = [](double v) {
        return std::floor(v + 0.5);
    };
    const auto validNative = [](double v) {
        return std::isfinite(v) && v >= -0.5 &&
               v <= 1099511627776.0;
    };
    if (!validOuter(outerAfterBody) ||
        !validNative(middleAfterBody) ||
        !validNative(deepAfterBody)) return;
    const double middle = normalizeInner(middleAfterBody);
    const double deepest = normalizeInner(deepAfterBody);
    if (!validOuter(middle) || !validOuter(deepest)) return;
    c->nestedOsrDeoptRemaining = outerAfterBody;
    c->nestedOsrDeoptInnerRemaining = middle;
    c->nestedOsrDeoptDeepRemaining = deepest;
    c->nestedOsrDeoptFourthRemaining = 0.0;
    c->nestedOsrDeoptPhase = 3;
    c->nestedOsrDeoptReady = 1;
}

// OSR-014: strict four-level source continuation. Only a completely
// committed deepest native Scratch loop body may publish phase 4;
// three source ancestor bodies remain incomplete at this suspension.
void xyo_rt_nested_osr_quad_yield_checkpoint(
    JitCtx* c, double outerAfterBody, double middleAfterBody,
    double deepAfterBody, double fourthAfterBody) {
    if (!c || !c->thread || !c->thread->nestedOsrActive ||
        !c->thread->compiled) return;
    const auto* compiled =
        static_cast<const CompiledScript*>(c->thread->compiled);
    c->nestedOsrDeoptReady = 0;
    c->nestedOsrDeoptPhase = 0;
    if (!compiled->nestedOsrInFlightGuard ||
        !compiled->nestedOsrInnerYieldGuard ||
        compiled->nestedOsrInnerLoopId.empty() ||
        compiled->nestedOsrDeepInnerLoopId.empty() ||
        compiled->nestedOsrFourthLoopId.empty()) return;
    const auto validNative = [](double v) {
        return std::isfinite(v) && v >= -0.5 &&
               v <= 1099511627776.0;
    };
    const auto validCount = [](double v) {
        return std::isfinite(v) && v >= 0.0 &&
               v <= 1099511627776.0 &&
               std::floor(v) == v;
    };
    // Native counted-loop private counters are floating-point, while
    // Scratch Interpreter's saved source repeat latch is integral.
    // Convert each not-yet-decremented inner body result exactly once.
    if (!validCount(outerAfterBody) ||
        !validNative(middleAfterBody) ||
        !validNative(deepAfterBody) ||
        !validNative(fourthAfterBody)) return;
    const double middle = std::floor(middleAfterBody + 0.5);
    const double deep = std::floor(deepAfterBody + 0.5);
    const double fourth = std::floor(fourthAfterBody + 0.5);
    if (!validCount(middle) || !validCount(deep) ||
        !validCount(fourth)) return;
    c->nestedOsrDeoptRemaining = outerAfterBody;
    c->nestedOsrDeoptInnerRemaining = middle;
    c->nestedOsrDeoptDeepRemaining = deep;
    c->nestedOsrDeoptFourthRemaining = fourth;
    c->nestedOsrDeoptPhase = 4;
    c->nestedOsrDeoptReady = 1;
}

uint64_t xyo_rt_nested_osr_arg_peek(
    JitCtx* c, const char16_t* name, uint32_t length) {
    Thread* t = c ? c->thread : nullptr;
    const auto* compiled = t
        ? static_cast<const CompiledScript*>(t->compiled) : nullptr;
    if (!t || !compiled || !t->nestedOsrActive ||
        t->stack.size() < 2 || t->stack.size() > 15 ||
        t->stackFrames.size() != t->stack.size() ||
        compiled->nestedOsrFramePath.size() + 1 != t->stack.size() ||
        compiled->nestedOsrArgFrameIndex >=
            compiled->nestedOsrFramePath.size() ||
        t->stack[compiled->nestedOsrArgFrameIndex] !=
            compiled->nestedOsrCallId ||
        !t->stackFrames[compiled->nestedOsrArgFrameIndex] ||
        !t->stackFrames[compiled->nestedOsrArgFrameIndex]->hasParams ||
        !name || length > 4096)
        return toB(Value::undefined());
    const auto& params =
        t->stackFrames[compiled->nestedOsrArgFrameIndex]->params;
    const auto found = params.find(std::u16string(name, length));
    return found == params.end() ? toB(Value::undefined())
                                 : toB(found->second);
}

// Read-only argument bridge for an explicitly selected nested warp OSR
// region. The interpreter call frame remains on the Thread stack and is
// traced by the GC while the native fiber runs. No argument is copied into
// unrooted raw storage, nor is any native SSA value invented.
uint64_t xyo_rt_nested_osr_arg_get(
    JitCtx* c, const char* reporterBlockId,
    const char16_t* name, uint32_t length) {
    Thread* t = c ? c->thread : nullptr;
    const auto* compiled = t
        ? static_cast<const CompiledScript*>(t->compiled) : nullptr;
    if (t && compiled && t->nestedOsrActive &&
        t->stack.size() >= 2 && t->stack.size() <= 15 &&
        t->stackFrames.size() == t->stack.size() &&
        compiled->nestedOsrFramePath.size() + 1 == t->stack.size() &&
        compiled->nestedOsrArgFrameIndex <
            compiled->nestedOsrFramePath.size() &&
        t->stack[compiled->nestedOsrArgFrameIndex] ==
            compiled->nestedOsrCallId &&
        t->stackFrames[compiled->nestedOsrArgFrameIndex] &&
        t->stackFrames[compiled->nestedOsrArgFrameIndex]->hasParams &&
        name && length <= 4096) {
        const auto& params =
            t->stackFrames[compiled->nestedOsrArgFrameIndex]->params;
        const auto found = params.find(std::u16string(name, length));
        if (found != params.end() && !found->second.isUndefined()) {
            // A direct read bypasses the interpreter's reporter executor.
            // Count exactly that Scratch reporter once, buffered just like
            // native LLVM's other primitive operations.
            if (c->blockCounter) ++c->pendingBlockCount;
            return toB(found->second);
        }
    }
    // Preserve the interpreter's undefined/special-name semantics and
    // reporter-count accounting when the direct proof does not apply.
    return xyo_rt_reporter(c, reporterBlockId);
}

} // extern "C"
