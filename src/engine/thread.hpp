// xyo-cpp: engine/thread.js の移植 — Scratch スレッド + _StackFrame。
// status の数値は ABI 契約（JIT 生成コードが直接参照）。
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/value.hpp"
#include "../ir/source_typed_ssa.hpp"
#include "../ir/gc_value_handles.hpp"
#include "../model/blocks.hpp"
#include "../model/variable.hpp"
#include "timer.hpp"

namespace xyo {

struct Target;
struct BlockCached;
struct Blocks;
struct Fiber;
struct JitCtx;
namespace ir { class NativeProcedureFrameSession; }
namespace ir {
struct SourceListReadEvents;
struct SourceListReadCheckpointAudit;
struct SourceScalarReadEvents;
struct SourceScalarPrefixAudit;
struct SourceReportedPrefixPlan;
}

struct Runtime;
namespace ir { struct NumericThreadSession; }
class Thread;
using ScriptFn = void (*)(JitCtx*);

/// JIT の変数解決キャッシュキー。Scratch は (id → name) の順で解決するため、
/// 同じ id でも name が異なれば別の Variable* になり得る。
/// ポインタ同一性で比較する（codegen が id/name をインタン化する）。
struct JitVarCacheKey {
    const char* id = nullptr;
    const char16_t* name = nullptr;
    int list = 0;
    bool operator==(const JitVarCacheKey& o) const {
        return id == o.id && name == o.name && list == o.list;
    }
};
struct JitVarCacheKeyHash {
    size_t operator()(const JitVarCacheKey& k) const {
        size_t h = std::hash<const void*>{}(k.id);
        h ^= std::hash<const void*>{}(k.name) + 0x9e3779b97f4a7c15ull +
             (h << 6) + (h >> 2);
        h ^= size_t(k.list) * 0x94d049bb133111ebull;
        return h;
    }
};

// Thread.STATUS_*（値は固定）
inline constexpr int STATUS_RUNNING = 0;
inline constexpr int STATUS_PROMISE_WAIT = 1;
inline constexpr int STATUS_YIELD = 2;
inline constexpr int STATUS_YIELD_TICK = 3;
inline constexpr int STATUS_DONE = 4;

/// _ops 中断時に保存される報告済み入力値（{opCached: id, inputValue}）。
struct ReportedEntry {
    std::string opCachedId;
    Value inputValue;
};

/// stackFrame.executionContext — ブロック内部状態（JS では自由形オブジェクト）。
/// StackFrame 内の unique_ptr に完全型が必要なため thread.hpp に置く。
struct ExecContext {
    std::unordered_map<std::string, Value> vals;
    /// control_wait / looks_sayforsecs / motion_glide 等のスタックタイマ
    std::unique_ptr<Timer> timer;
    double duration = 0;
    /// event_broadcastandwait
    Variable* broadcastVar = nullptr;
    std::vector<Thread*> startedThreads;
    bool hasStartedThreads = false;
    /// procedures_call
    bool executed = false;
    Value returnValue = Value::undefined();
    /// control_repeat / control_for_each — JS `typeof x === 'undefined'` 判定用
    bool hasLoopCounter = false;
    double loopCounter = 0;
    bool hasIndex = false;
    double index = 0;
    /// motion_glidesecstoxy
    double startX = 0, startY = 0, endX = 0, endY = 0;

    Value& operator[](const std::string& key) { return vals[key]; }
    bool has(const std::string& key) const { return vals.count(key) != 0; }
};

/// thread.js _StackFrame。積層ごとの実行メタデータ。
struct StackFrame {
    bool isLoop = false;
    bool warpMode = false;
    std::string reporting;                        // 実行中リポータ op id
    bool hasReported = false;                     // reported が有効か（JS null 区別）
    std::vector<ReportedEntry> reported;          // 中断時の凍結入力値
    bool waitingReporter = false;
    bool suspendLast = false;                     // 中断 op が _ops 最後か（async 再開用）
    bool hasParams = false;                       // params 非 null
    std::unordered_map<std::u16string, Value> params;
    std::unique_ptr<ExecContext> executionContext;
    const BlockCached* op = nullptr;              // 実行中の BlockCached
    const BlockCached* currentBlock = nullptr;    // このフレームの命令ブロック

