// xyo-cpp: ループ本体の GPU 並列実行向けサブセットコンパイラ。
// 仕様は gpu_compile.hpp のコメント参照。
//
// 反復間独立性の静的証明は emit 済みバイトコード上の CFG 固定点
// 解析（gpu_analysis.cpp）が担う — 本ファイルは lowering のみ。
//   変数: 本体で書かれる変数は「読み取り前に必ず代入済み」(definite)
//         か affine 再帰（preheader で iter*step 初期化）のみ受理。
//         一度も書かれない変数は読み専用スナップショット。
//   位置: x/y は反復を跨いで持ち越される。stroke/読み取りは
//         反復内設定済みか本体未書込み（一様）を要求。
//   ペン: penUp/penDown が存在し本体が down で終わり得る場合、
//         stroke 起点を観測する全移動が反復内 penUp/penDown 以後
//         であることを要求（pdSet）。全パス penUp 終了なら従来通り
//         lanes>0 は penUp 開始で正しい。pen op が無い本体は
//         実行側の penDown ガードで保守的に扱う。
//   制御: 内側の while/until/for_each/all_at_once も同一 VM の
//         制御フローへ lowering（レーン内は逐次）。stop/トップレベル
//         return は以降の全反復を止める直列意味論のため常に不可。
#include "gpu_compile.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "../core/cast.hpp"
#include "../core/jsnum.hpp"
#include "../core/sstr.hpp"
#include "gpu_analysis.hpp"

namespace xyo::gpu {

namespace {

constexpr int kMaxSlots = 96;    // シェーダ slots[] と一致
constexpr int kMaxWb = 128;      // シェーダ wbMask[4] と一致
constexpr int kMaxEmit = 65536;  // emit 命令数ガード
constexpr int kMaxTraverse = 200000; // 走査ガード

// リテラルとして受理する fields-only ブロック（文字列意味論を
// 要するものは拒否 → CPU フォールバック）。
bool literalOpcode(const std::string& op) {
    // 数値リテラルのみ — text は cast::toNumber("abc")=0 で
    // Scratch の文字列比較/真理値と等価にならないため拒否
    // （textLiteralIsNumber を通るものだけ別途受理する）。
    return op == "math_number" || op == "math_whole_number" ||
           op == "math_positive_number" || op == "math_integer" ||
           op == "math_angle" || op == "colour_picker";
}

/// text リテラルを数値として受理してよいか。数値化した結果を ES 表記へ
/// 戻した文字列が元と完全一致する場合のみ true（"1" は可、"-0"/"01"/
/// "1.0"/"abc" は不可）。この条件では文字列表現が数値の toString と
/// 一致するため、Scratch の比較・真理値・join・length が観測等価になる。
bool textLiteralIsNumber(const Block* b) {
    for (auto& [k, f] : b->fields) {
        (void)k;
        const double d = cast::toNumber(f.value);
        if (!std::isfinite(d)) return false;
        SStr* s = cast::toString(f.value);
        return s && jsnum::toString16(d) == std::u16string(s->view());
    }
    return false;
}

struct Compiler {
    Blocks* blocks;
    Program prog;
    bool ok = true;
    std::string why;

    // lane ローカル変数スロット（変数 id または "__argN"/"__tN" 等の内部名）
    std::unordered_map<std::string, uint32_t> slots;
    // slot → 変数 (id, name)（induction スナップショット用）
    std::unordered_map<uint32_t, std::pair<std::string, std::u16string>>
        slotVar;
    std::vector<char> slotStored;      // 実際に STOREV されるスロット
    // 読み専用変数 id → globals[] index
    std::unordered_map<std::string, uint32_t> globals;
    // writeback 変数 id → wbVars index
    std::unordered_map<std::string, uint32_t> wbMap;
    // 本体全体で書き込まれる変数 id（事前走査）
    std::unordered_set<std::string> written;
    BodyScan scan;

    // argument_reporter が参照する現在の引数名 → スロット
    std::unordered_map<std::string, uint32_t> argSlots;
    // proc インラインの再帰ガードと return 先ラベル
    std::unordered_set<std::string> inlining;
    std::vector<uint32_t> retPatches;
    int procDepth = 0;
    int tempN = 0;
    int depth = 0;              // 再帰深度ガード（expr/substack/proc）
    int scanDepth = 0;          // collectWritten 系の再帰ガード
    // f32 丸めが起きたソースリテラル PUSH の instruction index —
    // analyzeRecurrence へ渡して厳密整数扱いを止める
    std::vector<uint32_t> roundedPushes;

    // レコード推定数（maxPerLane の算出用）
    uint32_t estRecords = 0;

    void fail(std::string s) {
        if (ok) { ok = false; why = std::move(s); }
    }
    static std::string fieldStr(const BlockField* f) {
        if (!f || !f->value.isString()) return {};
        return SStr::toUtf8(f->value.asStrPtr()->view());
    }
    static double hexColor(const std::string& s) {
        if (s.size() != 7 || s[0] != '#') return -1;
        char* end = nullptr;
        long v = std::strtol(s.c_str() + 1, &end, 16);
        return (end && *end == 0) ? double(v) : -1;
    }
    void pushLiteral(const Block* s) {
        for (auto& [k, f] : s->fields) {
            (void)k;
            if (s->opcode == "colour_picker") {
                double d = hexColor(fieldStr(&f));
                emitF(OP_PUSH, float(d < 0 ? 0 : d));
                return;
            }
            double d = cast::toNumber(f.value);
            if (!std::isfinite(d)) {
                fail("non-numeric literal in numeric context");
                return;
            }
            emitSourceLiteral(d);
            return;
        }
        emitF(OP_PUSH, 0.0f);
    }

