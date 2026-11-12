// xyo-cpp: JIT 生成コードから呼ばれる extern "C" ランタイムヘルパ。
// jsexecute.js runtimeFunctions + cast/compare/list 系の移植。
// yield = fiber switch — C++ スタックが保持されるため、ヘルパ内部で
// 中断してもローカル状態は維持される（executeInCompatibilityLayer 相当）。
#include "jit.hpp"
#include "layq.hpp"
#include "rtprof.hpp"
#include "procedure_frame_state.hpp"

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
        xyo_yield(c);
    }
}

// A wait may become true and return to the *same* site within a warp
// loop. An unchanged site ID alone cannot prove no Scratch work occurred.
void xyo_wait_event_done(JitCtx* c) {
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
    if (!c->isWarp || isStuck(c)) xyo_yield(c);
}

// Poll without switching fibers or changing Scratch variables. The JIT can
// keep promoted scalars/lists in SSA if this returns false, and spills them
// only before an actual scheduler handoff. Only call in a warp loop.
int32_t xyo_warp_yield_due(JitCtx* c) {
    XYO_RT_PROF(kLoopY);
    return isStuck(c) ? 1 : 0;
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
    case 4: return jsnum::jsRound(v8math::sin(kPi * x / 180.0) * 1e10) / 1e10;
    case 5: return jsnum::jsRound(v8math::cos(kPi * x / 180.0) * 1e10) / 1e10;
    case 6: {
        // MathUtil.tan: %360、±90/270 は ±Infinity、他は 1e10 丸め
        double a = std::fmod(x, 360.0);
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

Variable* xyo_jitVar(JitCtx* c, const char* id, const char16_t* name,
                     int list) {
    XYO_RT_PROF(kVar);
#if !defined(XYO_DISABLE_RT_PROF)
    if (RTProf::get().enabled) ++g_varCtrs[0];
#endif
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
    t->jitVarSlots[size_t(idx)] = xyo_jitVar(c, id, name, isList);
    c->varSlotData = t->jitVarSlots.data();
}

uint64_t xyo_var_get(JitCtx* c, const char* id, const char16_t* name) {
    XYO_RT_PROF(kVar);
#if !defined(XYO_DISABLE_RT_PROF)
    if (RTProf::get().enabled) ++g_varCtrs[1];
#endif
    Variable* v = xyo_jitVar(c, id, name, false);
    return v ? toB(v->value) : toB(Value::undefined());
}

void xyo_var_set(JitCtx* c, const char* id, const char16_t* name,
                 uint64_t value) {
    XYO_RT_PROF(kVar);
#if !defined(XYO_DISABLE_RT_PROF)
    if (RTProf::get().enabled) ++g_varCtrs[2];
#endif
    Variable* v = xyo_jitVar(c, id, name, false);
    if (v) v->value = toV(value);
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
    c->thread->status = STATUS_DONE;
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
    c->thread->status = STATUS_DONE;
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
    thread.nativeProcedureFrames =
        std::make_unique<ir::NativeProcedureFrameSession>(
            source, thread.blockContainer, compiledRevision,
            ir::ScratchSourceDigest{digestLo, digestHi});
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