    void reset(bool keepWarp = false) {
        bool w = warpMode;
        isLoop = false;
        warpMode = keepWarp && w;
        reporting.clear();
        hasReported = false;
        reported.clear();
        waitingReporter = false;
        suspendLast = false;
        hasParams = false;
        params.clear();
        executionContext.reset();
        op = nullptr;
        currentBlock = nullptr;
    }
};

/// Thread — 実行中スタックコンテキスト。
struct AccessSet;

// Optional per-Thread cache of nested-WASM scalar variable bindings.
// The runtime supplies structural generation stamps for local/stage
// VariableMap changes (including clone/editor replacement). This is
// allocated only by the explicit nested WASM experiment.
struct NestedWasmBindingCache {
    uint64_t codeIdentity = 0;
    const Target* target = nullptr;
    const Target* stage = nullptr;
    uint64_t localGeneration = 0;
    uint64_t stageGeneration = 0;
    std::vector<Variable*> variables;
};

// RT-001: a detached, strictly diagnostic copy of representable LLVM
// integer and floating SSA results plus raw integer/float arguments.
// Descriptors carry pre-optimization instruction IDs and tagged widths.
// i64 may contain opaque/GC-bearing bits: these are NOT GC roots and
// must NEVER be interpreted as object addresses or executable state.
struct NativeRtSsaCapture {
    uint64_t sourceRevision = 0;
    uint64_t digestLo = 0, digestHi = 0;
    uint64_t sourcePc = 0, procedureId = 0, siteId = 0;
    uint64_t fingerprint = 0;
    uint32_t unsupported = 0;
    uint32_t unresolvedPointers = 0, unresolvedOther = 0;
    // Counts-only LLVM pointer origins, copied from compiler-provided
    // metadata. Never raw native addresses, never a GC root.
    std::array<uint32_t, 7> pointerOrigins{};
    // Counts only: LLVM pointer values ultimately anchored to exactly
    // one native AllocaInst and distinct native stack allocation roots.
    // These pointers are INVALID after Fiber destruction and are not
    // GC roots or portable Scratch interpreter values.
    uint32_t stackDerivedPointers = 0;
    uint32_t distinctStackAllocas = 0;
    // 'numericComplete' means all compiler-live numeric/floating
    // instructions and numeric arguments were represented. It still
    // does NOT prove GC-root or effect replay safety.
    bool numericComplete = false;
    std::vector<uint64_t> descriptors;
    std::vector<uint64_t> words;
    // RT-001: original compiler-created exact i64 argument ancestry
    // per descriptor; zero = no proof. Keep the compiler copy separate
    // from the diagnostic copy to catch rewritten provenance even when
    // the local integrity fingerprint is recomputed. These are ONLY
    // source arg indices, never GC addresses or handles.
    std::vector<uint32_t> compilerAliasArguments;
    std::vector<uint32_t> aliasArguments;
    // A DIFFERENT compiler proof: the i64 slot comes from a trusted
    // boxed Scratch string producer in this native function (including
    // PHI/SELECT only when every branch has such a producer).
    // Bit 1 is the sole supported value; zero means unproven.
    // These masks never encode GC addresses or assume a tag is valid.
    std::vector<uint32_t> compilerGeneratedStrings;
    std::vector<uint32_t> generatedStrings;
    // Each live heap SStr at a certified slot is retained using its own
    // exact GC handle. The snapshot owns this provider across native
    // Fiber retirement, and drops it on the next checkpoint or Thread
    // reset. This is explicitly OPT-IN and not an executable SSA state.
    // Shared root-provider LIFETIME preserves NativeRtSsaCapture's
    // existing copyable diagnostic semantics: copying a frozen SSA
    // snapshot must not silently drop its GC roots or invalidate
    // another copy's handles. Handles themselves remain thread-confined.
    std::shared_ptr<ir::GcValueHandles> generatedStringRoots;
    std::vector<ir::RootedValueHandle> generatedStringHandles;
    // Compiler-origin expectation copied separately from LLVM constant
    // descriptors. It is NEVER derived from mutable live SSA words
    // or mutable diagnostic descriptor storage. Unlike an extra
    // Thread member, this preserves Thread's original ABI size.
    std::optional<ir::SourceTypedSsaManifest> compilerManifest;
    // IR-001: origin-owner STRUCTURAL generation at the native capture,
    // separate from mutable SSA bits and the compiled reporter dictionary.
    // No Variable*/list pointer or GC Value is kept. It only permits a
    // comparison with an independently re-resolved owner on adoption.
    bool reporterOwnerReadback = false;
    const Target* reporterOwnerTarget = nullptr;
    const Target* reporterOwnerStage = nullptr;
    uint64_t reporterOwnerLocalGeneration = 0;
    uint64_t reporterOwnerStageGeneration = 0;
    // IR-001: individually checked GC roots for ACTUAL source reporter
    // heap-string SSA words retained at the native checkpoint. Separate
    // from compiler-known generated strings and source CallFrame args.
    // These roots protect the historical value, not its interpreter slot.
    std::shared_ptr<ir::GcValueHandles> reporterInputRoots;
    std::vector<ir::RootedValueHandle> reporterInputHandles;
    // IR-001: independent, source-evaluation-time INDEX/result evidence.
    // Root provider is owned by the snapshot; no raw GC pointers in events.
    std::shared_ptr<const ir::SourceListReadCheckpointAudit>
        reporterListReadEvidence;
    // Actual historical scalar reporter reads, INCLUDING values whose
    // LLVM SSA is dead before the physical checkpoint. No StackFrame write.
    std::shared_ptr<const ir::SourceScalarPrefixAudit>
        reporterScalarPrefixEvidence;
    // Read-only, GC-handle-backed cached interpreter reported-prefix
    // candidates; no StackFrame/ArgValues mutation or executable restore.
    std::shared_ptr<const ir::SourceReportedPrefixPlan>
        reporterPrefixPlan;