    uint32_t slotFor(const std::string& key) {
        // ユーザ変数スロットは prog.nSlots から連番確保 —
        // slots マップの内部一時キーと衝突しない。
        auto [it, ins] = slots.emplace(key, 0u);
        if (ins) it->second = prog.nSlots++;
        return it->second;
    }
    uint32_t tempSlot() {
        uint32_t s = prog.nSlots++;
        ++tempN;
        return s;
    }
    void noteStore(uint32_t slot) {
        if (slotStored.size() <= slot) slotStored.resize(slot + 1);
        slotStored[slot] = 1;
    }
    uint32_t globalFor(const std::string& id, const BlockField* f) {
        auto [it, ins] = globals.emplace(id, uint32_t(globals.size()));
        if (ins)
            prog.globals.push_back(
                {id, f ? std::u16string(f->value.isString()
                                            ? f->value.asStrPtr()->view()
                                            : std::u16string_view{})
                       : std::u16string{}});
        return it->second;
    }
    uint32_t wbFor(const std::string& id, const BlockField* f) {
        auto [it, ins] = wbMap.emplace(id, uint32_t(prog.wbVars.size()));
        if (ins) {
            std::u16string name;
            if (f && f->value.isString() && f->value.asStrPtr())
                name = std::u16string(f->value.asStrPtr()->view());
            uint32_t s = slotFor(id);
            slotVar[s] = {id, name};
            prog.wbVars.push_back({id, name, s,
                                   uint32_t(prog.wbVars.size())});
        }
        return it->second;
    }

    void emit(uint32_t op, uint32_t operand = 0) {
        prog.code.push_back(op);
        prog.code.push_back(operand);
        if (prog.code.size() / 2 > size_t(kMaxEmit)) fail("code size cap");
        switch (op) {
        case OP_STOREV: case OP_STOREVM: noteStore(operand & 0xFFFFu); break;
        case OP_GOTOXY: case OP_SETX: case OP_SETY:
        case OP_CHANGEX: case OP_CHANGEY: case OP_PENDOWN:
            ++estRecords; break;
        default: break;
        }
    }
    // ソースリテラル由来 PUSH — f32 へ丸まった場合は roundedPushes
    // に記録し、再帰証明で厳密整数扱いしない。
    void emitSourceLiteral(double d) {
        float f = float(d);
        if (!std::isfinite(f)) { fail("non-finite literal"); return; }
        if (double(f) != d)
            roundedPushes.push_back(uint32_t(prog.code.size() / 2));
        emitF(OP_PUSH, f);
    }
    void emitF(uint32_t op, float f) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        emit(op, u);
    }
    uint32_t pc() const { return uint32_t(prog.code.size() / 2); }
    uint32_t emitJmp(uint32_t op) {
        emit(op, 0);
        return uint32_t(prog.code.size() - 1);
    }
    void patch(uint32_t operandPos, uint32_t targetPc) {
        prog.code[operandPos] = targetPc;
    }

    // ---- 変数 -----------------------------------------------------------
    const BlockField* fieldOf(const Block* b, const char* name) {
        auto it = b->fields.find(name);
        return it == b->fields.end() ? nullptr : &it->second;
    }
    bool emitVarLoad(const std::string& id, const BlockField* f) {
        if (written.count(id)) {
            emit(OP_LOADV, slotFor(id));
            slotVar[slotFor(id)] = {
                id, f && f->value.isString() && f->value.asStrPtr()
                        ? std::u16string(f->value.asStrPtr()->view())
                        : std::u16string{}};
        } else {
            emit(OP_LOADG, globalFor(id, f));
        }
        return ok;
    }
    void emitVarStore(const std::string& id, const BlockField* f) {
        uint32_t wb = wbFor(id, f);
        emit(OP_STOREVM, prog.wbVars[wb].slot | (wb << 16));
    }

    // ---- 式 -------------------------------------------------------------
    const Block* blockOf(const BlockInput* in) {
        if (!in || in->block.empty()) return nullptr;
        return blocks->getBlock(in->block);
    }
    const Block* shadowOf(const BlockInput* in) {
        if (!in || in->shadow.empty()) return nullptr;
        return blocks->getBlock(in->shadow);
    }

    bool compileInput(const Block* parent, const char* name) {
        const BlockInput* in = parent->inputs.find(name);
        if (!in) { emitF(OP_PUSH, 0.0f); return true; }
        if (const Block* b = blockOf(in)) return compileExpr(b);
        if (const Block* s = shadowOf(in)) return compileExpr(s);
        emitF(OP_PUSH, 0.0f);
        return true;
    }

