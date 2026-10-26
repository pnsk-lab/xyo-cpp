// xyo-cpp: Blockly 準拠のブロックレイアウト（描画とヒットテストで共有）。
//
// scratch-blocks (block_render_svg_vertical.js) の寸法モデル:
//   GRID_UNIT=4。ブロック高 MIN_BLOCK_Y=48、フィールド高 32、
//   ノッチ幅 36（接続 32）・深さ 8、C ブロック腕 16、帽子 16。
// 座標はすべて「ワークスペース論理 px」（sb3 の x/y と同じ単位）。
// 描画側が zoom 倍率を掛ける。SDL・フォント実装には非依存。
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "catalog.hpp"
#include "geom.hpp"

namespace xyo {

class Blocks;
struct Block;

namespace blockly {

// ---- 寸法（論理 px。scratch-blocks の定数そのまま）------------------------
inline constexpr int kStackH = 48;       // MIN_BLOCK_Y（段の高さ）
inline constexpr int kRepH = 40;         // MIN_BLOCK_Y_REPORTER
inline constexpr int kShadowH = 32;      // MIN_BLOCK_Y_SINGLE_FIELD_OUTPUT
inline constexpr int kFieldH = 32;       // フィールド/入力行の高さ
inline constexpr int kCornerR = 4;       // CORNER_RADIUS
inline constexpr int kHatH = 16;         // START_HAT_HEIGHT
inline constexpr int kNotchX = 12;       // ノッチの左端
inline constexpr int kNotchH = 8;        // NOTCH_HEIGHT
inline constexpr int kNotchConn = 32;    // 接続オフセット
inline constexpr int kArmW = 16;         // STATEMENT_INPUT_EDGE_WIDTH
inline constexpr int kMouthHMin = 24;    // MIN_STATEMENT_INPUT_HEIGHT
inline constexpr int kCapH = 32;         // EXTRA_STATEMENT_ROW_Y
inline constexpr int kMinW = 64;         // MIN_BLOCK_X
inline constexpr int kMinRepW = 48;      // MIN_BLOCK_X_OUTPUT
inline constexpr int kMinShadowW = 40;   // MIN_BLOCK_X_SHADOW_OUTPUT
inline constexpr int kTextPx = 16;       // 12pt @96dpi 論理 px
inline constexpr int kFieldTextPx = 16;  // 12pt @96dpi（field.js blocklyText）
inline constexpr int kSepX = 8;          // SEP_SPACE_X
inline constexpr int kLabelPadX = 8;     // 行の先頭パディング（SEP_SPACE_X）
inline constexpr int kFieldPadX = 6;     // EDITABLE_FIELD_PADDING（白フィールド内余白）
inline constexpr int kBoxPadX = 8;       // BOX_FIELD_PADDING（非編集系の内余白）
inline constexpr int kInputMinX = 48;    // INPUT_AND_FIELD_MIN_X（ノッチ回避）
inline constexpr int kFieldMinW = 24;    // FIELD_WIDTH
inline constexpr int kArrowW = 12;       // ドロップダウン▼の幅
inline constexpr int kArrowPad = 8;      // DROPDOWN_ARROW_PADDING
inline constexpr int kEmptySocketW = 40; // 空入力ソケット（INPUT_SHAPE_SQUARE_WIDTH）
inline constexpr int kBoolSocketW = 48;  // 空入力ソケット（INPUT_SHAPE_HEXAGONAL_WIDTH）
inline constexpr int kExtStackH = 56;    // 拡張ブロックは 2GU 高い
inline constexpr int kExtRepH = 48;      // 拡張記者も同様
inline constexpr int kIconW = 24;        // ブロック内アイコン（flag/repeat…）
inline constexpr float kWsZoom = 0.675f; // scratch-gui 既定ズーム

/// テキスト計測・描画を注入する（エディタ側が FontRenderer を束ねる）。
struct TextRenderer {
    void* ctx = nullptr;
    int (*measure)(void* ctx, const std::string& utf8, int px,
                   bool bold) = nullptr;
    void (*draw)(void* ctx, float x, float y, const std::string& utf8,
                 int px, bool bold, Color c) = nullptr;
    /// 中線基準のテキスト（field.js: y=size.height/2+FIELD_TOP_PADDING(2),
    /// dominant-baseline=middle）。yMid は中線の y。
    void (*drawMid)(void* ctx, float x, float yMid, const std::string& utf8,
                    int px, bool bold, Color c) = nullptr;
    /// draw の座標系の拡大率（論理 px → 描画 px）。measure は論理 px のまま。
    float scale = 1.0f;
    int width(const std::string& s, int px, bool bold = false) const {
        return measure ? measure(ctx, s, px, bold) : (int)s.size() * px / 2;
    }
    void text(float x, float y, const std::string& s, int px, bool bold,
              Color c) const {
        if (draw) draw(ctx, x, y, s, px, bold, c);
    }
    void textMid(float x, float yMid, const std::string& s, int px, bool bold,
                 Color c) const {
        if (drawMid) drawMid(ctx, x, yMid, s, px, bold, c);
        else if (draw) draw(ctx, x, yMid, s, px, bold, c);
    }
};

// ---- ラベル区間 -----------------------------------------------------------
enum class SegKind : uint8_t {
    Text,       // ラベル文字（白・太字）
    NumField,   // 白いピル（数値/角度。編集可）
    StrField,   // 白い角丸箱（テキスト。編集可）
    Dropdown,   // ブロック色の箱 + ▼（メニュー）
    Variable,   // 変数/リストドロップダウン（ピル + ▼）
    Icon,       // flag/repeat/stop アイコン
    Socket,     // 空の value input（round/square/hex の「穴」）
    Input,      // 中身のある value input（shadow フィールドまたは入れ子ブロック）
    BoolInput,  // hexagonal の value input
    Swatch,     // カラースウォッチ（pen の色選択。白枠 + #rrggbb 塗り）
};

/// ラベル行の 1 区間。x,y,w,h はブロック左上からの相対（論理）。
struct LSeg {
    SegKind kind = SegKind::Text;
    std::string input;        // 対応する inputs のキー（field/input 系）
    std::string field;        // 対応する fields のキー（field 系）
    int arg = -1;             // BlockDef::args インデックス（-1 = 無関係）
    std::string text;         // 表示文字列
    bool hex = false;         // Socket/Input: hexagonal か（boolean）
    bool square = false;      // Socket: INPUT_SHAPE_SQUARE か（false=ROUND ピル）
    bool list = false;        // Variable: LIST か（色分け用）
    float x = 0, y = 0, w = 0, h = 0;
    bool sh = false;          // shadow 展開された区間（入力間隔=SEP のみ）
    int segOrder = 0;         // 安定した走査順（ヒットテスト用）
};

/// C ブロックの本体（口）。innerH は内側の空き高さ（中身込み）。
struct LMouth {
    int branch = 0;           // 0=SUBSTACK, 1=SUBSTACK2
    float x = 0, y = 0;       // 口の左上（腕の右側）
    float w = 0;              // 口の利用幅
    float innerH = 0;         // 中身チェーンの高さ（実測。空なら最小口高）
    float headX = 0, headY = 0; // 中身先頭ブロックの配置位置
};

// ---- レイアウト済みブロック ------------------------------------------------
struct LBlock {
    std::string id;
    float x = 0, y = 0;       // 左上（ws 論理座標 or 画面上の絶対論理座標）
    float w = 0;
    float h = 0;              // 本体高（下バンプ含まず）
    float fullH = 0;          // h + （あれば）下バンプ高
    edit::Shape shape = edit::Shape::Stack;
    uint8_t cblock = 0;       // 0/1/2
    bool shadow = false;
    bool hat = false;
    bool defineHat = false;   // procedures_definition の丸い帽子（上に張り出す）
    bool noBump = false;      // 下にバンプを持たない（End/forever）
    bool capRound = false;    // forever の丸い底キャップ
    float headerH = 0;        // 先頭行の高さ（ハット含む）
    std::vector<LSeg> segs;
    std::vector<LMouth> mouths;
    std::string sub2Label;    // else ラベル（cblock==2）
    float sub2LabelY = 0;     // else 中間バーの y
    int depth = 0;
    Block* block = nullptr;