    uint64_t computeFingerprint() const noexcept {
        uint64_t hash = 14695981039346656037ull;
        auto mix = [&](uint64_t word) {
            for (unsigned i = 0; i < 8; ++i) {
                hash ^= (word >> (i * 8)) & 0xffu;
                hash *= 1099511628211ull;
            }
        };
        mix(sourceRevision);
        mix(digestLo);
        mix(digestHi);
        mix(sourcePc);
        mix(procedureId);
        mix(siteId);
        mix(unsupported);
        mix(unresolvedPointers);
        mix(unresolvedOther);
        for (uint32_t originCount : pointerOrigins) mix(originCount);
        mix(stackDerivedPointers);
        mix(distinctStackAllocas);
        mix(numericComplete ? 1 : 0);
        mix(descriptors.size());
        for (uint64_t id : descriptors) mix(id);
        mix(words.size());
        for (uint64_t word : words) mix(word);
        mix(aliasArguments.size());
        for (uint32_t argument : aliasArguments) mix(argument);
        mix(generatedStrings.size());
        for (uint32_t producer : generatedStrings) mix(producer);
        // Root handle tokens are not SSA words and are intentionally
        // excluded from the raw diagnostic fingerprint. Their exact
        // resolved Value and lifetime are checked independently.
        return hash;
    }
};

class Thread {
public:
    explicit Thread(std::string firstBlock);
    ~Thread();