    bool compileExpr(const Block* b) {
        if (!b) { emitF(OP_PUSH, 0.0f); return true; }
        if (++depth > 256) { --depth; fail("expr depth limit"); return false; }
        bool r = compileExprInner(b);
        --depth;
        return r;
    }
    bool compileExprInner(const Block* b) {
        const std::string& op = b->opcode;

        if (op == "data_variable") {
            const BlockField* f = fieldOf(b, "VARIABLE");
            if (!f || f->id.empty()) {
                emitF(OP_PUSH, 0.0f);
                return true;
            }
            return emitVarLoad(f->id, f);
        }
        if (op == "argument_reporter_string_number" ||
            op == "argument_reporter_boolean" ||
            op == "argument_reporter") {
            const BlockField* f = fieldOf(b, "VALUE");
            if (f) {
                auto it = argSlots.find(fieldStr(f));
                if (it != argSlots.end()) {
                    emit(OP_LOADV, it->second);
                    return true;
                }
            }
            // インライン中でない引数（GPU 化ループの外側 proc の引数）
            // は表現不可 — 0 を push すると値が違うので失敗にする。
            fail("outer-scope argument");
            return false;
        }
        if (op == "motion_xposition") { emit(OP_POSX); return ok; }
        if (op == "motion_yposition") { emit(OP_POSY); return ok; }

        // 入力名は opcode ごとに異なる:
        //   算術(add/sub/mul/div/mod) = NUM1/NUM2
        //   比較(lt/gt/equals)        = OPERAND1/OPERAND2
        static const struct {
            const char* name; uint32_t op; bool cmp;
        } kBin[] = {
            {"operator_add", OP_ADD, false},
            {"operator_subtract", OP_SUB, false},
            {"operator_multiply", OP_MUL, false},
            {"operator_divide", OP_DIV, false},
            {"operator_mod", OP_MOD, false},
            {"operator_lt", OP_LT, true},
            {"operator_gt", OP_GT, true},
            {"operator_equals", OP_EQ, true},
        };
        for (auto& e : kBin)
            if (op == e.name) {
                const char* a = e.cmp ? "OPERAND1" : "NUM1";
                const char* b2 = e.cmp ? "OPERAND2" : "NUM2";
                if (!compileInput(b, a) || !compileInput(b, b2))
                    return false;
                emit(e.op);
                return ok;
            }
        if (op == "operator_random") {
            if (!compileInput(b, "FROM") || !compileInput(b, "TO"))
                return false;
            emit(OP_RAND);
            return ok;
        }
        if (op == "operator_and") {
            if (!compileInput(b, "OPERAND1") ||
                !compileInput(b, "OPERAND2")) return false;
            emit(OP_AND);
            return ok;
        }
        if (op == "operator_or") {
            if (!compileInput(b, "OPERAND1") ||
                !compileInput(b, "OPERAND2")) return false;
            emit(OP_OR);
            return ok;
        }
        if (op == "operator_not") {
            if (!compileInput(b, "OPERAND")) return false;
            emit(OP_NOT);
            return ok;
        }
        if (op == "operator_round") {
            if (!compileInput(b, "NUM")) return false;
            emit(OP_MATH, M_ROUND);
            return ok;
        }
        if (op == "operator_mathop") {
            const BlockField* f = fieldOf(b, "OPERATOR");
            static const struct { const char* name; uint32_t k; } kM[] = {
                {"abs", M_ABS}, {"floor", M_FLOOR}, {"ceiling", M_CEIL},
                {"sqrt", M_SQRT}, {"sin", M_SIN}, {"cos", M_COS},
                {"tan", M_TAN}, {"asin", M_ASIN}, {"acos", M_ACOS},
                {"atan", M_ATAN}, {"ln", M_LN}, {"log", M_LOG},
                {"e ^", M_EXP}, {"10 ^", M_EXP10},
            };
            uint32_t kind = _M_MAX;
            if (f) {
                std::string fn = fieldStr(f);
                for (auto& e : kM)
                    if (fn == e.name) { kind = e.k; break; }
            }
            if (kind == _M_MAX) { fail("mathop"); return false; }
            if (!compileInput(b, "NUM")) return false;
            emit(OP_MATH, kind);
            return ok;
        }
        // 明示ホワイトリストのリテラル shadow のみ受理。
        // それ以外の reporter/menu は失敗（CPU フォールバック）。
        if (!b->fields.empty() && b->inputs.empty() &&
            (literalOpcode(op) ||
             (op == "text" && textLiteralIsNumber(b)))) {
            pushLiteral(b);
            return ok;
        }
        fail("expr op: " + op);
        return false;
    }

    // ---- 文 -------------------------------------------------------------
    bool compileChain(const std::string& headId) {
        if (++depth > 128) { --depth; fail("nest depth limit"); return false; }
        bool r = compileChainInner(headId);
        --depth;
        return r;
    }
    bool compileChainInner(const std::string& headId) {
        std::string id = headId;
        int guard = 0;
        std::unordered_set<std::string> seen;
        while (ok && !id.empty()) {
            if (++guard > kMaxTraverse || !seen.insert(id).second) {
                fail("chain cycle");
                return false;
            }
            const Block* b = blocks->getBlock(id);
            if (!b) { fail("block missing"); return false; }
            if (!compileStmt(b)) return false;
            id = b->next;
        }
        return ok;
    }

    bool compileSubstack(const Block* b, const char* name) {
        const BlockInput* in = b->inputs.find(name);
        if (!in || in->block.empty()) return true;
        return compileChain(in->block);
    }

    // COLOR_PARAM メニューから attr index (0..3) を解決。失敗 -1。
    int colorParamOf(const Block* b) {
        const BlockInput* mi = b->inputs.find("COLOR_PARAM");
        if (!mi) return -1;
        const Block* m = blockOf(mi);
        if (!m) m = shadowOf(mi);
        if (!m || m->fields.empty()) return -1;
        std::string v = fieldStr(&m->fields.begin()->second);
        if (v == "color") return 0;
        if (v == "saturation") return 1;
        if (v == "brightness") return 2;
        if (v == "transparency") return 3;
        return -1;
    }

