// xyo-cpp: engine/runtime.js 相当 — ランタイム中核。
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/json.hpp"
#include "../core/value.hpp"
#include "../model/blocks.hpp"
#include "../model/target.hpp"
#include "../model/variable.hpp"
#include "io_devices.hpp"
#include "thread.hpp"

namespace xyo {

/// 環境変数 `XYO_DANGER` が真値（未設定・`0`・空以外）なら true。
/// Runtime::dangerMode の既定値（`--danger` は構築後に上書きする）。
bool dangerModeFromEnv();

class Gc;
class JitEngine;
struct Sequencer;
struct Sprite;
struct Target;
namespace gpu { struct ExecRequest; struct NativeRequest; }

/// ハットメタデータ（packageHats / extension isEdgeActivated 等）
struct HatMeta {
    bool edgeActivated = false;
    bool restartExistingThreads = false;
};

/// _flowing — 拡張 conditional/loop ブロックの分岐メタデータ
struct FlowingFlags {
    bool conditional = false;
    bool loop = false;
};

/// レンダラ抽象（NullRenderer / 将来の実レンダラ）
struct RendererAPI {
    virtual ~RendererAPI() = default;
    virtual int pick(double x, double y) { (void)x; (void)y; return -1; }
    virtual void draw() {}
    virtual bool drawableTouching(int, double, double) { return false; }
    virtual bool isTouchingDrawables(int, const int*, size_t) { return false; }
    virtual bool isTouchingColor(int, uint32_t) { return false; }
    virtual bool colorTouchingColor(uint32_t, uint32_t, int) { return false; }
    /// {left,right,top,bottom}
    virtual void getDrawableBounds(int, double* b) const { (void)b; }
    virtual bool hasDrawable(int) const { return false; }
    /// setXY 用 — fencing 時に範囲内へ丸める（renderer 責務）
    virtual void getFencedPosition(int drawableID, double& x, double& y,
                                   bool fencing) const {
        (void)drawableID; (void)fencing;
    }
    virtual void getCurrentSkinSize(int drawableID, double& w, double& h) const {
        (void)drawableID; w = h = 1;
    }
    /// goToFront/Back/Layers/Behind 用 — 新レイヤ値を返す
    virtual double setDrawableOrder(int drawableID, double order,
                                    const char* group, bool isRelative) {
        (void)drawableID; (void)order; (void)group; (void)isRelative;
        return 0;
    }
    virtual void destroyDrawable(int) {}
    virtual void emitVisualChange(Target*) {}
    /// 背面→前面の drawableID 列（isTouchingObject 用）
    virtual std::vector<int> drawableOrder() const { return {}; }
    virtual int drawableOfTarget(Target*) const { return -1; }

    // --- ペン（実レンダラのみ実装。scratch3_pen 拡張から呼ばれる）---
    /// ペン描画レイヤを生成し skin id を返す。失敗時は負値。
    virtual int createPenSkin() { return -1; }
    /// scratch 座標 (x0,y0)→(x1,y1) に直径 diameter の線を引く。
    virtual void penLine(int skin, const float rgba[4], double diameter,
                         double x0, double y0, double x1, double y1) {
        (void)skin; (void)rgba; (void)diameter;
        (void)x0; (void)y0; (void)x1; (void)y1;
    }
    virtual void penPoint(int skin, const float rgba[4], double diameter,
                          double x, double y) {
        (void)skin; (void)rgba; (void)diameter; (void)x; (void)y;
    }
    virtual void penClear(int skin) { (void)skin; }
    /// drawableID の現在の表示をペンスタンプする。
    virtual void penStamp(int skin, int drawableID) {
        (void)skin; (void)drawableID;
    }

