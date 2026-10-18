// xyo-cpp: LLVM ORC JIT — design.md §7。
// スクリプトを `void fn(JitCtx*)` にコンパイルし Fiber 上で実行する。
// yield は xyo_yield による fiber switch（任意の呼び出し深さから可能）。
#pragma once

#include <cstdint>
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

    /// スレッドの依存解析結果 — (topBlock, target) 単位でキャッシュ。
    /// 結果は常に非 null の AccessSet*（解析不能要素は unknown フラグで
    /// 表現）。並列実行可否は `set->safe()` + `!cs->mayYield` で判定。
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
    /// トップ Block* → CompiledScript（Block は Blocks 所有でポインタ安定）
    std::unordered_map<const void*, std::unique_ptr<CompiledScript>> cache_;
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
