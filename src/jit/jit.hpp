// xyo-cpp: Backend-neutral JIT runtime API — design.md §7。
// スクリプトを `void fn(JitCtx*)` にコンパイルし Fiber 上で実行する。
// yield は xyo_yield による fiber switch（任意の呼び出し深さから可能）。
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../aot/aot.hpp"
#include "layq.hpp"
#include "../core/value.hpp"
#include "../engine/deps.hpp"
#include "../gpu/gpu_isa.hpp"
#include "../gpu/gpu_compile.hpp"


namespace xyo {

struct Runtime;
struct Sequencer;
struct Target;
struct Thread;
struct Blocks;
struct Block;
struct ExecContext;
struct Variable;
struct NumericJitCode;
struct AotLazyCachedSlot;
class Fiber;
namespace wasm { struct HotCode; }
namespace ir { struct SourceFrameSymbols; }
namespace ir { struct SourceReporterRuntimeSites; }

// --- 実行モード -------------------------------------------------------
//
// HotSpot（既定）: スクリプトはインタプリタで開始し、呼び出し回数と
// 解釈実行したブロック数で JIT コンパイルを判定する。AOT（`xyo aot` /
// `--aot`）は明示指定時のみ使用する。Eager は従来動作（spawn 時に
// 全スクリプトをコンパイル）で、テスト/ベンチ用。
//
// ティアは 1 段（インタプリタ → 最適化 JIT）。LLVM の O1/インライン
// 無しの「速い C1」は実測でコンパイル時間が O2 の 6% しか短縮できず
// （IR emit が支配的）、C1→C2 の二重 emit が数秒の追加コストになる
// ため採用していない。
enum class JitMode { Off, HotSpot, Eager };

/// How to compile procedures within scripts selected for JIT.
enum class JitLazyMode { Auto, On, Off };

inline bool parseJitLazyMode(const char* s, JitLazyMode* out) {
    if (!s || !out) return false;
    if (!std::strcmp(s, "auto")) *out = JitLazyMode::Auto;
    else if (!std::strcmp(s, "on") || !std::strcmp(s, "yes") ||
             !std::strcmp(s, "1")) *out = JitLazyMode::On;
    else if (!std::strcmp(s, "off") || !std::strcmp(s, "no") ||
             !std::strcmp(s, "0")) *out = JitLazyMode::Off;
    else return false;
    return true;
}

namespace detail {
struct JitLazyState {
    JitLazyMode mode = [] {
        JitLazyMode parsed;
        // Old preview flag remains accepted for existing users.
        const char* value = std::getenv("XYO_JIT_LAZY_MODE");
        if (!value) value = std::getenv("XYO_JIT_LAZY_PROCEDURES");
        if (value && parseJitLazyMode(value, &parsed)) return parsed;
        return JitLazyMode::Auto;
    }();
};
inline JitLazyState& jitLazyState() {
    static JitLazyState state;
    return state;
}
} // namespace detail

inline JitLazyMode jitLazyMode() { return detail::jitLazyState().mode; }
inline void setJitLazyMode(JitLazyMode mode) {
    detail::jitLazyState().mode = mode;
}

/// "off" | "interp" | "hotspot" | "eager" を解析する。
inline bool parseJitMode(const char* s, JitMode* out) {
    if (!s || !out) return false;
    if (!std::strcmp(s, "off") || !std::strcmp(s, "interp") ||
        !std::strcmp(s, "disable")) {
        *out = JitMode::Off;
        return true;
    }
    if (!std::strcmp(s, "hotspot") || !std::strcmp(s, "hot")) {
        *out = JitMode::HotSpot;
        return true;
    }
    if (!std::strcmp(s, "eager")) {
        *out = JitMode::Eager;
        return true;
    }
    return false;
}

namespace detail {
struct JitModeState {
    JitMode mode = JitMode::HotSpot;
    bool explicitSet = false;
};
/// プロセス全体の実行モード。環境変数は初回参照時に一度だけ読む。
inline JitModeState& jitModeState() {
    static JitModeState st = [] {
        JitModeState s;
        if (std::getenv("XYO_JIT_DISABLE")) {
            s.mode = JitMode::Off;
            s.explicitSet = true;
            return s;
        }
        if (const char* e = std::getenv("XYO_JIT_MODE")) {
            JitMode m;
            if (parseJitMode(e, &m)) {
                s.mode = m;
                s.explicitSet = true;
            } else {
                std::fprintf(stderr,
                             "xyo-jit: unknown XYO_JIT_MODE '%s' "
                             "(hotspot|eager|off)\n", e);
            }
        }
        return s;
    }();
    return st;
}
} // namespace detail

inline JitMode jitMode() { return detail::jitModeState().mode; }
inline void setJitMode(JitMode m) {
    detail::jitModeState().mode = m;
    detail::jitModeState().explicitSet = true;
}
inline bool jitModeExplicit() {
    return detail::jitModeState().explicitSet;
}
inline bool jitEnabled() { return jitMode() != JitMode::Off; }

// Incremental fiber removal: in this opt-in mode execute compiled,
// non-suspending native scripts directly, retain existing stackless numeric
// and WASM continuations, and run unsupported suspending scripts with the
// interpreter until LLVM continuations can replace their native stacks.
// This is a correctness baseline, NOT a performance optimization.
inline bool jitFiberlessEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("XYO_JIT_FIBERLESS");
        return value && value[0] == '1';
    }();
    return enabled;
}