    bool compileStmt(const Block* b) {
        const std::string& op = b->opcode;

        if (op == "data_setvariableto") {
            const BlockField* f = fieldOf(b, "VARIABLE");
            if (!f || f->id.empty()) { fail("setvar no id"); return false; }
            if (!compileInput(b, "VALUE")) return false;
            emitVarStore(f->id, f);
            return ok;
        }
        if (op == "data_changevariableby") {
            const BlockField* f = fieldOf(b, "VARIABLE");
            if (!f || f->id.empty()) { fail("changevar no id"); return false; }
            if (!emitVarLoad(f->id, f)) return false;   // 読み+書き
            if (!compileInput(b, "VALUE")) return false;
            emit(OP_ADD);
            emitVarStore(f->id, f);
            return ok;
        }
        if (op == "motion_gotoxy") {
            if (!compileInput(b, "X") || !compileInput(b, "Y")) return false;
            emit(OP_GOTOXY);
            return ok;
        }
        if (op == "motion_setx") {
            if (!compileInput(b, "X")) return false;
            emit(OP_SETX);
            return ok;
        }
        if (op == "motion_sety") {
            if (!compileInput(b, "Y")) return false;
            emit(OP_SETY);
            return ok;
        }
        if (op == "motion_changexby") {
            if (!compileInput(b, "DX")) return false;
            emit(OP_CHANGEX);
            return ok;
        }
        if (op == "motion_changeyby") {
            if (!compileInput(b, "DY")) return false;
            emit(OP_CHANGEY);
            return ok;
        }
        if (op == "pen_penDown") { emit(OP_PENDOWN); return ok; }
        if (op == "pen_penUp") { emit(OP_PENUP); return ok; }
        if (op == "pen_setPenSizeTo") {
            if (!compileInput(b, "SIZE")) return false;
            emit(OP_PENSIZE);
            return ok;
        }
        if (op == "pen_changePenSizeBy") {
            if (!compileInput(b, "SIZE")) return false;
            emit(OP_PENSIZED);
            return ok;
        }
        if (op == "pen_setPenColorToColor") {
            if (!compileInput(b, "COLOR")) return false;
            emit(OP_PENCOLN);
            return ok;
        }
        if (op == "pen_setPenColorParamTo" ||
            op == "pen_changePenColorParamBy") {
            int param = colorParamOf(b);
            if (param < 0) { fail("colorParam"); return false; }
            if (!compileInput(b, "VALUE")) return false;
            emit(op == "pen_setPenColorParamTo" ? OP_PENCOLP : OP_PENCOLPB,
                 uint32_t(param));
            return ok;
        }
        if (op == "control_if" || op == "control_if_else") {
            if (!compileInput(b, "CONDITION")) return false;
            uint32_t jzPos = emitJmp(OP_JMPIF0);
            bool r1 = compileSubstack(b, "SUBSTACK");
            if (op == "control_if_else") {
                uint32_t jPos = emitJmp(OP_JMP);
                patch(jzPos, pc());
                bool r2 = compileSubstack(b, "SUBSTACK2");
                patch(jPos, pc());
                return ok && r1 && r2;
            }
            patch(jzPos, pc());
            return ok && r1;
        }
        if (op == "control_repeat") {
            if (!compileInput(b, "TIMES")) return false;
            uint32_t cnt = tempSlot();
            emit(OP_STOREV, cnt);
            uint32_t head = pc();
            emit(OP_LOADV, cnt);
            emitF(OP_PUSH, 0.5f);
            emit(OP_GE);
            uint32_t jzPos = emitJmp(OP_JMPIF0);
            uint32_t est0 = estRecords;
            estRecords = 0;
            bool r = compileSubstack(b, "SUBSTACK");
            // 内側ループのレコード推定 = 本体 × TIMES
            // （リテラルならその値、不明なら 48 で見積もる。
            // 過小評価は実行時の溢れ検出で CPU へフォールバック）
            double times = 48.0;
            if (const BlockInput* ti = b->inputs.find("TIMES")) {
                if (const Block* lit = shadowOf(ti)) {
                    double d = -1;
                    for (auto& [k, f] : lit->fields) {
                        (void)k;
                        d = cast::toNumber(f.value);
                        break;
                    }
                    if (d >= 0 && std::isfinite(d)) times = d;
                }
            }
            estRecords =
                est0 + uint32_t(std::min<double>(
                           estRecords * std::max(0.0, times), 1e6));
            if (!r) return false;
            emit(OP_LOADV, cnt);
            emitF(OP_PUSH, 1.0f);
            emit(OP_SUB);
            emit(OP_STOREV, cnt);
            emit(OP_JMP, head);
            patch(jzPos, pc());
            return ok;
        }
        if (op == "control_while" || op == "control_repeat_until") {
            // 内側ループはレーン内で逐次実行（同一 VM 制御フロー）。
            uint32_t head = pc();
            if (!compileInput(b, "CONDITION")) return false;
            if (op == "control_repeat_until") emit(OP_NOT);
            uint32_t jzPos = emitJmp(OP_JMPIF0);
            bool r = compileSubstack(b, "SUBSTACK");
            emit(OP_JMP, head);
            patch(jzPos, pc());
            return ok && r;
        }
        if (op == "control_for_each") {
            // i=1; while (i <= floor(VALUE)) { var=i; body; i++ }
            // JIT と同じく境界は一度だけ評価。
            const BlockField* f = fieldOf(b, "VARIABLE");
            if (!f || f->id.empty()) { fail("for_each no var"); return false; }
            if (!compileInput(b, "VALUE")) return false;
            emit(OP_MATH, M_FLOOR);
            uint32_t cnt = tempSlot();
            emit(OP_STOREV, cnt);
            uint32_t i = tempSlot();
            emitF(OP_PUSH, 1.0f);
            emit(OP_STOREV, i);
            uint32_t head = pc();
            emit(OP_LOADV, cnt);
            emit(OP_LOADV, i);
            emit(OP_GE);
            uint32_t jzPos = emitJmp(OP_JMPIF0);
            emit(OP_LOADV, i);
            emitVarStore(f->id, f);
            bool r = compileSubstack(b, "SUBSTACK");
            emit(OP_LOADV, i);
            emitF(OP_PUSH, 1.0f);
            emit(OP_ADD);
            emit(OP_STOREV, i);
            emit(OP_JMP, head);
            patch(jzPos, pc());
            return ok && r;
        }
        if (op == "control_all_at_once") {
            return compileSubstack(b, "SUBSTACK");
        }
        if (op == "control_stop") {
            // 直列では以降の全反復が止まる — lane 単位の halt では
            // 再現できないため常に不可。
            fail("stop in gpu repeat");
            return false;
        }
        if (op == "procedures_return") {
            // インライン中は呼出し後へジャンプで表現可能。
            // トップレベルはスクリプト終了 = 以降反復の停止なので不可。
            if (procDepth == 0) {
                fail("top-level return");
                return false;
            }
            retPatches.push_back(emitJmp(OP_JMP));
            return ok;
        }
        if (op == "procedures_call") return compileProcCall(b);

        fail("stmt op: " + op);
        return false;
    }

