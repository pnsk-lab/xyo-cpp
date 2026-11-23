// xyo-cpp: MLIR 風フロントエンドの入口実装。
//
// 既存の `jitEmitScript` からのフック（**既定で有効**、`XYO_MLIR=0` で
// 無効化）。対応サブセットのスクリプトを
// 「ブロック木 → xyo dialect → pass → LLVM IR」で emit する。
// サブセット外・検証失敗・lowering 失敗では一切 `mod` を変更せず false を
// 返し、既存 codegen がそのまま走る（フォールバックは常に安全側）。
#include "frontend.hpp"

#if defined(XYO_JIT) && !defined(__EMSCRIPTEN__)

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include "../engine/mega_preset.hpp"
#include "../engine/runtime.hpp"
#include "../core/cast.hpp"
#include "../jit/aot_emit.hpp"
#include "../jit/jit.hpp"
#include "../jit/stackless_coro_lower.hpp"
#include "../core/sstr.hpp"
#include "../model/blocks.hpp"
#include "ir.hpp"
#include "lower_llvm.hpp"
#include "scratch_build.hpp"

namespace xyo::mlir {
namespace {

std::atomic<int> g_force{-1};
std::atomic<uint64_t> g_compiled{0};
std::atomic<uint64_t> g_fallback{0};
std::mutex g_dumpMutex;
std::string g_lastDump;

bool truthful(const char* s) {
    if (!s || !*s) return false;
    return std::strcmp(s, "0") != 0 && std::strcmp(s, "off") != 0 &&
           std::strcmp(s, "false") != 0 && std::strcmp(s, "no") != 0;
}

bool traceEnabled() {
    static const bool on = truthful(std::getenv("XYO_MLIR_TRACE"));
    return on;
}

void trace(const char* fmt, ...) {
    if (!traceEnabled()) return;
    va_list ap;
    va_start(ap, fmt);
    std::fputs("[mlir] ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}

std::string scriptLabel(const std::string& topId, const xyo::Block* top) {
    return topId + " (" + (top ? top->opcode : std::string("?")) + ")";
}

// 整数プロモーションの適格変数を集める。条件は promoter 側の健全性
// 前提そのまま: target-local・非 cloud・slider 非監視・書き込みは
// 整数リテラルか整数化ループ内。書き込みの整数値は promoter が数える。
namespace {
const xyo::Block* resolveInputBlock(const Blocks& blocks,
                                    const xyo::Block& parent, const char* name) {
    const BlockInput* in = parent.inputs.find(name);
    if (!in) return nullptr;
    const std::string& id = !in->block.empty() ? in->block : in->shadow;
    if (id.empty()) return nullptr;
    return blocks.getBlock(id);
}

bool blockIntLiteral(const xyo::Block* b) {
    if (!b) return false;
    const std::string& op = b->opcode;
    if (op != "math_number" && op != "math_integer" &&
        op != "math_whole_number" && op != "math_positive_number" &&
        op != "math_angle")
        return false;
    auto it = b->fields.find("NUM");
    if (it == b->fields.end()) return false;
    // fractch/TW 形式では NUM が文字列（"0"）で来る。共有ヘルパーで
    // 数値解釈＋正準形まで判定する（整数プロモーションと同一基準）。
    int64_t c = 0;
    return intLiteralBits(it->second.value.bits, &c);
}
/// 整数値を作る式か（ブロック木の再帰）。リテラル整数・四則・mod・
/// round と clean 集合に入った変数読み・clean リストの要素読みを認める
/// （除算・文字列は不可）。clean 集合は呼び出し側の fixpoint が育てる。
bool blockIntExpr(const Blocks& tb, const xyo::Block* b,
                  const std::unordered_set<std::string>& cleanVars,
                  const std::unordered_map<std::string,
                                           std::pair<int64_t, int64_t>>&
                      cleanLists,
                  int depth = 0) {
    if (!b || depth > 8) return false;
    const std::string& op = b->opcode;
    if (op == "math_number" || op == "math_integer" ||
        op == "math_whole_number" || op == "math_positive_number" ||
        op == "math_angle")
        return blockIntLiteral(b);
    if (op == "operator_round")
        return blockIntExpr(tb, resolveInputBlock(tb, *b, "NUM"), cleanVars,
                            cleanLists, depth + 1);
    if (op == "operator_divide") {
        // 除算は整数値ではない。Scratch の x/m は実数（例: 5/2 = 2.5）で、
        // 整数表現（floor）に落とすと後で実数として使う箇所が変わる。
        // 「正の定数除数なら floor でよい」は floor をそのまま使う文脈
        // （リスト索引の切り捨て等）でしか成り立たず、変数/リストへの
        // 書き込みや mathop 入力には使えないので昇格対象から外す。
        return false;
    }
    if (op == "operator_add" || op == "operator_subtract" ||
        op == "operator_multiply" || op == "operator_mod") {
        return blockIntExpr(tb, resolveInputBlock(tb, *b, "NUM1"), cleanVars,
                            cleanLists, depth + 1) &&
               blockIntExpr(tb, resolveInputBlock(tb, *b, "NUM2"), cleanVars,
                            cleanLists, depth + 1);
    }
    if (op == "data_variable") {
        auto it = b->fields.find("VARIABLE");
        return it != b->fields.end() && cleanVars.count(it->second.id) != 0;
    }
    if (op == "data_itemoflist") {
        auto it = b->fields.find("LIST");
        return it != b->fields.end() && cleanLists.count(it->second.id) != 0;
    }
    return false;
}
/// 常に数値（素の double）を生成する式か（ブロック木の再帰）。
/// 算術/比較系の演算子は Scratch では常に数値（NaN 含む）を返すので、
/// オペランドの型は問わない。変数読みは集合に含まれるときだけ数値。
/// 数値要素リストの item 参照・リスト長・リスト内位置も数値。
bool blockNumericExpr(const Blocks& tb, const xyo::Block* b,
                      const std::unordered_set<std::string>& vars,
                      const std::unordered_set<std::string>& lists,
                      int depth = 0) {
    if (!b || depth > 8) return false;
    const std::string& op = b->opcode;
    if (op == "operator_add" || op == "operator_subtract" ||
        op == "operator_multiply" || op == "operator_divide" ||
        op == "operator_mod" || op == "operator_round" ||
        op == "operator_mathop" || op == "operator_length")
        return true;
    if (op == "math_number" || op == "math_integer" ||
        op == "math_whole_number" || op == "math_positive_number" ||
        op == "math_angle") {
        auto it = b->fields.find("NUM");
        if (it == b->fields.end()) return false;
        // project.json では NUM も文字列。フロントは数値に変換して const に
        // するので、数値として読めるものは数値式として扱う。
        if (it->second.value.isNumber()) return true;
        return it->second.value.isString() &&
               !std::isnan(xyo::cast::toNumberOrNaN(it->second.value));
    }
    if (op == "data_variable") {
        auto it = b->fields.find("VARIABLE");
        return it != b->fields.end() && vars.count(it->second.id) != 0;
    }
    // data_itemoflist は numericLists に入っていても数値確定ではない:
    // 証明は「スクリプト内の書き込みが全て数値」だが、ホスト（テスト・
    // モニタ・未コンパイル経路）が実行の合間に文字列を挿入し得る。
    // 'd' 昇格変数の box.f は必ず数値 bits を作るため、文字列を読んだ
    // 値の比較が lexical 比較を失う（numeric promotion テストで検出）。
    if (op == "data_lengthoflist")
        return true;
    if (op == "data_itemnumoflist")
        return true;
    return false;
}

/// 変数とリストの「常に数値」を不動点で求める。
void computeNumericTypes(Runtime& runtime, Target* target, Blocks* blocks,
                         Module& m) {
    static const bool off = [] {
        const char* e = std::getenv("XYO_MLIR_NONUMTYPE");
        return e && e[0] == '1';
    }();
    if (off || !target || !blocks) return;
    std::unordered_set<std::string> sliders;
    for (const auto& [id, rec] : runtime.monitorState.records)
        if (rec.mode == "slider" && rec.opcode == "data_variable")
            sliders.insert(id);
    std::unordered_set<std::string> vars, lists;
    for (const auto& [vid, var] : target->variables) {
        if (var.isCloud || sliders.count(vid)) continue;
        if (var.type == Variable::kListType) {
            lists.insert(vid);
        } else {
            // 初期値が素の数値であること（数値文字列・bool・空文字は不可）。
            if (var.value.isNumber()) vars.insert(vid);
        }
    }
    // 未知の opcode（compat 経路）があるプロジェクトでは安全側に倒す。
    bool unknownOp = false;
    for (Target* t : runtime.targets) {
        if (!t || !t->blocks) continue;
        (void)t->blocks;
    }
    if (unknownOp || (vars.empty() && lists.empty())) return;
    for (int round = 0; round < 8; ++round) {
        std::unordered_set<std::string> badVars, badLists;
        for (Target* t : runtime.targets) {
            if (!t || !t->blocks) continue;
            const Blocks& tb = *t->blocks;
            for (const auto& [bid, blk] : tb.allBlocks()) {
                const std::string& op = blk.opcode;
                if (op == "data_setvariableto" ||
                    op == "data_changevariableby") {
                    auto it = blk.fields.find("VARIABLE");
                    if (it == blk.fields.end() || it->second.id.empty())
                        continue;
                    if (!vars.count(it->second.id)) continue;
                    const xyo::Block* vin =
                        resolveInputBlock(tb, blk, "VALUE");
                    if (!blockNumericExpr(tb, vin, vars, lists)) badVars.insert(it->second.id);
                    continue;
                }
                if (op == "data_addtolist" || op == "data_insertatlist" ||
                    op == "data_replaceitemoflist") {
                    auto it = blk.fields.find("LIST");
                    if (it == blk.fields.end() || it->second.id.empty())
                        continue;
                    if (!lists.count(it->second.id)) continue;
                    const xyo::Block* vin =
                        resolveInputBlock(tb, blk, "ITEM");
                    if (!blockNumericExpr(tb, vin, vars, lists))
                        badLists.insert(it->second.id);
                    continue;
                }
            }
        }
        if (badVars.empty() && badLists.empty()) break;
        for (const auto& id : badVars) vars.erase(id);
        for (const auto& id : badLists) lists.erase(id);
        if (vars.empty() && lists.empty()) return;
    }
    if (getenv("XYO_MLIR_NUMDBG")) {
        std::fprintf(stderr, "[mlir-num] vars=%zu lists=%zu\n",
                     vars.size(), lists.size());
        for (const auto& id : lists)
            std::fprintf(stderr, "[mlir-num] list %s\n", id.c_str());
    }
    m.numericVars = std::move(vars);
    m.numericLists = std::move(lists);
    // list.ptr op に印を付ける（lowering がタグ検査を省く）。
    // intvalued=1 は要素が全て整数（NaN なし）と証明できた場合に付け、
    // lowering が NaN 正規化を省くのに使う。
    for (auto& f : m.functions()) {
        if (!f.body) continue;
        std::function<void(Block&)> walk = [&](Block& blk) {
            for (Op* o : blk.ops) {
                if (o->name == "xyo.list.ptr") {
                    const std::string id(o->attrStr("id"));
                    if (m.numericLists.count(id))
                        o->attrs["numeric"] = Attr::integer(1);
                    if (m.intValuedLists.count(id))
                        o->attrs["intvalued"] = Attr::integer(1);
                }
                for (auto& r : o->regions)
                    if (r)
                        for (auto& cb : r->blocks) walk(*cb);
            }
        };
        walk(*f.body);
    }
}

} // namespace

void computeIntEligibility(Runtime& runtime, Target* target, Blocks* blocks,
                           Module& m) {
    static const bool noInt = [] {
        const char* e = std::getenv("XYO_MLIR_NOINT");
        return e && e[0] == '1';
    }();
    // Stage 変数も対象にする（スプライトから書けるので、書き込み走査は
    // 全ターゲットのブロックを見る）。
    if (noInt || !target || !blocks) return;
    // slider 監視の変数は UI スレッドが concurrent に書く。
    std::unordered_set<std::string> sliders;
    for (const auto& [id, rec] : runtime.monitorState.records) {
        if (rec.mode == "slider" && rec.opcode == "data_variable")
            sliders.insert(rec.id);
    }
    for (const auto& [vid, var] : target->variables) {
        if (var.isCloud) continue;
        if (sliders.count(vid)) continue;
        // 初期値も整数でなければ整数輸送できない（yield 後に他スクリプトが
        // `change by 1` した結果が非整数になり得るため）。数値の整数また
        // は正準形の整数文字列だけを許す（"3.5"・"abc"・"" は不可）。
        int64_t init = 0;
        if (!intLiteralBits(var.value.bits, &init)) continue;
        m.intEligibleVars.insert(vid);
    }
    if (m.intEligibleVars.empty()) return;
    if (getenv("XYO_MLIR_INTDBG")) {
        size_t nblocks = blocks->allBlocks().size();
        size_t nwrites = 0, nav = 0;
        for (const auto& [bid, blk] : blocks->allBlocks()) {
            if (blk.opcode == "data_setvariableto" ||
                blk.opcode == "data_changevariableby") {
                ++nwrites;
                auto it = blk.fields.find("VARIABLE");
                if (it != blk.fields.end() && !it->second.id.empty()) ++nav;
            }
        }
        std::fprintf(stderr, "[mlir-int] eligible=%zu nonlit=%zu blocks=%zu writes=%zu withid=%zu\n",
                     m.intEligibleVars.size(), m.intNonLiteralWrites.size(),
                     nblocks, nwrites, nav);
    }
    // 全ブロック走査: 非リテラル書き込みを変数ごとに数える。
    // （promoter がループ内の内訳と突き合わせ、ループ外の非整数書き込みが
    // あれば整数化しない）
    // 書き込みの整数性は clean fixpoint で判定する: 全書き込みが
    // リテラル整数・四則・mod・round・clean 変数読みから成る変数だけが
    // clean に残る。clean 変数への書き込みは実行順序・スレッド交錯に
    // よらず整数を保つ（初期値整数＋各書き込みが整数保存）ため数えない。
    // 変数 id はプロジェクト全体で一意なので、全ターゲットのブロックを
    // 走査すれば「誰が書くか」を正しく数えられる。
    auto writeIsCleanInt =
        [&](const Blocks& tb, const xyo::Block& blk,
            const std::unordered_set<std::string>& cleanVars,
            const std::unordered_map<std::string, std::pair<int64_t, int64_t>>& cleanLists) {
            auto it = blk.fields.find("VARIABLE");
            if (it == blk.fields.end() || it->second.id.empty()) return true;
            const xyo::Block* vin = resolveInputBlock(tb, blk, "VALUE");
            if (!vin ||
                !blockIntExpr(tb, vin, cleanVars, cleanLists))
                return false;
            // change は x = x + delta に展開されるため、書き込み先自身も
            // clean でなければならない。
            if (blk.opcode == "data_changevariableby")
                return cleanVars.count(it->second.id) != 0;
            return true;
        };
    // リストの整数レンジ: 初期要素と全ストアが整数リテラルのリストだけ
    // [lo,hi] を持つ。範囲外読みは ""→0 になるので 0 で広げる。
    // 空リスト＋ストアなしは [0,0]（常に範囲外→常に 0）。
    // 変数読みのストアがあるリストは対象外（範囲が分からない）。
    // 単一パスで求まる（リテラルは自己完結し、変数側への依存は無い）。
    // XYO_MLIR_NOITEMRANGE=1 で無効化（A/B 用）。
    static const bool noItemRange = [] {
        const char* e = std::getenv("XYO_MLIR_NOITEMRANGE");
        return e && e[0] == '1';
    }();
    if (!noItemRange)
    for (Target* t : runtime.targets) {
        if (!t) continue;
        for (const auto& [vid, var] : t->variables) {
            if (var.type != Variable::kListType || !var.list) continue;
            if (var.isCloud) continue;
            bool ok = true;
            int64_t lo = 0, hi = 0;
            for (uint32_t i = 0; i < var.list->len; ++i) {
                int64_t c = 0;
                if (!intLiteralBits(var.list->items[i].bits, &c)) {
                    ok = false;
                    break;
                }
                if (i == 0) {
                    lo = hi = c;
                } else {
                    lo = std::min(lo, c);
                    hi = std::max(hi, c);
                }
            }
            if (ok)
                m.intListRanges[vid] = {std::min<int64_t>(lo, 0),
                                       std::max<int64_t>(hi, 0)};
        }
    }
    for (Target* t : runtime.targets) {
        if (!t || !t->blocks) continue;
        const Blocks& tb = *t->blocks;
        for (const auto& [bid, blk] : tb.allBlocks()) {
            const std::string& op = blk.opcode;
            if (op != "data_addtolist" && op != "data_insertatlist" &&
                op != "data_replaceitemoflist")
                continue;
            auto it = blk.fields.find("LIST");
            if (it == blk.fields.end() || it->second.id.empty()) continue;
            auto lr = m.intListRanges.find(it->second.id);
            if (lr == m.intListRanges.end()) continue;
            const xyo::Block* vin = resolveInputBlock(tb, blk, "ITEM");
            int64_t c = 0;
            // リテラル整数（math_number 等）のみ exact を保つ。
            if (vin && vin->opcode != "data_variable" &&
                blockIntLiteral(vin) &&
                intLiteralBits(
                    vin->fields.find("NUM")->second.value.bits, &c)) {
                lr->second.first = std::min(lr->second.first, c);
                lr->second.second = std::max(lr->second.second, c);
                lr->second.first = std::min<int64_t>(lr->second.first, 0);
                lr->second.second = std::max<int64_t>(lr->second.second, 0);
            } else {
                m.intListRanges.erase(it->second.id);
            }
        }
    }
    std::unordered_set<std::string> cleanVars = m.intEligibleVars;
    // for_each の添字代入は要素型が不定（文字列があり得る）なので、
    // clean には入れない（読み側の信頼を広げない。数え方は従来どおり）。
    for (Target* t : runtime.targets) {
        if (!t || !t->blocks) continue;
        for (const auto& [bid, blk] : t->blocks->allBlocks()) {
            if (blk.opcode != "control_for_each") continue;
            auto it = blk.fields.find("VARIABLE");
            if (it != blk.fields.end() && !it->second.id.empty())
                cleanVars.erase(it->second.id);
        }
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (Target* t : runtime.targets) {
            if (!t || !t->blocks) continue;
            const Blocks& tb = *t->blocks;
            for (const auto& [bid, blk] : tb.allBlocks()) {
                const std::string& op = blk.opcode;
                if (op == "data_setvariableto" ||
                    op == "data_changevariableby") {
                    auto it = blk.fields.find("VARIABLE");
                    if (it == blk.fields.end() || it->second.id.empty())
                        continue;
                    if (!cleanVars.count(it->second.id)) continue;
                    if (!writeIsCleanInt(tb, blk, cleanVars, m.intListRanges)) {
                        if (getenv("XYO_MLIR_INTDBG"))
                            std::fprintf(stderr,
                                         "[mlir-int] unclean var %s by %s\n",
                                         it->second.id.c_str(),
                                         blk.opcode.c_str());
                        cleanVars.erase(it->second.id);
                        changed = true;
                    }
                    continue;
                }
            }
        }
    }
    if (getenv("XYO_MLIR_INTDBG")) {
        std::string vs, ls, iv;
        for (const auto& id : cleanVars) { vs += id; vs += " "; }
        for (const auto& [id, box] : m.intListRanges) {
            ls += id + "[" + std::to_string(box.first) + "," +
                  std::to_string(box.second) + "] ";
        }
        for (const auto& id : m.intValuedLists) { iv += id; iv += " "; }
        std::fprintf(stderr, "[mlir-int] clean vars=[%s] lists=[%s] intValued=[%s]\n",
                     vs.c_str(), ls.c_str(), iv.c_str());
    }
    for (Target* t : runtime.targets) {
        if (!t || !t->blocks) continue;
        const Blocks& tb = *t->blocks;
        for (const auto& [bid, blk] : tb.allBlocks()) {
            const std::string& op = blk.opcode;
            const char* field = nullptr;
            if (op == "data_setvariableto" || op == "data_changevariableby")
                field = "VARIABLE";
            else if (op == "control_for_each")
                field = "VARIABLE";
            else
                continue;
            auto it = blk.fields.find(field);
            if (it == blk.fields.end() || it->second.id.empty()) continue;
            // for_each の添字代入は保守的に数えない（従来どおり）。
            // 数えるのは「非整数になり得る書き込み」だけ。
            if (op == "control_for_each") continue;
            if (!writeIsCleanInt(tb, blk, cleanVars, m.intListRanges))
                m.intNonLiteralWrites[it->second.id]++;
        }
    }
    // 整数値のみを持つリスト（NaN チェック省略用）。
    // 初期要素が全て整数リテラルかつ全ストアが整数証明可能なら登録。
    // intListRanges より緩い（変数 append を許す、範囲は追跡しない）。
    for (Target* t : runtime.targets) {
        if (!t) continue;
        for (const auto& [vid, var] : t->variables) {
            if (var.type != Variable::kListType || !var.list) continue;
            if (var.isCloud) continue;
            bool ok = true;
            for (uint32_t i = 0; i < var.list->len; ++i) {
                int64_t c = 0;
                if (!intLiteralBits(var.list->items[i].bits, &c)) {
                    ok = false;
                    break;
                }
            }
            if (ok) m.intValuedLists.insert(vid);
            if (getenv("XYO_MLIR_INTDBG"))
                std::fprintf(stderr, "[mlir-int] intValued init %s: %d\n",
                             vid.c_str(), (int)ok);
        }
    }
    for (Target* t : runtime.targets) {
        if (!t || !t->blocks) continue;
        const Blocks& tb = *t->blocks;
        for (const auto& [bid, blk] : tb.allBlocks()) {
            const std::string& op = blk.opcode;
            if (op != "data_addtolist" && op != "data_insertatlist" &&
                op != "data_replaceitemoflist")
                continue;
            auto it = blk.fields.find("LIST");
            if (it == blk.fields.end() || it->second.id.empty()) continue;
            if (m.intValuedLists.count(it->second.id) == 0) continue;
            const xyo::Block* vin = resolveInputBlock(tb, blk, "ITEM");
            if (!vin || !blockIntExpr(tb, vin, cleanVars, m.intListRanges)) {
                // 整数証明できない書き込みがあれば除外（保守的）。
                if (getenv("XYO_MLIR_INTDBG") && m.intValuedLists.count(it->second.id))
                    std::fprintf(stderr, "[mlir-int] intValued erase %s by %s\n",
                                 it->second.id.c_str(), op.c_str());
                m.intValuedLists.erase(it->second.id);
            }
        }
    }
    if (getenv("XYO_MLIR_INTDBG")) {
        std::string iv2;
        for (const auto& id : m.intValuedLists) { iv2 += id; iv2 += " "; }
        std::fprintf(stderr, "[mlir-int] intValued final=[%s]\n", iv2.c_str());
    }
    m.intCleanVars = std::move(cleanVars);
}

} // namespace

void setForceEnabled(int value) { g_force.store(value, std::memory_order_relaxed); }

namespace {
/// ユーザーが `XYO_MLIR` / `XYO_JIT_MLIR` を明示したか。
bool explicitMlirEnv() {
    return std::getenv("XYO_MLIR") != nullptr ||
           std::getenv("XYO_JIT_MLIR") != nullptr;
}
/// Mega プリセットが有効か（`xyo run`/`xyo test` の既定）。
}  // namespace

bool isEnabled() {
    const int forced = g_force.load(std::memory_order_relaxed);
    if (forced >= 0) return forced != 0;
    // 非 Mega の Eager ティアでは既定で MLIR を使わない。Mega プリセット
    // 下は compat 層の yield 標記修正（general_bcastwait）で既定 ON。
    // 明示的に `XYO_MLIR`/`XYO_JIT_MLIR` を渡した場合は従う。
    if (!explicitMlirEnv() && jitMode() == JitMode::Eager &&
        !megaPresetRequested())
        return false;
    const char* e = std::getenv("XYO_MLIR");
    if (!e) e = std::getenv("XYO_JIT_MLIR");
    // 既定で有効。`XYO_MLIR=0`（または `XYO_JIT_MLIR=0`）で従来の
    // codegen に戻せる。未対応 opcode・検証失敗・lowering 失敗では
    // tryEmitScript が false を返し、従来経路がそのまま走る
    // （フォールバックは常に安全側）。
    if (!e) return true;
    return truthful(e);
}

uint64_t compiledScripts() {
    return g_compiled.load(std::memory_order_relaxed);
}
uint64_t fallbackScripts() {
    return g_fallback.load(std::memory_order_relaxed);
}

std::string lastDump() {
    std::lock_guard<std::mutex> lock(g_dumpMutex);
    return g_lastDump;
}

bool tryEmitScript(llvm::Module& mod, llvm::LLVMContext& lc,
                   Runtime& runtime, Target* target, Blocks* blocks,
                   const std::string& topBlockId,
                   const std::string& entryName, CompiledScript& out,
                   JitPinned& pinned,
                   std::vector<std::string>& pendingFnNames,
                   std::map<std::string, std::string>& procNameMap) {
    if (!isEnabled()) return false;
    if (!blocks || !target) return false;
    // ブロック数カウント実行（xyo test の assert 等）は既存 codegen が
    // 専用のカウンタを emit するため、ここでは扱わない。
    if (runtime.blockCountEnabled) {
        g_fallback.fetch_add(1, std::memory_order_relaxed);
        trace("skip %s: block counting enabled", topBlockId.c_str());
        return false;
    }
    // Guarded 変数解決（実験）は「使用のたびに owner/世代を検証する」
    // 契約なので、スロットを直接 load するこの経路では扱わない。
    if (const char* guard = std::getenv("XYO_RT_GUARD_NATIVE_VAR_SLOTS");
        guard && guard[0] == '1') {
        g_fallback.fetch_add(1, std::memory_order_relaxed);
        trace("skip %s: guarded native variable slots", topBlockId.c_str());
        return false;
    }
    // stackless coroutine lowering は既存関数の形を前提に書き換えるため、
    // 同時に有効化されているときは既存経路へ譲る。
    if (std::getenv("XYO_JIT_CORO_DIRECT")) {
        g_fallback.fetch_add(1, std::memory_order_relaxed);
        trace("skip %s: XYO_JIT_CORO_DIRECT is set", topBlockId.c_str());
        return false;
    }

    const xyo::Block* top = blocks->getBlock(topBlockId);
    Module m;
    // green flag ハットは実行ごとに全スレッド停止後の単一起動になるため、
    // 同一スクリプト関数の多重実行が無い。入口ガード版の解析が同一関数
    // 内の外書きを許す条件に使う（broadcast/clone 等は多重起動し得る）。
    m.singleSpawn = top && top->opcode == "event_whenflagclicked";
    computeIntEligibility(runtime, target, blocks, m);
    const StringPinner pin = [&](const xyo::Value& lit) -> uint64_t {
        // 既存の SStr を pin してポインタ同一性を保つ。非文字列 Value が
        // 渡った場合はビット列をそのまま返す（呼出し側が文字列のみ渡す
        // 前提だが、防御としてそのまま返しておく）。
        if (lit.isString()) {
            SStr* s = lit.asStrPtr();
            if (s) {
                pinned.strings.push_back(s);
                return lit.bits;
            }
            return lit.bits; // kEmptyStr はポインタ不要のシングルトン
        }
        return lit.bits;
    };
    // スタックレス・コルーチン化の候補なら、backend の stackless::lower が
    // 受理できる IR 形状で emit する（legacy codegen の
    // stacklessNormalLoop と同じ判断）。判定は backend と同じ条件に揃える。
    static const bool blockCounting = [] {
        const char* e = std::getenv("XYO_BLOCK_COUNT");
        return e && e[0] == '1';
    }();
    const bool coroCandidate =
        top && !blockCounting && !std::getenv("XYO_JIT_CALLFRAME") &&
        !std::getenv("XYO_PROF") &&
        (stackless::eligible(*blocks, *top) ||
         stackless::generalCandidate(*blocks, *top));
    BuildOptions buildOpts;
    buildOpts.coroMode = coroCandidate;
    std::map<std::string, std::string> procNames;
    const BuildOutcome built = buildScriptFromBlocks(
        m, *blocks, topBlockId, entryName, pin, &procNames, &buildOpts);
    if (!built.ok) {
        g_fallback.fetch_add(1, std::memory_order_relaxed);
        trace("fallback %s: %s", scriptLabel(topBlockId, top).c_str(),
              built.reason.c_str());
        return false;
    }

    // 型推論は IR 構築後に行い、list.ptr に印を付ける（lowering 用）。
    computeNumericTypes(runtime, target, blocks, m);
    std::vector<Diagnostic> errors;
    if (!verify(m, &errors)) {
        g_fallback.fetch_add(1, std::memory_order_relaxed);
        trace("fallback %s: verifier failed (%s)", topBlockId.c_str(),
              errors.empty() ? "?" : errors.front().message.c_str());
        return false;
    }
    PassStats stats;
    runStandardPipeline(m, &stats);
    if (!verify(m, &errors)) {
        g_fallback.fetch_add(1, std::memory_order_relaxed);
        trace("fallback %s: verifier failed after passes (%s)",
              topBlockId.c_str(),
              errors.empty() ? "?" : errors.front().message.c_str());
        if (getenv("XYO_MLIR_INTDBG")) {
            for (auto& e : errors)
                std::fprintf(stderr, "[mlir-int-verify] %s\n", e.message.c_str());
            std::fprintf(stderr, "%s\n", m.print().c_str());
        }
        return false;
    }

    // pass がコンパイル時に作った文字列は JIT が使うので GC から守る。
    for (SStr* s : m.pinnedStrings) pinned.strings.push_back(s);

    const std::string text = m.print();
    if (const char* dump = std::getenv("XYO_MLIR_DUMP"); truthful(dump)) {
        std::lock_guard<std::mutex> lock(g_dumpMutex);
        g_lastDump = text;
        if (std::strcmp(dump, "1") != 0 && std::strcmp(dump, "on") != 0) {
            if (std::FILE* f = std::fopen(dump, "a")) {
                std::fputs(text.c_str(), f);
                std::fputc('\n', f);
                std::fclose(f);
            }
        } else {
            std::fputs(text.c_str(), stderr);
            std::fputc('\n', stderr);
        }
    }

    const LoweredFunction lowered =
        lowerToLLVM(m, mod, lc, entryName, coroCandidate);
    if (!lowered.ok || !lowered.fn || !lowered.error.empty()) {
        g_fallback.fetch_add(1, std::memory_order_relaxed);
        trace("fallback %s: lowering failed (%s)", topBlockId.c_str(),
              lowered.error.c_str());
        return false;
    }
    if (const char* llvmDump = std::getenv("XYO_MLIR_LLVM"); llvmDump) {
        // "1" でモジュール全体、それ以外は関数名の部分一致フィルタ。
        if (llvmDump[0] == '1' && llvmDump[1] == '\0') {
            mod.print(llvm::errs(), nullptr);
        } else if (llvmDump[0] != '\0') {
            const std::string want(llvmDump);
            for (llvm::Function& f : mod)
                if (f.getName().contains(want)) f.print(llvm::errs());
        }
    }
    // 生成した IR が壊れていたら関数ごと取り下げる（モジュールには
    // 触れていないので、既存 codegen が同じ名前で作り直せる）。
    // 手続き関数も検査する — entry だけだと callee 側の不正 IR
    // （typed な icmp/to_num(double) 等）が残り、JIT の O2 パイプライン
    // （IPSCCP 等）内でクラッシュした実績がある。
    {
        std::vector<llvm::Function*> toCheck{lowered.fn};
        for (const auto& [vkey, fname] : procNames)
            if (llvm::Function* f = mod.getFunction(fname))
                toCheck.push_back(f);
        std::string verifyError;
        llvm::Function* bad = nullptr;
        {
            llvm::raw_string_ostream os(verifyError);
            for (llvm::Function* f : toCheck)
                if (llvm::verifyFunction(*f, &os)) { bad = f; break; }
        }
        if (bad) {
            if (std::getenv("XYO_MLIR_LLVM_ERR"))
                bad->print(llvm::errs());
            // 名前は erase の前に控える — eraseFromParent() 後は
            // llvm::Value::getName() が解放済みメモリを読む。
            const std::string badName = bad->getName().str();
            for (llvm::Function* f : toCheck)
                if (f->getParent()) f->eraseFromParent();
            g_fallback.fetch_add(1, std::memory_order_relaxed);
            trace("fallback %s: invalid LLVM function %s (%s)",
                  topBlockId.c_str(), badName.c_str(),
                  verifyError.c_str());
            return false;
        }
    }

    pendingFnNames.push_back(entryName);
    // 手続き関数は backend がシンボル解決して CompiledScript::procedures に
    // 束縛する（vkey は既存 codegen と同じ proccode + "|w"/"|n"）。
    for (const auto& [vkey, fname] : procNames) {
        procNameMap[vkey] = fname;
        pendingFnNames.push_back(fname);
    }
    out.entry = nullptr;  // JIT アドレス解決後に埋める（既存経路と同じ）
    // compat/clone 系ヘルパを emit した場合は既存 codegen と同じく
    // caller スレッド affinity を要求する（worker で走らせない）。
    out.mainThread = lowered.mainThread;
    out.mayYield = lowered.mayYield;
    // 予算 watchdog だけで譲る（warp ループのみ）なら、legacy と同じく
    // 「協調的 yield のみ」として同期バッチに参加できる。
    static const bool coopOn = [] {
        const char* e = std::getenv("XYO_MLIR_NOCOOP");
        return !(e && e[0] == '1');
    }();
    out.cooperativeOnly = coopOn && lowered.mayYield &&
                          !lowered.voluntaryYield;
    out.lazy = false;
    out.shortSync = false;
    out.mlirFrontend = true;
    g_compiled.fetch_add(1, std::memory_order_relaxed);
    trace("emit %s: ops=%zu promoted=%llu loops=%llu yield=%d coop=%d guard=%llu int=%llu cse=%llu",
          scriptLabel(topBlockId, top).c_str(), m.opCount(),
          static_cast<unsigned long long>(stats.promotedVars),
          static_cast<unsigned long long>(stats.promotedLoops),
          int(lowered.mayYield), int(out.cooperativeOnly),
          static_cast<unsigned long long>(stats.guardedLoops),
          static_cast<unsigned long long>(stats.intLoops),
          static_cast<unsigned long long>(stats.cse));
    return true;
}

} // namespace xyo::mlir

#endif // XYO_JIT && !__EMSCRIPTEN__
