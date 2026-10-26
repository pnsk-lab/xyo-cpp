// xyo-cpp: プロジェクト構造解析 — 静的な「構造的最適化」候補の抽出。
//
// 実行せずに sb3 を読み、以下を出す:
//   * ターゲット/スクリプト/ブロックの規模
//   * スクリプトごとの到達可能ブロック数・ループ・手続き呼出し・yield
//   * deps.cpp の AccessSet（並列スケジューラと同一の静的解析）による
//     読み書き集合と、worker で走れるか（spawn/unknown/mainThread）
//   * 衝突グラフの連結成分 = 並列度の理論上限
//   * 直列化の原因キー（変数/フィールド/グローバル）ランキング
//   * 手続き（proccode/warp/呼出し数/再帰/本体の yield）
//   * ループ内の O(n) 走査・broadcast・wait 等の構造的アンチパターン
//
// 解析は非破壊（Runtime を書き換えない）。変数解決は deps/JIT と同一規則。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "../engine/deps.hpp"

namespace xyo {
struct Block;
struct Runtime;
struct Target;
}  // namespace xyo

namespace xyo::analysis {

/// ループ内で見つかった構造的アンチパターンの件数。
struct Patterns {
    size_t itemOfInLoop = 0;      // data_itemoflist / data_itemnumoflist
    size_t listScanInLoop = 0;    // data_listcontainsitem / data_lengthoflist
    size_t broadcastInLoop = 0;   // event_broadcast / event_broadcastandwait
    size_t waitInLoop = 0;        // ループ内の yield（wait/sayforsecs 等）
    size_t cloneInLoop = 0;       // control_create_clone_of
};

/// トップレベルブロックの種別。ハットだけがスレッドを spawn する。
enum class ScriptKind : uint8_t {
    Hat = 0,        // event_*/control_start_as_clone 等（実行される）
    Definition,     // procedures_definition（呼出し経由でのみ実行）
    ReporterTop,    // 単体で置かれた reporter / 変数ブロック（実行されない）
};

/// トップレベル 1 スクリプトの静的情報。
struct ScriptInfo {
    Target* target = nullptr;
    std::string targetName;
    bool isStage = false;
    const Block* top = nullptr;
    std::string topId;
    std::string hat;           // top->opcode（"procedures_definition" も含む）
    ScriptKind kind = ScriptKind::Hat;
    std::string label;         // 表示用: "draw:event_whenbroadcastreceived" / "bsp/gen"
    std::string proccode;      // procedures_definition のみ
    bool warp = false;         // 定義側の "run without screen refresh"

    size_t blocks = 0;         // 到達可能ブロック数（定数 shadow を除く。手続き本体は 1 回）
    size_t selfBlocks = 0;     // うち手続き呼出しの先を含まない「自己」のブロック数
    size_t maxDepth = 0;       // C ブロック/入力の入れ子の深さ
    size_t loops = 0;          // 到達可能なループ op 数
    size_t procCalls = 0;      // procedures_call 数（手続き本体の分も含む）
    size_t broadcasts = 0;     // broadcast / broadcastandwait 数
    bool yields = false;       // yield し得る（非 warp のループ境界を含む）
    bool inherentYield = false;  // wait/glide/sound 等、warp でも消えない yield

    AccessSet access;          // deps.cpp と同一の読み書き集合
    std::vector<AccessNote> notes;  // 逐次実行を強制した理由（最大 8 件）
    std::vector<std::string> reads, writes;  // describeKey 済み・ソート済み
    Patterns patterns;
};

/// 手続き定義 1 件の静的情報。
struct ProcedureInfo {
    std::string targetName;
    std::string proccode;
    bool warp = false;
    size_t blocks = 0;         // 本体のみ（入れ子の呼び出し先は含まない）
    size_t calls = 0;          // プロジェクト全体からの呼出し数
    bool selfRecursive = false;
    bool hasLoop = false;
    bool yields = false;       // 本体に wait/glide/sound 等の「消えない」yield がある
};

/// ターゲット 1 件の規模。
struct TargetInfo {
    std::string name;
    bool isStage = false;
    size_t blocks = 0;         // 保持している全ブロック
    size_t reachable = 0;      // トップから到達できるブロック
    size_t scripts = 0;        // トップレベルスクリプト数
    size_t variables = 0;
    size_t lists = 0;
    size_t costumes = 0;
    size_t sounds = 0;
};

/// 直列化の原因になるメモリキーと、そのキーを共有するスクリプト。
struct ConflictInfo {
    std::string key;                  // "var:score@Stage" 等（所有者付き）
    MemKey raw;
    std::vector<size_t> scripts;      // ScriptInfo の index（書き手を先頭に）
    size_t writers = 0;
};

struct Finding {
    enum class Level { Info, Note, Warn };
    Level level = Level::Info;
    std::string text;
};

struct Report {
    std::string path;

    std::vector<TargetInfo> targets;
    std::vector<ScriptInfo> scripts;
    std::vector<ProcedureInfo> procedures;
    std::vector<ConflictInfo> conflicts;
    std::vector<Finding> findings;

    size_t totalBlocks = 0;
    size_t unreachableBlocks = 0;
    /// どこからも参照されていない定数 shadow（未使用の引数レポータ等）
    size_t orphanedShadows = 0;
    size_t unusedVariables = 0;
    size_t unusedLists = 0;
    bool variableUsageDynamic = false;
    size_t executableScripts = 0;   // ハット数（実際に spawn するスクリプト）
    size_t definitions = 0;
    size_t reporterTops = 0;

    // 並列性（ハット = 実行されるスクリプトのうち workerSafe なものの衝突グラフ）
    size_t workerSafe = 0;
    size_t spawnScripts = 0;
    size_t unknownScripts = 0;
    size_t mainThreadScripts = 0;
    size_t maxComponent = 0;
    std::vector<size_t> componentSizes;   // 降順
    std::vector<std::vector<size_t>> groups;  // 連結成分（ScriptInfo index）
};

struct Options {
    /// 衝突キーの表示件数（0 = 全部）
    size_t topConflicts = 20;
    /// スクリプト表の表示件数（0 = 全部）
    size_t topScripts = 0;
};

/// Runtime を読み取り専用で解析する。
Report analyzeProject(Runtime& rt, const std::string& path,
                      const Options& opt = {});

/// 人が読むテキストレポート。
std::string formatText(const Report& r, const Options& opt = {});

/// workerSafe スクリプトの衝突グラフを Graphviz DOT で出力（XYO_DEPGRAPH と同形式）。
std::string formatDot(const Report& r);

/// レポートから sb3 に戻す変換の候補（warp 化・変数分離など）を抽出。
/// 変換は適用せず、対象と根拠だけを返す。
struct TransformHint {
    enum class Kind : uint8_t { WarpProcedure, SplitVariable, ListScan };
    Kind kind = Kind::WarpProcedure;
    std::string target;   // 対象名（proccode / 変数ラベル）
    std::string reason;
};
std::vector<TransformHint> suggestTransforms(const Report& r);

}  // namespace xyo::analysis