// --- JIT ABI ------------------------------------------------------------

using ScriptFn = void (*)(struct JitCtx*);

/// JIT コードと C++ ランタイムの橋渡しコンテキスト。
/// レイアウトは生成コードから直接参照される（ABI 契約）。
struct JitCtx {
    Thread* thread;
    Target* target;
    Runtime* runtime;
    Sequencer* sequencer;
    Fiber* fiber;

    /// 手続き引数（procedures_call 呼び出し時に差し替え・復帰）
    const Value* args;
    uint64_t numArgs;

    /// warp バースト開始時刻（isStuck 判定基準、rt.currentMSecs 基準）
    double warpStartMs;
    /// 現在の warp 実行中フラグ（control_all_at_once / warp 手続き）
    int32_t isWarp;
    /// "stop this script" 検知 — 手続き呼び出し元へ伝播して早期 return
    int32_t stopped;

    /// compat 入力評価・ブランチの出力スロット
    Value reportOut;
    int32_t compatBranch;
    int32_t compatBranchIsLoop;

    /// compat 層が使う合成 ExecContext（呼出し中のみ有効）
    ExecContext* compatEc;

    /// 変数スロット配列 — xyo_varslot_init がエントリで1度だけ
    /// 解決を埋める（Thread::jitVarSlots の data() を指す）。
    /// 生成コードは `varSlotData[idx]` を直接 load する。
    Variable** varSlotData;

    /// warp ループの isStuck 判定を間引くカウンタ
    /// （生成コードが (++n & 63)==0 のときだけ xyo_loop_yield を呼ぶ）。
    uint64_t yieldCheckN;

    /// broadcast and wait が待機中に保持している startedThreads（待機中のみ
    /// 非 null）。完了スレッドの回収（Runtime::reclaimThreads）が生ポインタ
    /// 参照を辿るために公開する — 生成コードは触らない。
    std::vector<Thread*>* joinRefs;

    /// Monotonic time sampled asynchronously by the Sequencer watchdog.
    /// Null outside stepThreads() or when using a custom clock; those paths
    /// retain the existing synchronous xyo_warp_yield_due check.
    std::atomic<int64_t>* sampledWarpClockUs;
    std::atomic<int64_t>* sampledWarpDeadlineUs;
    // Non-null only while the Sequencer watchdog is armed.
    // Unix milliseconds sampled every ~1ms; use the direct clock otherwise.
    std::atomic<int64_t>* sampledUnixMs;

