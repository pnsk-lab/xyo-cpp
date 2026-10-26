// xyo-cpp: engine/thread.js の移植 — Scratch スレッド + _StackFrame。
// status の数値は ABI 契約（JIT 生成コードが直接参照）。
#pragma once

#include <cstdint>
#include <deque>
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
struct Runtime;
class Thread;
using ScriptFn = void (*)(JitCtx*);

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
    }
};

/// Thread — 実行中スタックコンテキスト。
class Thread {
public:
    explicit Thread(std::string firstBlock);
    ~Thread();

    std::string topBlock;
    std::vector<std::string> stack;              // "" = JS null
    /// unique_ptr 保持で StackFrame*/ExecContext& の参照を常に安定させる
    /// （pushStack/vector 再配置でダングルしない）。pop したフレームは
    /// trashFrames に移し、JS GC 相当の「ポップ済みでも読める」挙動を維持。
    std::deque<std::unique_ptr<StackFrame>> stackFrames;
    std::deque<std::unique_ptr<StackFrame>> trashFrames;
    /// JIT proc フレームの再利用プール — xyo_proc_pop が reset 済みの
    /// フレームを貯め、pushStack が make_unique の代わりに再利用する。
    /// 内容は fresh StackFrame と等価（pop 時に reset 済み・GC 参照解放済み）。
    std::vector<std::unique_ptr<StackFrame>> framePool;
    int status = STATUS_RUNNING;
    bool isKilled = false;
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
    bool isCompiled = false;
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
    std::unordered_map<const char*, Variable*> jitVarCache;
    /// 変数スロット配列 — idx → Variable*（JitCtx::varSlotData が
    /// data() を指す）。スクリプトエントリで xyo_varslot_init が埋める。
    std::vector<Variable*> jitVarSlots;
    mutable std::string idCache_;   // getId() 遅延キャッシュ

    static std::string getIdFromTargetAndBlock(const Target* target,
                                             const std::string& topBlock);
    /// target.id&topBlock — target セット後は不変なので遅延キャッシュ
    std::string getId() const;

    void pushStack(const std::string& blockId);
    void reuseStackForNextBlock(const std::string& blockId);
    std::string popStack();
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