    bool compileProcCall(const Block* call) {
        if (++depth > 64) { --depth; fail("nest depth limit"); return false; }
        bool r = compileProcCallInner(call);
        --depth;
        return r;
    }
    bool compileProcCallInner(const Block* call) {
        if (!call->mutation) { fail("call no mutation"); return false; }
        const std::string pcode = call->mutation->attr("proccode");
        std::string defId = blocks->getProcedureDefinition(pcode);
        if (defId.empty()) {
            // 未定義呼び出しは引数も評価しない純粋 no-op（プロジェクト
            // 規約 — 削除済み手続きの残骸等）。
            return true;
        }
        if (!inlining.insert(pcode).second) { fail("recursion"); return false; }

        const Block* def = blocks->getBlock(defId);
        std::vector<std::u16string> names;
        std::vector<std::string> ids;
        std::vector<Value> defaults;
        blocks->getProcedureParamNamesIdsAndDefaults(pcode, names, ids,
                                                     defaults);

        // 引数式は「呼出し側」のスコープで評価する — 先に全引数を
        // 一意スロットへ評価してから argSlots を切り替える
        // （fill.draw の引数式が呼出し側 proc の引数を読むため）。
        std::vector<uint32_t> calleeSlots(ids.size());
        for (size_t i = 0; i < ids.size(); ++i) {
            const BlockInput* in = call->inputs.find(ids[i]);
            bool have = in && (!in->block.empty() || !in->shadow.empty());
            if (have) {
                if (!compileInput(call, ids[i].c_str())) return false;
            } else {
                double d = i < defaults.size() ? cast::toNumber(defaults[i])
                                             : 0.0;
                if (!std::isfinite(d)) d = 0;
                emitSourceLiteral(d);
            }
            std::string argName =
                i < names.size() ? SStr::toUtf8(names[i]) : std::string{};
            uint32_t s = tempSlot();
            emit(OP_STOREV, s);
            calleeSlots[i] = s;
        }
        std::unordered_map<std::string, uint32_t> prevArgs;
        prevArgs.swap(argSlots);
        for (size_t i = 0; i < ids.size(); ++i) {
            std::string argName =
                i < names.size() ? SStr::toUtf8(names[i]) : std::string{};
            if (!argName.empty()) argSlots[argName] = calleeSlots[i];
        }
        uint32_t retBase = uint32_t(retPatches.size());
        ++procDepth;
        bool r = !def || def->next.empty() ? true
                                          : compileChain(def->next);
        --procDepth;
        uint32_t endPc = pc();
        for (size_t i = retBase; i < retPatches.size(); ++i)
            patch(retPatches[i], endPc);
        retPatches.resize(retBase);
        inlining.erase(pcode);
        argSlots.swap(prevArgs);
        return ok && r;
    }

    // ---- 事前走査: 本体が書き込む変数 id・ペン操作・属性 ------------------
    void collectWritten(const std::string& headId,
                        std::unordered_set<std::string>& seen) {
        if (++scanDepth > 128) { --scanDepth; fail("scan depth"); return; }
        std::string id = headId;
        int guard = 0;
        while (ok && !id.empty()) {
            if (!ok) break;
            if (++guard > kMaxTraverse || !seen.insert(id).second) break;
            const Block* b = blocks->getBlock(id);
            if (!b) break;
            scanBlock(b, seen);
            id = b->next;
        }
        --scanDepth;
    }
    void scanBlock(const Block* b, std::unordered_set<std::string>& seen) {
        if (!ok) return;
        const std::string& op = b->opcode;
        if (op == "data_setvariableto" || op == "data_changevariableby" ||
            op == "control_for_each") {
            const char* fname =
                op == "control_for_each" ? "VARIABLE" : "VARIABLE";
            if (const BlockField* f = fieldOf(b, fname);
                f && !f->id.empty())
                written.insert(f->id);
        }
        if (op == "pen_penDown" || op == "pen_penUp")
            scan.hasPenOps = true;
        if (op == "pen_setPenSizeTo" || op == "pen_changePenSizeBy")
            scan.writtenAttr[0] = true;
        if (op == "pen_setPenColorToColor")
            scan.writtenAttr[1] = scan.writtenAttr[2] = scan.writtenAttr[3] =
                scan.writtenAttr[4] = true;
        if (op == "pen_setPenColorParamTo" ||
            op == "pen_changePenColorParamBy") {
            int p = colorParamOf(b);
            if (p < 0) for (int k = 1; k < 5; ++k) scan.writtenAttr[k] = true;
            else scan.writtenAttr[p + 1] = true;
        }
        if (op == "motion_gotoxy") scan.writtenX = scan.writtenY = true;
        if (op == "motion_setx" || op == "motion_changexby")
            scan.writtenX = true;
        if (op == "motion_sety" || op == "motion_changeyby")
            scan.writtenY = true;
        for (auto& [iname, in] : b->inputs.items) {
            if (iname == "SUBSTACK" || iname == "SUBSTACK2") {
                if (!in.block.empty()) collectWritten(in.block, seen);
            } else {
                collectWrittenExpr(in.block, seen);
                collectWrittenExpr(in.shadow, seen);
            }
        }
        if (op == "procedures_call" && b->mutation) {
            const std::string pc = b->mutation->attr("proccode");
            std::string defId = blocks->getProcedureDefinition(pc);
            if (!defId.empty() && inlining.insert(pc).second) {
                if (const Block* def = blocks->getBlock(defId))
                    if (!def->next.empty())
                        collectWritten(def->next, seen);
                inlining.erase(pc);
            }
        }
    }
    void collectWrittenExpr(const std::string& bid,
                            std::unordered_set<std::string>& seen) {
        if (!ok) return;
        if (++scanDepth > 128) {
            fail("scan depth");
            --scanDepth;
            return;
        }
        const Block* b = bid.empty() ? nullptr : blocks->getBlock(bid);
        if (!b || !seen.insert(bid).second) { --scanDepth; return; }
        for (auto& [iname, in] : b->inputs.items) {
            (void)iname;
            collectWrittenExpr(in.block, seen);
            collectWrittenExpr(in.shadow, seen);
        }
        --scanDepth;
    }