    // Interpreter -> JIT on-stack replacement. Only used by a dedicated
    // continuation entry; never interpreted as a fresh loop's TIMES input.
    double osrRepeatRemaining = 0.0;

    /// Shared total and non-atomic per-fiber accumulation. The generated
    /// native code only updates pendingBlockCount. A context is owned by one
    /// executing fiber at a time; yield and retirement publish pending counts.
    /// Appended so older JitCtx ABI offsets remain unchanged.
    std::atomic<uint64_t>* blockCounter = nullptr;
    uint64_t pendingBlockCount = 0;
    // Appended field: existing runtime/AOT JitCtx offsets are unchanged.
    // Null outside a sampled native tick or with a custom clock.
    std::atomic<uint64_t>* sampledUnixDaysBits = nullptr;
    // Safe after an actual native Fiber yield: all promoted variables
    // have been spilled and the previous counted-loop body has committed.
    // This is NOT an arbitrary LLVM SSA/effect-PC deoptimizer.
    int32_t nestedOsrDeoptReady = 0;
    double nestedOsrDeoptRemaining = 0.0;
    // OSR-011: the MOST RECENT scheduler yield's source phase.
    // 1: completed selected outer-loop body; 2: completed one inner
    // loop body while the enclosing outer body is not yet complete.
    // Zero/invalid means no proven executable continuation.
    int32_t nestedOsrDeoptPhase = 0;
    double nestedOsrDeoptInnerRemaining = 0.0;
    // OSR-013 phase 3: selected outer -> middle -> leaf source loop.
    // A deep native yield has completed one LEAF body, but neither
    // enclosing iteration body has reached its source latch.
    double nestedOsrDeoptDeepRemaining = 0.0;
    // OSR-014 phase 4: four lexical counted repeats, last body
    // completed; all enclosing counted-loop bodies are unfinished.
    double nestedOsrDeoptFourthRemaining = 0.0;
    // LAYQ (src/jit/layq.hpp): per-thread warp-poll quantum, always a
    // power of two (keeps the mask form `q - (n & (q-1))`). Initialized
    // to 64 at attach; the adaptive policy widens it when idle.
    // ABI: appended, older JitCtx offsets unchanged.
    uint64_t yieldQuantum = 64;
    // LAYQ: last Runtime::stopEpoch observed at a warp poll. A mismatch
    // hands off (spill + xyo_yield), bounding stop latency at one
    // quantum regardless of the deadline cadence.
    uint64_t observedStopEpoch = 0;
    // LAYQ v2: scheduler-published recommended quantum + stop generation.
    // Pointer indirection follows the sampledWarpClockUs precedent (avoids
    // offsetof into Runtime, which is not standard-layout): wired at
    // attach, read with relaxed atomic loads in JIT poll/entry blocks.
    // Defaults point at never-written fallbacks (floor quantum, no stop)
    // so hand-rolled test contexts without attach stay safe.
    std::atomic<uint32_t>* quantumHintPtr = &layqFallbackQuantum;
    std::atomic<uint64_t>* stopEpochPtr = &layqFallbackStopEpoch;
    // LAYQ diagnostic counters (XYO_YIELD_QSTATS=1 dumps them at release):
    // polls executed, polls that handed off, sum of quanta in force.
    // Own-thread only (written by generated poll blocks, read at release).
    uint64_t layqPolls = 0;
    uint64_t layqTaken = 0;
    uint64_t layqQSum = 0;
};

/// Publish an entire execution slice with one atomic RMW, including when a
/// stopped script will never be resumed. A second flush is idempotent.
inline void flushJitBlockCount(JitCtx* c) {
    if (!c || !c->blockCounter || !c->pendingBlockCount) return;
    const uint64_t pending = c->pendingBlockCount;
    c->pendingBlockCount = 0;
    c->blockCounter->fetch_add(pending, std::memory_order_relaxed);
}

static_assert(std::is_trivially_copyable_v<JitCtx>);

/// fiber switch（rt.cpp）。ヘルパ実装（rt_blocks.cpp 等）からも呼ぶ。
extern "C" void xyo_yield(JitCtx* c);