    // --- GPU 並列 repeat（GL4.3+ の実レンダラのみ。src/gpu 参照）---
    /// req を実行。成功したら req.executed=true（失敗/非対応は false）。
    virtual bool gpuExecParallel(gpu::ExecRequest& req) {
        (void)req;
        return false;
    }
    virtual bool gpuExecNative(gpu::NativeRequest& req) {
        (void)req;
        return false;
    }
    /// GPU パス用のフェンス矩形 {left,right,bottom,top}（scratch 座標）。
    /// getFencedPosition と同じ枠組みを矩形として返す。
    virtual void getFenceRect(int drawableID, double out[4]) const {
        (void)drawableID;
        out[0] = -240; out[1] = 240; out[2] = -180; out[3] = 180;
    }
};

/// オーディオ抽象
struct AudioAPI {
    virtual ~AudioAPI() = default;
    virtual double getLoudness() const { return -1; }
};

/// monitorBlocks 内のモニターレコード（MonitorRecord 移植）。
struct MonitorRecord {
    std::string id;
    std::string mode = "default";   // default/large/slider/list
    std::string opcode;
    std::map<std::string, std::u16string> params;
    std::u16string spriteName;
    std::string targetId;
    double x = 0, y = 0, width = 0, height = 0;
    bool visible = false;
    double sliderMin = 0, sliderMax = 100;
    bool isDiscrete = true;
    Value value = Value::undefined();   // 直近報告値
};

/// _monitorState — OrderedMap 相当 + dirty。
struct MonitorState {
    std::map<std::string, MonitorRecord> records;
    bool dirty = false;
    bool empty() const { return records.empty(); }
    MonitorRecord& getOrCreate(const std::string& id) { return records[id]; }
};

struct VisualReportEvent {
    Target* target = nullptr;
    std::string blockId;
    Value value = Value::undefined();
};

class Runtime {
public:
    Runtime();
    ~Runtime();

    // --------------------------------------------------------- 主要メンバ --
    std::unique_ptr<Sequencer> sequencer;
    std::unique_ptr<JitEngine> jit;   // XYO_JIT 時のみ非 null
    IoDevices ioDevices;

    std::vector<Target*> targets;            // 全ターゲット（非所有）
    std::vector<Target*> executableTargets;  // 実行対象（stage 最後）
    std::vector<std::unique_ptr<Sprite>> ownedSprites;   // sb3 ロード時の所有
    std::deque<std::unique_ptr<Thread>> ownedThreads;
    std::vector<Thread*> threads;
    std::unordered_map<std::string, Thread*> threadMap;

    std::unique_ptr<Blocks> monitorBlocks;
    std::unique_ptr<Blocks> flyoutBlocks;
    std::unordered_map<std::string, HatMeta> hats;
    std::unordered_map<std::string, PrimFn> primitives;
    std::unordered_map<std::string, FlowingFlags> flowing;

    /// monitorBlockInfo: opcode → {isSpriteSpecific, getId} 相当
    struct MonitorInfo {
        bool isSpriteSpecific = false;
        // getId: (targetId, fields) → monitor block id（後で必要なら）
    };
    std::unordered_map<std::string, MonitorInfo> monitorBlockInfo;
    MonitorState monitorState;

    // --- 時間 ---
    double currentMSecs = 0;
    double currentStepTime = 1000.0 / 30.0;
    bool turboMode = false;
    bool redrawRequested = false;
    bool interpolationEnabled = false;
    bool miscLimits = true;      // runtimeOptions.miscLimits
    bool fencing = true;         // runtimeOptions.fencing
    int stageWidth = 480;
    int stageHeight = 360;
    int maxClones = 300;

    /// warp スレッドが 1 ティック内で連続実行できる時間予算（ms）。
    /// TurboWarp の `WARP_TIME`(500ms) 相当 — 超えると他スレッドへ譲る。
    /// dangerMode では判定自体を飛ばす（予算が無い）。
    double warpTimeMs = 500.0;
    /// 危険モード（`XYO_DANGER=1` / `--danger`）: **スレッドごとの実行時間
    /// 予算を外す**。warp スクリプトは 1 ティックで終わらなくても他スレッド
    /// や描画に譲らず走り続けるため、アプリが固まる（最悪ハングする）。
    /// 長い計算を 1 フレームで走らせたい計測・変換検証専用。既定 off。
    /// **実行開始（最初の spawn/コンパイル）前に決めること** — JIT は
    /// このフラグを前提に warp ループの yield 点を落としたコードを吐く。
    bool dangerMode = dangerModeFromEnv();

    // --- 外部インターフェース ---
    RendererAPI* renderer = nullptr;
    /// Scratch.pen 共有レイヤ skin id（createPenSkin の遅延結果）
    int penSkinId = -1;
    AudioAPI* audioEngine = nullptr;
    Target* editingTarget = nullptr;
    std::u16string platformName = u"xyo";

    // --- クラウド ---
    int cloudVariableCount = 0;
    int maxCloudVariables = 10;
    int cloneCounter = 0;

    // --- コアブロックパッケージ状態 ---
    double controlCounter = 0;                    // control_get_counter
    std::u16string answer;                        // sensing_answer
    struct AskEntry {                             // ask and wait キュー
        std::u16string question;
        Thread* thread;
        Target* target;
        bool wasVisible, wasStage;
    };
    std::deque<AskEntry> questionList;
    double cachedLoudness = -1;
    double cachedLoudnessTimestamp = 0;
    Timer loudnessTimer;