    const LSeg* segAt(float lx, float ly) const {
        for (const auto& s : segs)
            if (lx >= s.x && lx < s.x + s.w && ly >= s.y && ly < s.y + s.h)
                return &s;
        return nullptr;
    }
};

// ---- モデル補助 -----------------------------------------------------------
std::string chainTail(Blocks* blocks, const std::string& id);
std::string prevOf(Blocks* blocks, const std::string& id);
edit::Shape blockShapeOf(Blocks* blocks, const std::string& id);
void collectSubtree(Blocks* blocks, const std::string& id,
                    std::set<std::string>& out);
/// ラベルが必要とするブロックの表示情報（catalog 無し時のフォールバック含む）。
struct BlockStyle {
    edit::Shape shape = edit::Shape::Stack;
    uint8_t cblock = 0;
    std::string label;        // %n 展開前の message0（不明時は proccode/opcode）
    std::string sub2Label;
    Color primary{0x99, 0x66, 0x80, 255};
    Color secondary{0x77, 0x4C, 0x6A, 255};
    Color tertiary{0x55, 0x33, 0x4D, 255};
    bool known = false;
    bool ext = false;         // 拡張ブロック（scratch の isScratchExtension → 背が高い）
};
BlockStyle styleOf(Blocks* blocks, const Block* b);

// ---- 測量 / 配置 -----------------------------------------------------------
/// ブロックのラベル区間を生成する（フィールド箱・ソケット含む）。
/// x,y はブロック左上基準。子ブロックの見積もりに使われるので純粋関数。
void measureSegs(Blocks* blocks, const Block* b, const TextRenderer& tr,
                 const BlockStyle& st, std::vector<LSeg>& out);
/// ブロックのサイズを測る（再帰。入れ子ブロックの大きさに依存）。
struct BlockSize {
    float w = 0;
    float h = 0;              // 本体高（バンプ除く）
    float headerH = 0;        // 先頭行の高さ（C ブロックの口開始位置）
    std::vector<LSeg> segs;
    float innerW = 0;         // 内側の使用幅（C ブロック最小幅判定用）
};
BlockSize measureBlock(Blocks* blocks, const Block* b,
                       const TextRenderer& tr);

/// head から始まるチェーンを (x,y) に配置し LBlock 列を返す。
/// out はチェーン全体（SUBSTACK の中身を含む全ブロック）。y は進む。
/// 戻り値: チェーン下端の y（次に置くべき位置）。
float layoutChain(Blocks* blocks, const std::string& head, float x, float y,
                  const TextRenderer& tr, std::vector<LBlock>& out,
                  int depth = 0);
/// チェーンの総高さ（下端 y - 上端 y）。
float chainHeight(Blocks* blocks, const std::string& head,
                  const TextRenderer& tr);

/// sb3 の x/y を持つトップレベルをその位置へ、x/y 無し（0,0 or None）の
/// スクリプトを自動整列する（重なり回避付き）。
struct ScriptLayout {
    std::vector<LBlock> blocks;             // 全ブロック（配置済み）
    std::map<std::string, int> headAt;      // トップレベル id → blocks 添字
};
ScriptLayout layoutWorkspace(Blocks* blocks, const TextRenderer& tr);

} // namespace blockly
} // namespace xyo