/// id/name → Variable* 解決を Thread::jitVarCache にキャッシュする
/// 共有ヘルパ（rt.cpp 実装、rt_blocks.cpp 等から使用）。
/// list=true で lookupOrCreateList、false で lookupOrCreateVariable。
extern "C" Variable* xyo_jitVar(JitCtx* c, const char* id,
                                const char16_t* name, int list);

/// 変数スロット配列 [idx] に解決を書き込み、ctx->varSlotData を更新する
/// （生成コードのエントリブロックからのみ呼ばれる）。
extern "C" void xyo_varslot_init(JitCtx* c, uint64_t idx, const char* id,
                                 const char16_t* name, int32_t isList);

/// RT-001 opt-in native binding verification. Resolve against the live
/// Target; optional FAST mode borrows the cache only while the complete
/// local/stage binding stamp matches. A stale pointer is never read.
/// The JIT must call this at EACH use and never hoist the return.
extern "C" Variable* xyo_varslot_checked(JitCtx* c, uint64_t idx,
                                         const char* id,
                                         const char16_t* name,
                                         int32_t isList);
// Serial-only, side-effecting owner verification for opt-in LLVM
// inline generation guard. Returns null for stage targets.
extern "C" const Target* xyo_varslot_guard_stage(JitCtx* c);

/// コンパイル済みスクリプト（target->blocks 共有 — クローン間で共有可）
struct CompiledScript {
    ScriptFn entry = nullptr;
    // Non-native browser Wasm callable; dispatched separately by Sequencer.
    std::shared_ptr<wasm::HotCode> wasm;
    ScriptFn osrEntry = nullptr;
    std::string osrLoopId;  // top-level repeat, immutable after publication
    /// True when this script's procedures are compiled on first use.
    bool lazy = false;
    /// variantKey(proccode+warp) → 手続き関数
    std::unordered_map<std::string, ScriptFn> procedures;
    /// 生成コード内に yield 点が存在するか。false なら fiber なしで同期実行可。
    bool mayYield = true;
    // A bounded, synchronous script executes faster in scheduler order than
    // through a dependency graph and worker dispatch. Calculated once from
    // the script's structural scan, never from benchmark names or input.
    bool shortSync = false;
    /// Compat/reporter fallbacks or AOT-owned GPU programs require the caller
    /// thread even when opcode-level dependency analysis looks compute-only.
    bool mainThread = false;
    // v3 only: all cached slots used by this script and its procedures.
    // Descriptor lifetime belongs to JitEngine::Impl.
    std::vector<AotLazyCachedSlot*> aotLazyWorkerSlots;
    /// GPU 並列ループプログラム（生成コードがポインタで参照するため
    /// モジュールと同寿命でここが所有）。生成コードが実際に参照する
    /// のは `ParallelLoop::program`。
    std::vector<std::unique_ptr<gpu::ParallelLoop>> gpuProgs;
    // Append-only: existing fields retain their ABI/layout offsets.
    // Opt-in bounded native loop region inside a live warp procedure.
    ScriptFn nestedOsrEntry = nullptr;
    std::string nestedOsrLoopId;
    std::string nestedOsrCallId;
    // Empty for the original 2-frame nested OSR; outer warp call ID for
    // the opt-in 3-frame case. Metadata only, no native frame ABI change.
    std::string nestedOsrParentCallId;
    // Immutable outermost-to-innermost Scratch source callsites.
    std::vector<std::string> nestedOsrCallPath;
    // Reject previously compiled native continuation after source edits:
    // matching block IDs alone do not prove identical reporter semantics.
    uint64_t nestedOsrSourceRevision = 0;
    // Full lexical control stack, excluding final counted repeat.
    // arm==0 is a procedure call, arm 1/2 an IF/IF-ELSE arm.
    std::vector<std::string> nestedOsrFramePath;
    std::vector<uint8_t> nestedOsrBranchArms;
    uint32_t nestedOsrArgFrameIndex = 0;
    // Optional readonly IR-001 provenance dictionary for an exact source
    // CallFrame/loop-PC projection. This never supplies executable deopt
    // machine state or replaces the interpreter's live GC roots.
    bool nestedOsrSourceProofRequired = false;
    std::shared_ptr<const ir::SourceFrameSymbols> nestedOsrSourceSymbols;
    // Requires source proof. Only a completed counted-loop body may be
    // handed back after a native Fiber scheduler suspension.
    bool nestedOsrInFlightGuard = false;
    // An explicitly proven single nested repeat may itself yield. The
    // source ID is frozen at compile time, not taken from mutated Blocks.
    bool nestedOsrInnerYieldGuard = false;
    std::string nestedOsrInnerLoopId;
    // Nonempty only for proven two nested loops under selected repeat.
    std::string nestedOsrDeepInnerLoopId;
    std::string nestedOsrFourthLoopId;
    // IR-001 opt-in, immutable compiled reporter->SSA provenance. Owned
    // by JitEngine alongside this compiled script, never by Thread.
    // Append-only to preserve older CompiledScript field offsets.
    // No raw LLVM pointers/GC values or executable restore permission.
    std::shared_ptr<const ir::SourceReporterRuntimeSites> nativeReporterSites;
    // START-015: append-only host-side stamp, without moving existing
    // CompiledScript fields or changing generated JitCtx ABI offsets.
    // A nonzero sourceRevision requires exact owner and revision equality.
    Blocks* aotSourceBlocks = nullptr;
    uint64_t aotSourceRevision = 0;
};