    /// setTimeout 相当の待機キュー（sayforsecs/thinkforsecs 等）。
    /// 各 step 開始時に期限切れ分を resolveWait する。
    struct WaitEntry {
        Thread* thread = nullptr;
        double deadlineMs = 0;
        /// Promise.resolve() 相当: 次の processWaitQueue で無条件解決
        bool nextTick = false;
        Value resolveValue = Value::undefined();
        // sayforsecs/thinkforsecs 用のバブルクリア
        Target* bubbleTarget = nullptr;
        const char* bubbleType = nullptr;
        int bubbleUsageId = 0;
    };
    std::deque<WaitEntry> waitQueue;
    void processWaitQueue();

    /// GC ルートプロバイダ — Runtime 配下の全 Value/SList* をマーク
    void markRoots(Gc& g);

    bool compilerOptionsEnabled = true;   // argument 'is compiled?' 用
    std::string origin;                    // meta.origin（sb3）

    // --- 実行状態 ---
    int projectRunStatus = 0;
    bool refreshTargets = false;                  // _refreshTargets
    VisualReportEvent lastVisualReport;
    std::vector<Thread*> lastStepDoneThreads;

    // --- コールバック（イベント emit 代替）---
    std::function<void(const VisualReportEvent&)> onVisualReport;
    std::function<void(int runningCount)> onProjectRunStatus;
    std::function<void(const MonitorState&)> onMonitorsUpdate;
    std::function<void(Target*, const std::u16string& error)> onCompileError;
    std::function<void()> onTargetsUpdate;
    std::function<void(Target*)> onTargetCreated;
    std::function<void(Target*)> onTargetRemoved;
    /// SAY — 'say'/'think' バブル表示（target, type, text）
    std::function<void(Target*, const char* type, const std::u16string&)> onSay;
    /// QUESTION — ask and wait の入力要求（nullopt でクリア）
    std::function<void(const std::u16string* question)> onQuestion;
    std::function<bool(Target*)> onGpuFrame;
    std::function<void(bool)> onGpuMode;
    std::function<void()> onGreenFlag;
    uint64_t stepSerial = 0;
    /// PROJECT_STOP_ALL 相当（stopAll/greenFlag/「すべてを止める」で発火。
    /// オーディオ停止等に使用）
    std::function<void()> onProjectStopped;

    // -------------------------------------------------- opcode/hat 登録 --
    PrimFn getOpcodeFunction(const std::string& opcode) const {
        auto it = primitives.find(opcode);
        return it == primitives.end() ? nullptr : it->second;
    }
    bool getIsHat(const std::string& opcode) const { return hats.count(opcode) != 0; }
    bool getIsEdgeActivatedHat(const std::string& opcode) const {
        auto it = hats.find(opcode);
        return it != hats.end() && it->second.edgeActivated;
    }
    FlowingFlags getFlowing(const std::string& opcode) const {
        auto it = flowing.find(opcode);
        return it == flowing.end() ? FlowingFlags{} : it->second;
    }
    void registerPrimitive(const std::string& opcode, PrimFn fn) {
        primitives[opcode] = fn;
    }
    void registerHat(const std::string& opcode, HatMeta meta) { hats[opcode] = meta; }
    void registerFlowing(const std::string& opcode, FlowingFlags f) {
        flowing[opcode] = f;
    }

    // -------------------------------------------------------- スレッド管理 --
    /// topHint: 解決済みトップブロック（startHats の RuntimeScriptCache 経由
    /// で getBlock を省略する JIT compile ヒント）
    Thread* pushThread(const std::string& id, Target* target,
                       bool stackClick = false, bool updateMonitor = false,
                       const Block* topHint = nullptr);
    void stopThread(Thread* thread);
    Thread* restartThread(Thread* thread, const Block* topHint = nullptr);
    bool isActiveThread(Thread* thread) const;
    bool isWaitingThread(Thread* thread) const;
    void toggleScript(const std::string& topBlockId, Target* target,
                      bool stackClick = false);
    void addMonitorScript(const std::string& topBlockId, Target* optTarget);
    void updateThreadMap();