    std::string topBlock;
    std::vector<std::string> stack;              // "" = JS null
    /// unique_ptr 保持で StackFrame*/ExecContext& の参照を常に安定させる
    /// （pushStack/vector 再配置でダングルしない）。pop したフレームは
    /// trashFrames に移し、JS GC 相当の「ポップ済みでも読める」挙動を維持。
    // Only back/index access is needed. Empty vectors allocate nothing, unlike
    // deque's map and initial block. Moving unique_ptrs keeps frame addresses
    // stable across growth and transfer into trashFrames.
    std::vector<std::unique_ptr<StackFrame>> stackFrames;
    std::vector<std::unique_ptr<StackFrame>> trashFrames;
    /// JIT proc フレームの再利用プール — xyo_proc_pop が reset 済みの
    /// フレームを貯め、pushStack が make_unique の代わりに再利用する。
    /// 内容は fresh StackFrame と等価（pop 時に reset 済み・GC 参照解放済み）。
    std::vector<std::unique_ptr<StackFrame>> framePool;
    int status = STATUS_RUNNING;
    bool isKilled = false;
    // The wait-until CONDITION can be skipped in unchanged scheduler passes.
    // The site is opaque: never dereference it (AOT embeds build-time IDs).
    bool eventWaitArmed = false;
    uintptr_t eventWaitSite = 0;
    uint64_t eventWaitEpoch = 0;
    // Distinguish "condition stayed false" from "left this wait and
    // returned to the very same wait site" within one scheduler slice.
    uint64_t eventWaitExitCount = 0;
    // Optional pure wait-until single-variable dependency snapshot. Only
    // read eventWatchVariable after source and binding generations match.
    bool eventWatchValid = false;
    const Variable* eventWatchVariable = nullptr;
    uint64_t eventWatchBits = 0;
    uint64_t eventWatchSourceRevision = 0;
    const Target* eventWatchStage = nullptr;
    uint64_t eventWatchLocalGeneration = 0;
    uint64_t eventWatchStageGeneration = 0;
    // Validated against Runtime::threads before use; public vector edits may
    // invalidate this hint, in which case membership falls back to a scan.
    mutable size_t runtimeIndex = SIZE_MAX;
    Target* target = nullptr;
    Blocks* blockContainer = nullptr;
    bool stackClick = false;
    bool updateMonitor = false;
    bool requestScriptGlowInFrame = false;
    std::string blockGlowInFrame;
    bool hasWarpTimer = false;
    Timer warpTimer;
    Value justReported = Value::undefined();