class JitEngine {
public:
    JitEngine();
    ~JitEngine();

    JitEngine(const JitEngine&) = delete;
    JitEngine& operator=(const JitEngine&) = delete;

    /// thread の topBlock 以下をコンパイル。成功すれば CompiledScript を返す。
    /// キャッシュはトップ Block* 単位（Blocks 共有 — クローン間でも共有可）。
    /// topHint: getBlock ルックアップ済みのトップブロック（任意）。
    const CompiledScript* compile(Thread& thread, Runtime& runtime,
                                  Sequencer& sequencer,
                                  const Block* topHint = nullptr);
    // Opt-in path for the validated, resumable numeric CFG. Called only
    // after decideCompile, before falling back to legacy code generation.
    bool tryAttachNumeric(Thread& thread, const Block* top);
    // Publish worker results on the runtime thread, never from the worker.
    void pollAsync();
    // Experimental: precompile a small number of expensive broadcast hats
    // before any user script starts. Only queues work, never executes blocks.
    void prefetchWarpBroadcasts(Runtime& runtime);
    // Only for an interpreter thread that has executed no Scratch block yet.
    // Native AOT v3 requires a *current* source Blocks owner. If omitted,
    // only ordinary (non-AOT) JIT code can be returned.
    const CompiledScript* readyScript(
        const Block* top, const Blocks* source = nullptr) const;
    // Read-only scheduler query; compiling_ belongs to the main thread.
    bool isCompiling(const Block* top) const {
        return top && compiling_.contains(top);
    }

    /// HotSpot のコンパイル判定。spawn ごとに 1 回呼ぶ。プロファイル
    /// （呼び出し回数・解釈実行ブロック数）を更新し、今回のスレッドを
    /// JIT で走らせるなら true を返す。
    bool decideCompile(Thread& thread, const Block* top);

    /// インタプリタが実行したブロック数を記録する（HotSpot の
    /// ホットネス判定用）。top はスクリプトのトップ Block*。
    void noteInterpBlocks(const void* top, uint64_t blocks);

    /// 指定スクリプトのプロファイル（診断用 — XYO_JIT_VERBOSE 等）。
    struct ScriptStats {
        uint64_t invocations = 0;
        uint64_t interpBlocks = 0;
        bool compiled = false;
        bool lazy = false;
        bool aot = false;
        bool hasLoop = false;
    };
    bool scriptStats(const void* top, ScriptStats* out) const;

