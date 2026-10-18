// xyo-cpp: スレッド間メモリ依存解析 — design.md §4.6。
//
// 各スレッドのスクリプトを静的に走査して読み/書きメモリキー集合を
// 求め、衝突グラフを構築する。キーは 3 種:
//   Var    — 解決済み Variable*（スカラ・リスト共通。リスト内容も同一キー）
//   Field  — (Target*, TField) ターゲット固有属性 (x, y, size, …)
//   Global — (nullptr, GKey) ランタイム共有状態 (timer, monitor, …)
//
// 変数解決は非破壊ルックアップで JIT と同一規則（id → 自ターゲット →
// ステージ、name → 同様）を使う。実行時に変数を「生成」する経路
// （未解決フィールド）は target->variables の map 変異になるため
// unknown 扱いで並列から外す。broadcast/clone/stop/背景切替/レイヤ
// 操作などスケジューラ・共有構造を変える op は spawn/unknown で検出し、
// そのスレッド全体を逐次実行に回す。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace xyo {

struct Block;
struct Blocks;
struct Runtime;
struct Target;
struct Thread;
struct Variable;

// --- メモリキー -----------------------------------------------------------

/// ターゲット固有属性のサブ種別
enum class TF : uint8_t {
    X, Y, Dir, Size, Visible, RotStyle, Costume, Effect, Layer,
    Bubble, Drag, Volume, Sfx, _Count
};
/// ランタイム共有状態のサブ種別
enum class GK : uint8_t {
    Io,        // マウス/キー/answer/username/clock/loudness（読み専用系）
    Timer,     // sensing_timer / resettimer
    Monitor,   // monitorState / monitorBlocks キャッシュ
    Say,       // onSay コールバックの観測順序
    Audio,     // soundBank / audioEngine
    Counter,   // controlCounter
    Backdrop,  // ステージの currentCostume
    _Count
};

struct MemKey {
    enum Kind : uint8_t { Var = 0, Field = 1, Global = 2 };
    const void* a = nullptr;  // Var: Variable* / Field: Target* / Global: nullptr
    Kind kind = Var;
    uint8_t sub = 0;          // Field/Global のサブ種別

    static MemKey var(const Variable* v) {
        MemKey k; k.a = v; k.kind = Var; return k;
    }
    static MemKey field(const Target* t, TF s) {
        MemKey k; k.a = t; k.kind = Field; k.sub = uint8_t(s); return k;
    }
    static MemKey global(GK s) {
        MemKey k; k.kind = Global; k.sub = uint8_t(s); return k;
    }
    bool operator==(const MemKey& o) const {
        return a == o.a && kind == o.kind && sub == o.sub;
    }
};

struct MemKeyHash {
    size_t operator()(const MemKey& k) const {
        size_t h = reinterpret_cast<uintptr_t>(k.a) >> 4;
        return h ^ (size_t(k.kind) << 8 | k.sub) * 0x9E3779B97F4A7C15ull;
    }
};

/// 1 スレッド（スクリプト）のメモリアクセス集合
struct AccessSet {
    std::unordered_set<MemKey, MemKeyHash> reads, writes;
    /// スケジューラ/ランタイム構造の変更（broadcast 起動・clone・stop・
    /// 背景切替・レイヤ並べ替え等）→ 逐次実行を強制
    bool spawn = false;
    /// 未解析/compat/動的ターゲット解決/実行時変数生成 → 逐次実行を強制
    bool unknown = false;

    bool safe() const { return !spawn && !unknown; }
    /// 書き込みキーが相手の読み/書きと交差するか（読み同士は衝突しない）
    bool conflicts(const AccessSet& o) const {
        for (const MemKey& w : writes)
            if (o.writes.count(w) || o.reads.count(w)) return true;
        for (const MemKey& r : reads)
            if (o.writes.count(r)) return true;
        return false;
    }
};

/// DOT 出力用のキー説明（"var:score@Sprite1" / "field:x@Sprite1" / "g:timer"）
std::string describeKey(const MemKey& k);

// --- 解析 -----------------------------------------------------------------

/// スクリプトのアクセス集合を静的解析。top はハット/先頭ブロック —
/// top 自身は評価されない（startHats が処理済み）ため top->next から走査。
/// 例外は投げない。未解決要素は unknown フラグで表現される。
AccessSet analyzeScript(Runtime& rt, Target& target, Blocks& blocks,
                        const Block* top);

// --- 衝突グラフ -----------------------------------------------------------

/// バッチ内スレッドの依存グラフ。ノード = バッチ内 index。
struct DepGraph {
    size_t n = 0;
    std::vector<std::vector<size_t>> adj;    // 無向衝突辺
    /// エッジ理由（DOT 用）— (i,j) i<j のキー説明リスト
    std::vector<std::pair<size_t, size_t>> edges;
    std::vector<std::string> edgeLabels;

    /// 衝突辺で union-find → 連結成分ごとのスレッド列（バッチ内 index 順）。
    /// 返り値: グループ配列（各グループはバッチ順 = spawn 順を保持）
    std::vector<std::vector<size_t>> groups() const;
};

/// バッチ内スレッド群の衝突グラフを構築（O(n²) ペア判定、n は小さい前提）
DepGraph buildDepGraph(const std::vector<const AccessSet*>& sets);

/// DOT 形式ダンプ。names[i] はノード名（"Sprite:hats.opcode" 等）
std::string depGraphToDot(const DepGraph& g,
                          const std::vector<std::string>& names);

} // namespace xyo