    // --- JIT 領域（isCompiled のときのみ意味を持つ）---
    /// トップ Block*（HotSpot のプロファイルキー / インタプリタ
    /// ブロック数の記録先）。tryCompile で解決する。
    const Block* topBlockPtr = nullptr;
    bool triedToCompile = false;
    // Set before the first interpreted Scratch block is evaluated. OSR
    // switches a started thread only at a verified loop-head checkpoint.
    bool interpreterStarted = false;
    // Optional cooperative async JIT warmup deadline. Zero means that the
    // thread has not yet been deferred. This is reset on Thread recycling.
    double asyncJitWarmupDeadlineMs = 0.0;
    bool osrActive = false;
    double osrRemaining = 0.0;
    // Browser WebAssembly single-top-level repeat continuation. 0: prefix,
    // 1: body, 2: suffix. Resets with Thread reconstruction and retirement.
    uint8_t wasmLoopPhase = 0;
    uint32_t wasmLoopIndex = 0;
    double wasmLoopRemaining = 0.0;
    // Index of the next compiled/interpreted region in the hybrid script.
    // It survives scheduler yields but resets with recycled Scratch threads.
    uint32_t wasmRegionIndex = 0;
    // Scratch-value staging area reused across WASM loop invocations to
    // avoid two heap allocations per scheduler checkpoint.
    std::vector<uint64_t> wasmSlotBits;
    // Optional, explicitly attached shared-IR continuation. This is not
    // enabled by default or inferred from normal Scratch JIT compilation.
    std::unique_ptr<ir::NumericThreadSession> numericSession;
    // Opt-in LLVM native procedure-frame mirror. Independent of normal
    // Fiber execution, survives scheduler yields, never owns GC pointers.
    std::unique_ptr<ir::NativeProcedureFrameSession> nativeProcedureFrames;
    bool isCompiled = false;
    // One-shot permission to attempt a nested WASM dispatch from the next
    // interpreter execute(). It is consumed before evaluating any primitive,
    // preventing reentrant callbacks from silently crossing checkpoints.
    bool nestedWasmEligible = false;
    std::unique_ptr<NestedWasmBindingCache> nestedWasmBindings;
    // True only for a JIT non-waiting broadcast receiver. Such a Thread*
    // is never handed back to a broadcast-and-wait caller as a join handle.
    bool reusableBroadcastReceiver = false;
    bool executableHat = false;
    const void* compiled = nullptr;   // const CompiledScript*（JitEngine 所有）
    JitCtx* jitCtx = nullptr;         // 所有（~Thread で解放）
    Fiber* fiber = nullptr;           // 所有（~Thread で解放）
    ExecContext compatibilityStackFrame;         // compiled script の compat 呼び出し用
    bool hasCompatStackFrame = false;
    /// JIT rt 関数の変数/リスト解決キャッシュ。キーは生成モジュールに
    /// pin された id 文字列ポインタ（モジュールは JIT 生存中破棄されず
    /// 一意・安定）。Variable* は VariableMap の要素を借用する。
    /// rehash では安定だが erase/recreate や stage shadow では無効。
    /// RT の opt-in 保護モードは構造世代と所有者を照合して再結合する。
    std::unordered_map<JitVarCacheKey, Variable*, JitVarCacheKeyHash> jitVarCache;
    /// 変数スロット配列 — idx → Variable*（JitCtx::varSlotData が
    /// data() を指す）。スクリプトエントリで xyo_varslot_init が埋める。
    std::vector<Variable*> jitVarSlots;
    mutable std::string idCache_;   // getId() 遅延キャッシュ
    /// parallelAccessFor の結果（JitEngine::accessCache_ は (top, target)
    /// 単位で不変）。パスごとに unordered_map を 2 段引くのを避ける。
    /// accessGen が JitEngine の世代と違えば無効（invalidate で進む）。
    const AccessSet* accessCache = nullptr;
    bool accessResolved = false;
    uint32_t accessGen = 0;
    // Append-only: all previously emitted JIT field offsets are unchanged.
    bool nestedOsrActive = false;
    // Optional mark-epoch for Thread reclamation graph closure. This is
    // NOT a GC mark, native ABI field, or script-visible state.
    uint64_t reclaimRetentionEpoch = 0;
    // Only allocated for native pure wait-until predicates with 2-4
    // independent scalar dependencies. The common one-variable watcher
    // remains allocation-free; appended storage preserves JIT ABI offsets.
    struct AdditionalWaitDependencies {
        std::array<const Variable*, 3> variables{};
        std::array<uint64_t, 3> bits{};
        uint8_t count = 0;
    };
    std::unique_ptr<AdditionalWaitDependencies> eventWatchAdditional;
    // RT-001: actual post-Fiber root AfterLoopBody checkpoints, counted
    // independently for each Scratch Thread. IR-001 intentionally keeps
    // yields() as its historic *nonroot procedure* mirror statistic.
    // Appended after JIT-visible fields to preserve their ABI offsets.
    uint64_t nativeRootLoopYieldCount = 0;
    // Allocated only by the RT opt-in LLVM SSA detachment experiment.
    std::unique_ptr<NativeRtSsaCapture> nativeRtSsaCapture;
    // RT-001 soft GC budget: only a source-certified native loop
    // checkpoint may request retirement of a running LLVM Fiber.
    // On rejection the original Fiber remains live and GC stays
    // deferred; limit automatic retries to one per Scratch Thread.
    bool nativeGcSoftBudgetYieldDue = false;
    bool nativeGcSoftBudgetAttempted = false;
    // RT-001 optional generation-stamped native Variable* cache. Append
    // after all existing JIT-visible fields to keep JitCtx ABI untouched.
    // A cache hit may borrow Variable* ONLY after comparing both local
    // and stage generations and the current Target/stage identities.
    // A stamp mismatch invalidates the entire borrowed-pointer map
    // BEFORE any pointer is dereferenced (including ABA reuse).
    const Target* nativeVarCacheTarget = nullptr;
    const Target* nativeVarCacheStage = nullptr;
    uint64_t nativeVarCacheLocalGeneration = 0;
    uint64_t nativeVarCacheStageGeneration = 0;
    bool nativeVarCacheValid = false;
    uint64_t nativeVarCacheHits = 0;
    uint64_t nativeVarCacheMisses = 0;
    uint64_t nativeVarCacheInvalidations = 0;
    // RT-001 indexed-fast variant, separate from the regular hash cache.
    // Each slot is keyed by the exact codegen-interned (id, name, kind).
    // Only borrow after the full owner + local/stage generation stamp
    // matches. A stamp mismatch clears all entries before any dereference.
    struct NativeVarIndexedEntry {
        JitVarCacheKey key{};
        Variable* variable = nullptr;
    };
    std::vector<NativeVarIndexedEntry> nativeVarIndexedSlots;
    // RT-001 native LLVM serial-only inline guard mirror, updated by
    // xyo_varslot_checked on every vector mutation or epoch mismatch.
    // Never dereference these after Target/Stage owner checks fail.
    // The live identity and map generations are checked on EVERY use.
    const NativeVarIndexedEntry* nativeVarInlineEntries = nullptr;
    uint64_t nativeVarInlineCount = 0;
    uint64_t nativeVarInlineTraceHits = 0;
    const uint64_t* nativeVarLocalGenerationAddress = nullptr;
    const uint64_t* nativeVarStageGenerationAddress = nullptr;
    // Opt-in async LLVM reporter: roots for *compiler-proven* live
    // Variable::value SSA words. Owned by Thread, not the abandoned Fiber
    // C++ stack: a cancelled/retired Fiber may skip local destructors.
    // Never implies permission to collect with other SSA roots unresolved.
    std::vector<std::unique_ptr<ir::GcValueHandles>> nativeAsyncVariableRoots;
    // RT-001 diagnostic only: physical JIT call-return PC and the
    // rooted reporter helper's own frame address. These are sampled
    // BEFORE suspending; neither is a machine GC root or an unwind proof.
    // A cancelled Fiber skips C++ destructors; retirement must clear both.
    uintptr_t nativeAsyncReporterReturnPc = 0;
    uintptr_t nativeAsyncReporterFramePtr = 0;
    // Borrowed JIT alloca address for a paused-callsite diagnostics ONLY.
    // The Thread-owned GcValueHandles above contain the actual GC roots.
    // Never use this stack pointer as a heap root; clear on retirement.
    uintptr_t nativeAsyncReporterInputBuffer = 0;
    uint32_t nativeAsyncReporterInputWords = 0;
    // SCHED-002: an ordinary short native receiver queued but not yet entered.
    // Cleared at every execution/retirement boundary; never a compiled-code
    // cache or a replacement for Thread identity. Appended after ABI fields.
    bool pendingShortBroadcast = false;
    // IR-001: bounded list-read records, allocated only in audit mode.
    // APPEND-ONLY DATA: preserve pre-existing Thread member offsets
    // required by native JIT code and scheduler probes.
    std::unique_ptr<ir::SourceListReadEvents> nativeSourceListReads;
    // Append only: compiled code may embed Thread member offsets.
    std::unique_ptr<ir::SourceScalarReadEvents> nativeSourceScalarReads;
    // Common sequence for scalar/list reporter observations, reset only
    // on Thread completion; 0 and UINT64_MAX signal overflow.
    uint64_t nativeSourceReporterReadSequence = 1;
    static std::string getIdFromTargetAndBlock(const Target* target,
                                             const std::string& topBlock);
    /// target.id&topBlock — target セット後は不変なので遅延キャッシュ
    const std::string& getId() const;