    /// スレッドの依存解析結果 — (topBlock, target) 単位でキャッシュ。
    /// 結果は常に非 null の AccessSet*（解析不能要素は unknown フラグで
    /// 表現）。並列実行可否は workerSafe() と GPU 呼出しの有無で判定。
    /// yield する fiber も、GC を遅延したバッチ内では並列実行できる。
    const AccessSet* accessFor(const Thread& thread, Runtime& runtime);
    // Called on the scheduler thread, before native worker dispatch.
    // Never build Blocks caches on a worker. An unresolved source is unsafe.
    bool prepareAotWorker(const CompiledScript& script) const;

    /// AOT オブジェクト（`xyo aot` が生成）をロードし、slot に実
    /// アドレスを束縛してスクリプトを通常のコンパイルキャッシュへ
    /// 登録する。projectHash は sb3 の FNV-1a（不一致なら拒否）。
    bool loadAot(Runtime& runtime, const void* object, size_t size,
                 uint64_t projectHash, std::string* why = nullptr,
                 const aot::ProgressCallback& progress = {});

    /// コンパイル済みスクリプト/依存解析の全キャッシュを破棄する。
    /// 実行中スレッドが無いこと（CompiledScript/BlockCached 参照を
    /// 破棄するため）を呼び出し側が保証する。
    void invalidate();
    /// accessFor の結果キャッシュの世代（invalidate で進む）。
    uint32_t accessGeneration() const { return accessGen_; }

    bool ok() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    /// トップ Block* ごとのコンパイル結果。ティアごとに保持し、実行中
    /// スレッドが参照中の旧ティアのコードを破棄しない（JIT コードは
    /// エンジンと同寿命）。
    struct ScriptEntry {
        std::unique_ptr<CompiledScript> code;
        bool aot = false;
    };
    /// HotSpot のホットネス（トップ Block* 単位）。
    struct ScriptProfile {
        std::string id;                 // 診断表示用（トップブロック id）
        uint64_t invocations = 0;
        uint64_t interpBlocks = 0;
        /// 0=未走査 1=ループ無し 2=ループ有り（スクリプト内に
        /// control_forever/repeat/while/for_each が到達可能か）
        uint8_t hasLoop = 0;
        /// 到達可能ブロック数の目安（走査時の上限で飽和）。
        /// 巨大な直線スクリプトを解釈のまま放置しないための判定に使う。
        uint32_t nodeCount = 0;
    };
    /// トップ Block* → コンパイル結果（Block は Blocks 所有でポインタ安定）
    std::unordered_map<const void*, ScriptEntry> cache_;
    // START-015: active native Fibers may still borrow CompiledScript*
    // after invalidate(). Retain v3 code objects until engine shutdown;
    // invalidate their source stamps so no old code is dispatched again.
    std::vector<std::unique_ptr<CompiledScript>> retiredAot_;
    std::unordered_map<const void*, std::shared_ptr<NumericJitCode>>
        numericCache_;
    std::unordered_set<const void*> numericRejected_;
    std::unordered_map<const void*, ScriptProfile> profiles_;
    /// スクリプトの静的走査（初回のみ）。戻り値は hasLoop（0/1/2）、
    /// *nodes に到達可能ブロック数の目安を返す。
    uint8_t scanScriptLoop(Blocks& blocks, const Block* top,
                           uint32_t* nodes);
    /// コンパイル失敗/対象外の topBlock（再試行しない — spawn 毎に
    /// compile が呼ばれるためヒット経路を安く保つ）
    std::unordered_set<const void*> failed_;
    std::unordered_set<const void*> compiling_;
    uint64_t asyncGeneration_ = 1;
    /// 依存解析キャッシュ: topBlock → (target → AccessSet)。
    /// 変数解決がターゲット依存（自ターゲット→ステージ）のため
    /// target ごとに再計算する。
    std::unordered_map<const void*,
                       std::unordered_map<const Target*, AccessSet>>
        accessCache_;
    uint32_t accessGen_ = 1;
    uint64_t scriptCounter_ = 0;
};

} // namespace xyo
