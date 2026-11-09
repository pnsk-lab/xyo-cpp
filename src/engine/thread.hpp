// xyo-cpp: engine/thread.js の移植 — Scratch スレッド + _StackFrame。
// status の数値は ABI 契約（JIT 生成コードが直接参照）。
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/value.hpp"
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
    /// 一意・安定）。Variable* は std::map ノードで安定、target は
    /// スレッド不変なので target 単位の解決として正しい。
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