    /// 完了したスレッドの重い状態を返す（frames / 実行コンテキスト /
    /// JIT キャッシュ）。Thread オブジェクト自体は broadcastandwait の
    /// startedThreads が生ポインタで保持するため解放できない（ABA）が、
    /// 中身を読む観測者は無い — status / stack / updateMonitor だけ。
    /// スケジューラが速くなってスレッド生成が増え、32 秒の実行で 100 万
    /// スレッド × 数 KB = RSS 3GB になっていた。
    void releaseFinished();

    void pushStack(const std::string& blockId);
    void reuseStackForNextBlock(const std::string& blockId);
    std::string popStack();
    // Reuse interpreter frames once execute() has finished reading popped ones.
    void recycleTrashFrames();
    /// 現在スクリプトを手続き呼び出し境界までポップ
    void stopThisScript();
    /// トップのブロック id（空なら ""）
    std::string peekStack() const;
    StackFrame* peekStackFrame();
    StackFrame* peekParentStackFrame();
    /// undefined → 報告なし（JS: undefined は null 化される）
    void pushReportedValue(Value v) {
        pendingShortBroadcast = false;
        justReported = v.isUndefined() ? Value::undefined() : v;
    }
    void initParams();
    void pushParam(const std::u16string& name, Value v);
    /// 直近の params 持ちフレームを探索（JS getParam 準拠）
    Value getParam(const std::u16string& name) const;
    const std::unordered_map<std::u16string, Value>* getAllParams() const;
    bool atStackTop() const { return peekStack() == topBlock; }
    void goToNextBlock();
    bool isRecursiveCall(const std::string& procedureCode) const;
    /// topHint: 解決済み topBlock — getBlock ルックアップ省略用
    void tryCompile(const Block* topHint = nullptr);
};

} // namespace xyo