    // ---- while/until 条件の正規化 -----------------------------------------
    // cond → {counter変数id, cmp(継続条件 counter<op>bound), 不変側入力名,
    //         比較ブロック}
    struct NormCond {
        const Block* cmpBlock = nullptr;
        const BlockField* varField = nullptr;
        std::string varId;
        std::string invInput;         // "OPERAND1" or "OPERAND2"
        int cmp = 0;                  // 1< 2<= 3> 4>=
    };
    const Block* inputBlock(const Block* b, const char* name) {
        const BlockInput* in = b->inputs.find(name);
        if (const Block* x = blockOf(in)) return x;
        if (const Block* x = shadowOf(in)) return x;
        return nullptr;
    }
    // 不変数式の純粋性: リテラル・読み専用スカラ変数・算術/math のみ。
    // random/IO/時間/reporter 副作用は不可。参照した読み専用変数は
    // boundIds へ（実行時型ガード用）。
    bool pureInvariant(const Block* b,
                       std::vector<std::pair<std::string, const BlockField*>>&
                           boundVars,
                       int depth) {
        if (!b || depth > 64) return false;
        const std::string& op = b->opcode;
        if (op == "data_variable") {
            const BlockField* f = fieldOf(b, "VARIABLE");
            if (!f || f->id.empty()) return false;
            if (written.count(f->id)) return false;
            boundVars.push_back({f->id, f});
            return true;
        }
        if (op == "argument_reporter_string_number" ||
            op == "argument_reporter_boolean" ||
            op == "argument_reporter")
            return true;   // JIT が外側スコープで評価（読み専用）
        if (!b->fields.empty() && b->inputs.empty())
            return literalOpcode(op) &&
                   (op != "text" ||
                    [&] {
                        for (auto& [k, f] : b->fields) {
                            (void)k;
                            return std::isfinite(cast::toNumber(f.value));
                        }
                        return true;
                    }());
        static const std::unordered_set<std::string> kPure = {
            "operator_add", "operator_subtract", "operator_multiply",
            "operator_divide", "operator_mod", "operator_round",
            "operator_mathop", "operator_lt", "operator_gt",
            "operator_equals", "operator_and", "operator_or",
            "operator_not",
        };
        if (!kPure.count(op)) return false;
        if (op == "operator_mathop") {
            const BlockField* f = fieldOf(b, "OPERATOR");
            std::string fn = fieldStr(f);
            static const std::unordered_set<std::string> kM = {
                "abs", "floor", "ceiling", "sqrt", "sin", "cos", "tan",
                "asin", "acos", "atan", "ln", "log", "e ^", "10 ^",
            };
            if (!kM.count(fn)) return false;
        }
        for (auto& [iname, in] : b->inputs.items) {
            (void)iname;
            const Block* c = blockOf(&in);
            if (!c) c = shadowOf(&in);
            if (!c) {
                // 未接続入力は 0 扱い — リテラルと同等の定数
                if (in.block.empty() && in.shadow.empty()) continue;
                return false;
            }
            if (!pureInvariant(c, boundVars, depth + 1)) return false;
        }
        return true;
    }
    bool normCondition(const Block* loop, bool isUntil, NormCond& out) {
        bool inv = isUntil;   // until の継続条件は !cond
        const Block* c = inputBlock(loop, "CONDITION");
        int guard = 0;
        while (c && c->opcode == "operator_not" && ++guard < 32) {
            inv = !inv;
            c = inputBlock(c, "OPERAND");
        }
        if (!c) { fail("while condition missing"); return false; }
        int base;   // operator_lt → 1, operator_gt → 3（operand1 <op> operand2）
        if (c->opcode == "operator_lt") base = 1;
        else if (c->opcode == "operator_gt") base = 3;
        else { fail("while condition not lt/gt"); return false; }
        const Block* o1 = inputBlock(c, "OPERAND1");
        const Block* o2 = inputBlock(c, "OPERAND2");
        const BlockField* f1 =
            o1 && o1->opcode == "data_variable" ? fieldOf(o1, "VARIABLE")
                                                : nullptr;
        const BlockField* f2 =
            o2 && o2->opcode == "data_variable" ? fieldOf(o2, "VARIABLE")
                                                : nullptr;
        bool c1 = f1 && !f1->id.empty() && written.count(f1->id);
        bool c2 = f2 && !f2->id.empty() && written.count(f2->id);
        if (c1 == c2) {
            fail("while condition: need exactly one written scalar operand");
            return false;
        }
        // counter を左側へ正規化（operand2 が counter なら比較反転）
        if (!c1) base = (base == 1) ? 3 : 1;
        if (inv) {
            static const int kInvMap[] = {0, 4, 3, 2, 1};  // <↔>=, <=↔>
            base = kInvMap[base];
        }
        out.cmpBlock = c;
        out.varField = c1 ? f1 : f2;
        out.varId = out.varField->id;
        out.invInput = c1 ? "OPERAND2" : "OPERAND1";
        out.cmp = base;
        std::vector<std::pair<std::string, const BlockField*>> bv;
        if (!pureInvariant(c1 ? o2 : o1, bv, 0)) {
            fail("while bound not pure invariant");
            return false;
        }
        for (auto& [id, f] : bv) {
            if (!boundIds.insert(id).second) continue;
            boundVars.push_back({id, f});
        }
        return true;
    }
    std::unordered_set<std::string> boundIds;
    std::vector<std::pair<std::string, const BlockField*>> boundVars;
};

// 解析済み emission → 最終 Program（検証・CFG 解析・induction
// preheader 挿入・ジャンプ調整）。失敗 nullopt。
bool finalize(Compiler& c, std::string* why) {
    Program& cprog = c.prog;
    cprog.code.push_back(OP_HALT);
    cprog.code.push_back(0);
    const size_t n = cprog.code.size() / 2;
    Program& p = cprog;
    if (p.nSlots > 96 || p.wbVars.size() > 128) {
        if (why) *why = "too many slots/wb";
        return false;
    }
    std::string vwhy;
    if (!validateBytecode(p.code.data(), n, p.nSlots,
                          uint32_t(p.globals.size()),
                          uint32_t(p.wbVars.size()), &vwhy)) {
        if (why) *why = "bytecode: " + vwhy;
        return false;
    }
    if (c.scan.slotWritten.size() < p.nSlots)
        c.scan.slotWritten.resize(p.nSlots);
    for (size_t i = 0; i < c.slotStored.size(); ++i)
        c.scan.slotWritten[i] = c.scan.slotWritten[i] || c.slotStored[i];

    CfgResult r = analyzeCfg(p.code.data(), n, p.nSlots, c.scan);
    // pen 終端事実は最終成功解析から取る（induction 再検証が走れば r2）
    if (!r.ok) {
        if (why) *why = r.why;
        if (std::getenv("XYO_GPU_DEBUG"))
            std::fprintf(stderr, "gpu-cfg: %s\n", r.why.c_str());
        return false;
    }
    // 読み取り前に確定できなかったスロット → affine 再帰を試行
    std::vector<uint32_t> indSlots;
    for (uint32_t slot : r.needsRecurrence) {
        if (slot >= p.nSlots) {
            if (why) *why = "bad slot";
            return false;
        }
        if (std::getenv("XYO_GPU_DEBUG")) {
            for (size_t q = 0; q + 1 < p.code.size(); q += 2)
                std::fprintf(stderr, "  code[%zu]=%u %u\n", q/2,
                             p.code[q], p.code[q+1]);
        }
        Recurrence rec = analyzeRecurrence(p.code.data(), n, p.nSlots, slot,
                                           &c.roundedPushes);
        if (!rec.proven) {
            if (why) *why = "var read before write in iteration";
            if (std::getenv("XYO_GPU_DEBUG"))
                std::fprintf(stderr, "gpu-recur: slot %u not proven\n",
                             slot);
            return false;
        }
        indSlots.push_back(slot);
        auto it = c.slotVar.find(slot);
        if (it == c.slotVar.end()) {
            if (why) *why = "internal slot carried";
            return false;
        }
        InductionVar iv;
        iv.slot = slot;
        iv.step = float(rec.step);
        iv.minOff = float(rec.minOff);
        iv.maxOff = float(rec.maxOff);
        iv.globalIdx = uint32_t(p.globals.size());
        c.globals.emplace(it->second.first, iv.globalIdx);
        p.globals.push_back({it->second.first, it->second.second});
        p.induction.push_back(iv);
    }
    if (!indSlots.empty()) {
        // induction スロットは preheader で初期化 → definite 扱いで再検証
        CfgResult r2 = analyzeCfg(p.code.data(), n, p.nSlots, c.scan,
                                  indSlots);
        if (!r2.ok || !r2.needsRecurrence.empty()) {
            if (why) *why = !r2.ok ? r2.why : "unresolved carried var";
            return false;
        }
        // preheader: slot = globals[g] + ITER*step（各 induction で 6 命令）
        const uint32_t nPre = uint32_t(indSlots.size()) * 6;
        std::vector<uint32_t> pre;
        pre.reserve(size_t(nPre) * 2);
        auto emitP = [&](uint32_t op, uint32_t ar) {
            pre.push_back(op);
            pre.push_back(ar);
        };
        for (const InductionVar& iv : p.induction) {
            emitP(OP_LOADG, iv.globalIdx);
            emitP(OP_ITER, 0);
            float fs = iv.step;
            uint32_t ub;
            std::memcpy(&ub, &fs, 4);
            emitP(OP_PUSH, ub);
            emitP(OP_MUL, 0);
            emitP(OP_ADD, 0);
            emitP(OP_STOREV, iv.slot);   // 初期化は writeback しない
        }
        // 本体の全ジャンプ先を preheader 長でシフト
        for (size_t i = 0; i + 1 < p.code.size(); i += 2) {
            uint32_t op = p.code[i];
            if (op == OP_JMP || op == OP_JMPIF0) p.code[i + 1] += nPre;
        }
        pre.insert(pre.end(), p.code.begin(), p.code.end());
        p.code.swap(pre);
    }
    p.endMustDown = false;   // lanes>0 は常に penUp 開始
    p.endAttrMask = 0;
    return true;
}

} // namespace