    /// allScriptsDo — f(topBlockId, target)
    template <typename F> void forEachScript(F&& f, Target* optTarget) {
        if (optTarget) {
            for (auto& s : optTarget->blocks->getScripts()) f(s, optTarget);
        } else {
            for (int t = int(executableTargets.size()) - 1; t >= 0; --t) {
                Target* target = executableTargets[t];
                for (auto& s : target->blocks->getScripts()) f(s, target);
            }
        }
    }
    /// allScriptsByOpcodeDo — f({blockId, fieldsOfInputs}, target)
    template <typename F> void forEachScriptByOpcode(const std::string& opcode,
                                                     F&& f, Target* optTarget) {
        if (optTarget) {
            for (auto& s : optTarget->blocks->getRuntimeScripts(opcode)) f(s, optTarget);
        } else {
            for (int t = int(executableTargets.size()) - 1; t >= 0; --t) {
                Target* target = executableTargets[t];
                for (auto& s : target->blocks->getRuntimeScripts(opcode)) f(s, target);
            }
        }
    }

    std::vector<Thread*> startHats(
        const std::string& requestedHatOpcode,
        const std::map<std::string, std::u16string>* optMatchFields,
        Target* optTarget);

    void stopForTarget(Target* target, Thread* optException);
    void stopAll();
    void greenFlag();
    void step();                 // _step
    bool stepOnce() { step(); return !threads.empty(); }
    int stepNTimes(int n) { for (int i = 0; i < n; ++i) { if (threads.empty() && i) return i; step(); } return n; }
    /// 全スレッド終了まで最大 maxSteps ティック実行
    int runUntilDone(int maxSteps = 10000);
    void updateCurrentMSecs();

    // ------------------------------------------------------------ ターゲット --
    void addTarget(Target* target);
    void moveExecutable(Target* target, int delta);
    void setExecutablePosition(Target* target, int newIndex);
    void removeExecutable(Target* target);
    void disposeTarget(Target* target);
    Target* getTargetForStage() const;
    Target* getTargetById(const std::string& id) const;
    Target* getSpriteTargetByName(const std::u16string& name) const;
    Target* getTargetByDrawableId(int drawableId) const;
    void requestTargetsUpdate(Target*) { refreshTargets = true; }
    void changeCloneCounter(int delta) { cloneCounter += delta; }
    bool clonesAvailable() const { return cloneCounter < maxClones; }

    // --- ask and wait ---
    /// ホストが回答を供給 → ANSWER イベント相当
    void emitAnswer(const std::u16string& answerText);

    // ------------------------------------------------------------ モニタ --
    void pushMonitors();
    int getMonitorThreadCount(const std::vector<Thread*>& list) const;
    void requestUpdateMonitor(const std::map<std::string, std::u16string>& fields);
    void requestUpdateMonitorValue(const std::string& id, Value v,
                                   const std::u16string& spriteName);
    void requestAddMonitor(const MonitorRecord& rec);
    void requestRemoveMonitor(const std::string& id);
    void requestRemoveMonitorByTargetId(const std::string& targetId);

    // ------------------------------------------------------------ その他 --
    void visualReport(Target* target, const std::string& blockId, Value value);
    void emitKeyPressed(const std::u16string& scratchKey);
    void emitCompileError(Target* target, const std::u16string& error);
    void onTargetGreenFlag(Target* target);
    void emitProjectRunStatus(int running);
    Variable* createNewGlobalVariable(const std::u16string& name,
                                      const char* type,
                                      const std::string& id = "");
    std::vector<std::u16string> getAllVarNamesOfType(const char* type) const;
    bool canAddCloudVariable() const {
        return cloudVariableCount < maxCloudVariables;
    }
    void addCloudVariable() { ++cloudVariableCount; }
    void removeCloudVariable() { --cloudVariableCount; }
    void requestBlocksUpdate() {}
    void emitTargetsUpdate() {
        if (onTargetsUpdate) onTargetsUpdate();
    }
    /// STOP_FOR_TARGET — ask キューから対象の質問を除去
    void emitStopForTarget(Target* t, Thread* exc);
    /// PROJECT_STOP_ALL — 全質問クリア + サウンド停止等
    void emitProjectStopAll();
    /// PROJECT_START — answer リセット + エフェクトクリア
    void emitProjectStart();
    void clearAllQuestions();
};

/// execute.js — 1ブロック分の _ops を実行
void execute(Sequencer& sequencer, Thread& thread);
/// 非同期プリミティブの完了報告（JS promise 解決相当）
void resolveWait(Sequencer& sequencer, Thread& thread, Value resolvedValue);

} // namespace xyo
