// xyo-cpp: LLVM ORC JIT — design.md §7。
// スクリプトを `void fn(JitCtx*)` にコンパイルし Fiber 上で実行する。
// yield は xyo_yield による fiber switch（任意の呼び出し深さから可能）。
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../core/value.hpp"
#include "../engine/deps.hpp"
#include "../gpu/gpu_isa.hpp"
#include "../gpu/gpu_compile.hpp"

namespace llvm { class LLVMContext; }
namespace llvm::orc { class LLJIT; }

namespace xyo {

struct Runtime;
struct Sequencer;
struct Target;
struct Thread;
struct Blocks;
struct Block;
struct ExecContext;
struct Variable;
class Fiber;

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
};

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

/// コンパイル済みスクリプト（target->blocks 共有 — クローン間で共有可）
struct CompiledScript {
    ScriptFn entry = nullptr;
    /// variantKey(proccode+warp) → 手続き関数
    std::unordered_map<std::string, ScriptFn> procedures;
    /// 生成コード内に yield 点が存在するか。false なら fiber なしで同期実行可。
    bool mayYield = true;
    /// Compat/reporter fallbacks or AOT-owned GPU programs require the caller
    /// thread even when opcode-level dependency analysis looks compute-only.
    bool mainThread = false;
    /// GPU 並列ループプログラム（生成コードがポインタで参照するため
    /// モジュールと同寿命でここが所有）。生成コードが実際に参照する
    /// のは `ParallelLoop::program`。
    std::vector<std::unique_ptr<gpu::ParallelLoop>> gpuProgs;
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
        bool aot = false;
        bool hasLoop = false;
    };
    bool scriptStats(const void* top, ScriptStats* out) const;

    /// スレッドの依存解析結果 — (topBlock, target) 単位でキャッシュ。
    /// 結果は常に非 null の AccessSet*（解析不能要素は unknown フラグで
    /// 表現）。並列実行可否は workerSafe() と GPU 呼出しの有無で判定。
    /// yield する fiber も、GC を遅延したバッチ内では並列実行できる。
    const AccessSet* accessFor(const Thread& thread, Runtime& runtime);

    /// AOT オブジェクト（`xyo aot` が生成）をロードし、slot に実
    /// アドレスを束縛してスクリプトを通常のコンパイルキャッシュへ
    /// 登録する。projectHash は sb3 の FNV-1a（不一致なら拒否）。
    bool loadAot(Runtime& runtime, const void* object, size_t size,
                 uint64_t projectHash, std::string* why = nullptr);

    /// コンパイル済みスクリプト/依存解析の全キャッシュを破棄する。
    /// 実行中スレッドが無いこと（CompiledScript/BlockCached 参照を
    /// 破棄するため）を呼び出し側が保証する。
    void invalidate();

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
    std::unordered_map<const void*, ScriptProfile> profiles_;
    /// スクリプトの静的走査（初回のみ）。戻り値は hasLoop（0/1/2）、
    /// *nodes に到達可能ブロック数の目安を返す。
    uint8_t scanScriptLoop(Blocks& blocks, const Block* top,
                           uint32_t* nodes);
    /// コンパイル失敗/対象外の topBlock（再試行しない — spawn 毎に
    /// compile が呼ばれるためヒット経路を安く保つ）
    std::unordered_set<const void*> failed_;
    /// 依存解析キャッシュ: topBlock → (target → AccessSet)。
    /// 変数解決がターゲット依存（自ターゲット→ステージ）のため
    /// target ごとに再計算する。
    std::unordered_map<const void*,
                       std::unordered_map<const Target*, AccessSet>>
        accessCache_;
    uint64_t scriptCounter_ = 0;
};

} // namespace xyo