std::optional<Program> compileParallelRepeat(Blocks* blocks,
                                             const std::string& headId,
                                             std::string* why) {
    Compiler c;
    c.blocks = blocks;
    {
        std::unordered_set<std::string> seen;
        c.collectWritten(headId, seen);
        c.inlining.clear();
    }
    if (!headId.empty())
        c.compileChain(headId);
    if (!c.ok) {
        if (why) *why = c.why;
        return std::nullopt;
    }
    c.prog.mode = LoopMode::Repeat;
    if (!finalize(c, why)) return std::nullopt;
    if (c.prog.nSlots > uint32_t(kMaxSlots)) {
        if (why) *why = "too many variable slots";
        return std::nullopt;
    }
    if (c.prog.wbVars.size() > uint32_t(kMaxWb)) {
        if (why) *why = "too many writeback vars";
        return std::nullopt;
    }
    c.prog.hasPenOps = c.scan.hasPenOps;
    c.prog.maxPerLane =
        std::max(32u, std::min(2048u, c.estRecords + 8u));
    return std::move(c.prog);
}

std::optional<ParallelLoop> compileParallelLoop(Blocks* blocks,
                                              const Block* loop,
                                              std::string* why) {
    if (!loop) {
        if (why) *why = "no loop";
        return std::nullopt;
    }
    const std::string& op = loop->opcode;
    LoopMode mode;
    if (op == "control_repeat") mode = LoopMode::Repeat;
    else if (op == "control_for_each") mode = LoopMode::ForEach;
    else if (op == "control_while" || op == "control_repeat_until")
        mode = LoopMode::While;
    else {
        if (why) *why = "not a loop";
        return std::nullopt;
    }

    Compiler c;
    c.blocks = blocks;
    std::string bodyHead;
    if (const BlockInput* in = loop->inputs.find("SUBSTACK"))
        bodyHead = in->block;
    {
        std::unordered_set<std::string> seen;
        if (!bodyHead.empty()) c.collectWritten(bodyHead, seen);
        // for_each のループ変数は反復毎に書き込まれる（implicit write）
        if (mode == LoopMode::ForEach) {
            if (const BlockField* f = c.fieldOf(loop, "VARIABLE");
                f && !f->id.empty())
                c.written.insert(f->id);
            else {
                if (why) *why = "for_each no var";
                return std::nullopt;
            }
        }
        c.inlining.clear();
    }

    // while/until: 条件を正規化し counted loop に帰着できるか確認。
    Compiler::NormCond nc;
    if (mode == LoopMode::While) {
        if (!c.normCondition(loop, op == "control_repeat_until", nc))
            { if (why) *why = c.why; return std::nullopt; }
    }

    if (mode == LoopMode::ForEach) {
        // 本体の先頭に var = ITER+1 を挿入（空・条件次第で skip する
        // 本体でもループ変数は必ず書き戻される）。
        const BlockField* f = c.fieldOf(loop, "VARIABLE");
        c.emit(OP_ITER, 0);
        c.emitF(OP_PUSH, 1.0f);
        c.emit(OP_ADD, 0);
        c.emitVarStore(f->id, f);
    }
    if (!bodyHead.empty())
        c.compileChain(bodyHead);
    if (!c.ok) {
        if (why) *why = c.why;
        return std::nullopt;
    }
    c.prog.mode = mode;

    // bound が参照する変数は globals 末尾へ（実行時型ガードのみ —
    // VM は LOADG しない）。
    for (auto& [id, f] : c.boundVars) {
        c.prog.globals.push_back(
            {id, f && f->value.isString() && f->value.asStrPtr()
                     ? std::u16string(f->value.asStrPtr()->view())
                     : std::u16string{}});
        ++c.prog.nBoundGuards;
    }

    if (!finalize(c, why)) return std::nullopt;

    if (mode == LoopMode::While) {
        // カウンタは証明済み再帰で非ゼロステップ必須
        auto itSlot = c.slots.find(nc.varId);
        if (itSlot == c.slots.end()) {
            if (why) *why = "while counter not a body var";
            return std::nullopt;
        }
        int32_t widx = -1;
        for (size_t i = 0; i < c.prog.induction.size(); ++i)
            if (c.prog.induction[i].slot == itSlot->second)
                { widx = int32_t(i); break; }
        if (widx < 0 || c.prog.induction[size_t(widx)].step == 0) {
            if (why) *why = "while counter not a constant-step recurrence";
            return std::nullopt;
        }
        c.prog.whileInd = widx;
        c.prog.whileCmp = uint32_t(nc.cmp);
    }

    if (c.prog.nSlots > uint32_t(kMaxSlots)) {
        if (why) *why = "too many variable slots";
        return std::nullopt;
    }
    if (c.prog.wbVars.size() > uint32_t(kMaxWb)) {
        if (why) *why = "too many writeback vars";
        return std::nullopt;
    }
    c.prog.hasPenOps = c.scan.hasPenOps;
    c.prog.maxPerLane =
        std::max(32u, std::min(2048u, c.estRecords + 8u));

    ParallelLoop out;
    out.program = std::move(c.prog);
    if (mode == LoopMode::While) {
        out.limitParent = nc.cmpBlock;
        out.limitInput = nc.invInput;
    }
    return out;
}

} // namespace xyo::gpu
