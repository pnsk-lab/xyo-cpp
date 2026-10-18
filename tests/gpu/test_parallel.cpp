// xyo-cpp: GPU 並列ループ（VM バイトコード経路）のテスト。
// test_gpu.cpp の main から test_parallel_gpu() として呼ばれる
// （独自 main は持たない）。
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/cast.hpp"
#include "core/rng.hpp"
#include "core/sstr.hpp"
#include "core/value.hpp"
#include "engine/runtime.hpp"
#include "engine/thread.hpp"
#include "gpu/gpu_analysis.hpp"
#include "gpu/gpu_compile.hpp"
#include "gpu/gpu_isa.hpp"
#include "load/sb3.hpp"
#include "model/blocks.hpp"
#include "model/target.hpp"
#include "model/variable.hpp"
#include "render/sdl_renderer.hpp"

#if defined(XYO_JIT)
#include "jit/jit.hpp"
extern "C" int32_t xyo_gpu_repeat(xyo::JitCtx*, const void*, double);
#endif

using namespace xyo;

static int p_checks = 0, p_fail = 0;
#define CHECK(cond) do { ++p_checks; if (!(cond)) { ++p_fail; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++p_checks; \
    if (!((a) == (b))) { ++p_fail; \
    std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)

namespace {

// ---------------------------------------------------------------
// プログラマティック Blocks ビルダー（SB3 ローダを介さず直接構築）
// ---------------------------------------------------------------
struct BB {
    Blocks* bs;
    // 同一 Blocks を共有する BB インスタンス間で id が衝突しないよう
    // static カウンタ（createBlock は重複 id を無視して古いブロックを
    // 返すため、インスタンス毎のリセットは stale block 参照になる）。
    static int n;
    std::string id() { return "pb" + std::to_string(n++); }
    Block* add(const char* op, bool topLevel = false) {
        Block b;
        b.id = id();
        b.opcode = op;
        b.topLevel = topLevel;
        bs->createBlock(b);
        return bs->getBlock(b.id);
    }
    // 数値リテラル shadow（math_number）
    void lit(Block* parent, const char* name, double v) {
        Block* s = add("math_number");
        BlockField f;
        f.value = Value::num(v);
        s->fields["NUM"] = f;
        BlockInput in;
        in.shadow = s->id;
        parent->inputs.set(name, in);
    }
    void inp(Block* parent, const char* name, Block* c) {
        BlockInput in;
        in.block = c->id;
        parent->inputs.set(name, in);
    }
    // data_variable shadow（変数参照を inputs に直接置く sb3 形）
    void varInp(Block* parent, const char* name, const char* vname,
                const char* vid) {
        Block* s = add("data_variable");
        BlockField f;
        f.value = Value::string(SStr::create(utf8ToU16(vname)));
        f.id = vid;
        s->fields["VARIABLE"] = f;
        BlockInput in;
        in.shadow = s->id;
        parent->inputs.set(name, in);
    }
    void varField(Block* b, const char* vname, const char* vid) {
        BlockField f;
        f.value = Value::string(SStr::create(utf8ToU16(vname)));
        f.id = vid;
        b->fields["VARIABLE"] = f;
    }
    Block* setVar(const char* vname, const char* vid) {
        Block* b = add("data_setvariableto");
        varField(b, vname, vid);
        return b;
    }
    Block* changeVar(const char* vname, const char* vid) {
        Block* b = add("data_changevariableby");
        varField(b, vname, vid);
        return b;
    }
    Block* chain(std::initializer_list<Block*> list) {
        Block* prev = nullptr;
        for (Block* b : list) {
            if (prev) prev->next = b->id;
            prev = b;
        }
        return list.size() ? *list.begin() : nullptr;
    }
};
int BB::n = 0;

struct Env {
    Runtime rt;
    Blocks* bs;   // sprite blocks
    Target* sp;
    bool ok = false;
    Env() {
        // 最小プロジェクト（Stage + Sprite + 変数）
        static const char* kProj = R"JSON({
          "targets": [
            {"isStage":true,"name":"Stage",
             "variables":{"sg":["shared",42],"gs":["gs",0]},
             "lists":{},"broadcasts":{},"blocks":{},"comments":{},
             "costumes":[],"sounds":[],"currentCostume":0,"volume":100,
             "layerOrder":0},
            {"isStage":false,"name":"S",
             "variables":{"vi":["i",0],"vv":["v",0],"vn":["n",0],
                          "vb":["bound",0],"vw":["w",0],"vj":["j",0]},
             "lists":{},"broadcasts":{},"blocks":{},"comments":{},
             "costumes":[],"sounds":[],"currentCostume":0,"x":0,"y":0,
             "size":100,"direction":90,"rotationStyle":"all around",
             "visible":true,"layerOrder":1}
          ],"monitors":[],"extensions":["pen"],
          "meta":{"semver":"3.0.0","vm":"0.0.0","agent":""}
        })JSON";
        Sb3LoadResult r = loadSb3(rt, kProj, nullptr);
        ok = r.ok;
        if (!ok) { std::printf("load: %s\n", r.error.c_str()); return; }
        for (Target* t : rt.targets)
            if (t->sprite && !t->isStage) { sp = t; break; }
        bs = sp ? sp->blocks : nullptr;
    }
    Variable* var(const char* id) { return sp->lookupVariableById(id); }
};

// ---- compile/CFG ケース -------------------------------------------------

std::optional<gpu::Program> compileRepeat(BB& b, Block* head,
                                        std::string* why = nullptr) {
    Block* r = b.add("control_repeat");
    b.lit(r, "TIMES", 20);
    if (head) b.inp(r, "SUBSTACK", head);
    std::string w2;
    auto pl = gpu::compileParallelLoop(b.bs, r, why ? why : &w2);
    if (!pl) return std::nullopt;
    return std::move(pl->program);
}

void test_cfg_locals() {
    Env e;
    CHECK(e.ok);
    BB b{e.bs};
    // set v = 5; set n = v + 1 → 全て反復内確定 → 受理
    {
        Block* s1 = b.setVar("v", "vv");
        b.lit(s1, "VALUE", 5);
        Block* s2 = b.setVar("n", "vn");
        Block* a = b.add("operator_add");
        b.varInp(a, "NUM1", "v", "vv");
        b.lit(a, "NUM2", 1);
        b.inp(s2, "VALUE", a);
        auto p = compileRepeat(b, b.chain({s1, s2}));
        CHECK(p.has_value());
        if (p) {
            CHECK(p->induction.empty());
            CHECK_EQ(p->wbVars.size(), 2);
        }
    }
    // set n = v * 2 （v は一度も書かれない → globals スナップショット）
    {
        BB b2{e.bs};
        Block* s = b2.setVar("n", "vn");
        Block* m = b2.add("operator_multiply");
        b2.varInp(m, "NUM1", "v", "vv");
        b2.lit(m, "NUM2", 2);
        b2.inp(s, "VALUE", m);
        auto p = compileRepeat(b2, b2.chain({s}));
        CHECK(p.has_value());
        if (p) CHECK_EQ(p->globals.size(), 1);
    }
}

void test_cfg_recurrence() {
    Env e;
    CHECK(e.ok);
    // change i by 1 → carried 読みだが step=1 の affine 再帰 → 受理
    {
        BB b{e.bs};
        Block* cv = b.changeVar("i", "vi");
        b.lit(cv, "VALUE", 1);
        std::string why;
        auto p = compileRepeat(b, b.chain({cv}), &why);
        CHECK(p.has_value());
        if (!p) std::printf("  why: %s\n", why.c_str());
        if (p) {
            CHECK_EQ(p->induction.size(), 1);
            if (!p->induction.empty())
                CHECK_EQ(p->induction[0].step, 1.0f);
        }
    }
    // set i = i + 1 も受理
    {
        BB b{e.bs};
        Block* s = b.setVar("i", "vi");
        Block* a = b.add("operator_add");
        b.varInp(a, "NUM1", "i", "vi");
        b.lit(a, "NUM2", 1);
        b.inp(s, "VALUE", a);
        auto p = compileRepeat(b, b.chain({s}));
        CHECK(p.has_value());
        if (p) CHECK_EQ(p->induction.size(), 1);
    }
    // 両分岐が同一増分 → 受理
    {
        BB b{e.bs};
        Block* cond = b.add("control_if_else");
        Block* lt = b.add("operator_lt");
        b.varInp(lt, "OPERAND1", "n", "vn");
        b.lit(lt, "OPERAND2", 5);
        b.inp(cond, "CONDITION", lt);
        Block* c1 = b.changeVar("i", "vi");
        b.lit(c1, "VALUE", 1);
        Block* c2 = b.changeVar("i", "vi");
        b.lit(c2, "VALUE", 1);
        b.inp(cond, "SUBSTACK", c1);
        b.inp(cond, "SUBSTACK2", c2);
        auto p = compileRepeat(b, b.chain({cond}));
        CHECK(p.has_value());
    }
    // 不等増分 / 条件付き増分 / 非線形 → 拒否
    {
        BB b{e.bs};
        Block* cond = b.add("control_if_else");
        Block* lt = b.add("operator_lt");
        b.varInp(lt, "OPERAND1", "n", "vn");
        b.lit(lt, "OPERAND2", 5);
        b.inp(cond, "CONDITION", lt);
        Block* c1 = b.changeVar("i", "vi");
        b.lit(c1, "VALUE", 1);
        Block* c2 = b.changeVar("i", "vi");
        b.lit(c2, "VALUE", 2);
        b.inp(cond, "SUBSTACK", c1);
        b.inp(cond, "SUBSTACK2", c2);
        CHECK(!compileRepeat(b, b.chain({cond})));
    }
    {
        BB b{e.bs};
        Block* cond = b.add("control_if");
        Block* lt = b.add("operator_lt");
        b.varInp(lt, "OPERAND1", "n", "vn");
        b.lit(lt, "OPERAND2", 5);
        b.inp(cond, "CONDITION", lt);
        Block* c1 = b.changeVar("i", "vi");
        b.lit(c1, "VALUE", 1);
        b.inp(cond, "SUBSTACK", c1);
        CHECK(!compileRepeat(b, b.chain({cond})));
    }
    {
        BB b{e.bs};   // i *= 2 — 非線形
        Block* s = b.setVar("i", "vi");
        Block* m = b.add("operator_multiply");
        b.varInp(m, "NUM1", "i", "vi");
        b.lit(m, "NUM2", 2);
        b.inp(s, "VALUE", m);
        CHECK(!compileRepeat(b, b.chain({s})));
    }
    {
        BB b{e.bs};   // v += i（蓄積 — i も v も両方 carried）
        Block* c1 = b.changeVar("i", "vi");
        b.lit(c1, "VALUE", 1);
        Block* c2 = b.changeVar("v", "vv");
        b.varInp(c2, "VALUE", "i", "vi");
        CHECK(!compileRepeat(b, b.chain({c1, c2})));
    }
    // 読み取り前に書かれない非再帰変数 → 拒否
    {
        BB b{e.bs};
        Block* s = b.setVar("n", "vn");
        Block* a = b.add("operator_add");
        b.varInp(a, "NUM1", "w", "vw");   // w 未書込み…ではなく本体未書込みなら global
        b.varInp(a, "NUM2", "i", "vi");   // i 未書込み → 両方 global → 実は受理
        b.inp(s, "VALUE", a);
        auto p = compileRepeat(b, b.chain({s}));
        CHECK(p.has_value());   // w/i は本体未書込み → スナップショット
    }
    // 内側 repeat の0回経路でしか書かれない変数の読み → 拒否
    {
        BB b{e.bs};
        Block* inner = b.add("control_repeat");
        b.lit(inner, "TIMES", 3);
        Block* sw = b.setVar("w", "vw");
        b.lit(sw, "VALUE", 7);
        b.inp(inner, "SUBSTACK", sw);
        Block* rd = b.setVar("n", "vn");
        b.varInp(rd, "VALUE", "w", "vw");
        CHECK(!compileRepeat(b, b.chain({inner, rd})));
    }
}

void test_cfg_inner_loops() {
    Env e;
    CHECK(e.ok);
    // 内側 while: レーン内逐次 — 初期化済みローカル状態のみ
    {
        BB b{e.bs};
        Block* s0 = b.setVar("v", "vv");
        b.lit(s0, "VALUE", 0);
        Block* w = b.add("control_while");
        Block* lt = b.add("operator_lt");
        b.varInp(lt, "OPERAND1", "v", "vv");
        b.lit(lt, "OPERAND2", 3);
        b.inp(w, "CONDITION", lt);
        Block* inc = b.changeVar("v", "vv");
        b.lit(inc, "VALUE", 1);
        b.inp(w, "SUBSTACK", inc);
        auto p = compileRepeat(b, b.chain({s0, w}));
        CHECK(p.has_value());
    }
    // 内側 repeat_until
    {
        BB b{e.bs};
        Block* s0 = b.setVar("v", "vv");
        b.lit(s0, "VALUE", 0);
        Block* w = b.add("control_repeat_until");
        Block* gt = b.add("operator_gt");
        b.varInp(gt, "OPERAND1", "v", "vv");
        b.lit(gt, "OPERAND2", 3);
        b.inp(w, "CONDITION", gt);
        Block* inc = b.changeVar("v", "vv");
        b.lit(inc, "VALUE", 1);
        b.inp(w, "SUBSTACK", inc);
        CHECK(compileRepeat(b, b.chain({s0, w})).has_value());
    }
    // 内側 for_each（初期化済みローカルのみ）
    {
        BB b{e.bs};
        Block* fe = b.add("control_for_each");
        fe->fields["VARIABLE"] = BlockField{
            Value::string(SStr::create(u"j")), "vj"};
        b.lit(fe, "VALUE", 4);
        Block* s = b.setVar("v", "vv");
        b.lit(s, "VALUE", 1);
        b.inp(fe, "SUBSTACK", s);
        CHECK(compileRepeat(b, b.chain({fe})).has_value());
    }
    // all_at_once はそのままインライン
    {
        BB b{e.bs};
        Block* aa = b.add("control_all_at_once");
        Block* s = b.setVar("v", "vv");
        b.lit(s, "VALUE", 1);
        b.inp(aa, "SUBSTACK", s);
        CHECK(compileRepeat(b, b.chain({aa})).has_value());
    }
}

void test_cfg_rejects() {
    Env e;
    CHECK(e.ok);
    // 欠落ブロック
    {
        BB b{e.bs};
        Block* s = b.setVar("v", "vv");
        b.lit(s, "VALUE", 1);
        s->next = "nonexistent_block";
        CHECK(!compileRepeat(b, s));
    }
    // チェーンサイクル
    {
        BB b{e.bs};
        Block* s = b.setVar("v", "vv");
        b.lit(s, "VALUE", 1);
        s->next = s->id;
        CHECK(!compileRepeat(b, s));
    }
    // 未知 fields-only reporter（whitelist 外）
    {
        BB b{e.bs};
        Block* s = b.setVar("v", "vv");
        Block* r = b.add("looks_costumenumbername");
        r->fields["NUMBER_NAME"] = BlockField{Value::string(SStr::create(u"number")), ""};
        b.inp(s, "VALUE", r);
        CHECK(!compileRepeat(b, s));
    }
    // トップレベル return（反復停止の直列意味論を再現不可）
    {
        BB b{e.bs};
        Block* s = b.setVar("v", "vv");
        b.lit(s, "VALUE", 1);
        Block* ret = b.add("procedures_return");
        b.lit(ret, "VALUE", 0);
        CHECK(!compileRepeat(b, b.chain({s, ret})));
    }
    // stop も不可
    {
        BB b{e.bs};
        Block* s = b.add("control_stop");
        s->fields["STOP_OPTION"] =
            BlockField{Value::string(SStr::create(u"this script")), ""};
        CHECK(!compileRepeat(b, s));
    }
    // wait/broadcast/touching 系
    {
        BB b{e.bs};
        Block* s = b.setVar("v", "vv");
        b.varInp(s, "VALUE", "i", "vi");   // i は本体未書込み → global
        Block* w = b.add("control_wait");
        b.lit(w, "DURATION", 0);
        CHECK(!compileRepeat(b, b.chain({s, w})));
    }
}

// 手続きインライン経由の再帰受理 / return 経路
void test_cfg_proc() {
    Env e;
    CHECK(e.ok);
    BB b{e.bs};
    // proc "inc": change i by arg
    Block* def = b.add("procedures_definition");
    Block* proto = b.add("procedures_prototype");
    Mutation m;
    m.tagName = "mutation";
    m.attrs["proccode"] = "inc %s";
    m.attrs["argumentids"] = "[\"arg0\"]";
    m.attrs["argumentnames"] = "[\"by\"]";
    m.attrs["argumentdefaults"] = "[\"\"]";
    m.attrs["warp"] = "true";
    proto->mutation = m;
    def->inputs.set("custom_block", BlockInput{proto->id, ""});
    Block* body = b.changeVar("i", "vi");
    Block* arg = b.add("argument_reporter_string_number");
    arg->fields["VALUE"] = BlockField{Value::string(SStr::create(u"by")), ""};
    b.inp(body, "VALUE", arg);
    def->next = body->id;
    Block* call = b.add("procedures_call");
    call->mutation = m;
    call->inputs.set("arg0", BlockInput{"", ""});
    // 引数リテラル: shadow math_number
    {
        Block* ls = b.add("math_number");
        ls->fields["NUM"] = BlockField{Value::num(1), ""};
        call->inputs.set("arg0", BlockInput{"", ls->id});
    }
    auto p = compileRepeat(b, b.chain({call}));
    CHECK(p.has_value());
    if (p) {
        CHECK_EQ(p->induction.size(), 1);
    }
    // proc 内の return は post-return に書き込みを見せない:
    // proc が return で早期終了した経路の後続 store は
    // フィクションではない — 早期 return 経路は確定を持ち越さない。
    // return を含む proc + その後の change i で再帰受理を確認。
    BB b2{e.bs};
    Block* def2 = b2.add("procedures_definition");
    Block* proto2 = b2.add("procedures_prototype");
    Mutation m2 = m;
    m2.attrs["proccode"] = "step";
    m2.attrs["argumentids"] = "[]";
    m2.attrs["argumentnames"] = "[]";
    proto2->mutation = m2;
    def2->inputs.set("custom_block", BlockInput{proto2->id, ""});
    Block* c = b2.add("control_if");
    Block* lt = b2.add("operator_lt");
    b2.varInp(lt, "OPERAND1", "n", "vn");
    b2.lit(lt, "OPERAND2", 5);
    b2.inp(c, "CONDITION", lt);
    Block* ret = b2.add("procedures_return");
    b2.lit(ret, "VALUE", 0);
    b2.inp(c, "SUBSTACK", ret);
    Block* inc2 = b2.changeVar("i", "vi");
    b2.lit(inc2, "VALUE", 1);
    def2->next = c->id;
    c->next = inc2->id;
    Block* call2 = b2.add("procedures_call");
    call2->mutation = m2;
    // if が早期 return で i+=1 を飛ばす → 反復毎の step が不定 → 拒否
    CHECK(!compileRepeat(b2, b2.chain({call2})).has_value());
    // return なしの逐次 proc は受理されること（上と対照）
    BB b3{e.bs};
    Block* def3 = b3.add("procedures_definition");
    Block* proto3 = b3.add("procedures_prototype");
    Mutation m3 = m;
    m3.attrs["proccode"] = "step2";
    m3.attrs["argumentids"] = "[]";
    m3.attrs["argumentnames"] = "[]";
    proto3->mutation = m3;
    def3->inputs.set("custom_block", BlockInput{proto3->id, ""});
    Block* inc3 = b3.changeVar("i", "vi");
    b3.lit(inc3, "VALUE", 1);
    def3->next = inc3->id;
    Block* call3 = b3.add("procedures_call");
    call3->mutation = m3;
    auto p3 = compileRepeat(b3, b3.chain({call3}));
    CHECK(p3.has_value());
    // 未定義呼び出しは引数未評価の no-op → 受理
    BB b4{e.bs};
    Block* call4 = b4.add("procedures_call");
    Mutation m4;
    m4.tagName = "mutation";
    m4.attrs["proccode"] = "ghost %s";
    m4.attrs["argumentids"] = "[\"a0\"]";
    call4->mutation = m4;
    call4->inputs.set("a0", BlockInput{"", ""});
    auto p4 = compileRepeat(b4, b4.chain({call4}));
    CHECK(p4.has_value());
}

void test_cfg_pen() {
    Env e;
    CHECK(e.ok);
    auto gotoxy_i = [](BB& b) {
        Block* g = b.add("motion_gotoxy");
        b.lit(g, "X", 0);
        b.varInp(g, "Y", "i", "vi");
        return g;
    };
    auto gotoxy00 = [](BB& b) {
        Block* g = b.add("motion_gotoxy");
        b.lit(g, "X", 0); b.lit(g, "Y", 0);
        return g;
    };
    // 移動のみ（ペン op 無し）→ 受理（実行時 initial-down 拒否に委ねる）
    {
        BB b{e.bs};
        auto p = compileRepeat(b, b.chain({gotoxy_i(b)}));
        CHECK(p.has_value());
        if (p) CHECK(!p->hasPenOps);
    }
    // gotoxy;pendown;penup → 受理（lane>0 は up 開始、carried 観測は
    // 終端 up が殺す）
    {
        BB b{e.bs};
        auto p = compileRepeat(b, b.chain({gotoxy_i(b), b.add("pen_penDown"),
                                         b.add("pen_penUp")}));
        CHECK(p.has_value());
    }
    // penup;gotoxy;pendown（終端 down、観測前に状態上書き済み）→ 受理
    {
        BB b{e.bs};
        auto p = compileRepeat(b, b.chain({b.add("pen_penUp"),
                                           gotoxy_i(b),
                                           b.add("pen_penDown")}));
        CHECK(p.has_value());
        if (p) CHECK(p->hasPenOps);
    }
    // gotoxy;pendown（終端 down、carried 観測あり）→ 拒否
    {
        BB b{e.bs};
        auto p = compileRepeat(b, b.chain({gotoxy_i(b),
                                           b.add("pen_penDown")}));
        CHECK(!p.has_value());
    }
    // 条件付き pendown の後の移動（carried 観測 + 終端 down）→ 拒否
    {
        BB b{e.bs};
        Block* c = b.add("control_if");
        Block* lt = b.add("operator_lt");
        b.varInp(lt, "OPERAND1", "n", "vn");
        b.lit(lt, "OPERAND2", 5);
        b.inp(c, "CONDITION", lt);
        Block* pd = b.add("pen_penDown");
        b.inp(c, "SUBSTACK", pd);
        auto p = compileRepeat(b, b.chain({gotoxy00(b), c, gotoxy_i(b)}));
        CHECK(!p.has_value());
    }
    // set v = xposition; setx(1) — x が本体で書かれるのに読みが先 → 拒否
    {
        BB b{e.bs};
        Block* rd = b.setVar("v", "vv");
        Block* xp = b.add("motion_xposition");
        b.inp(rd, "VALUE", xp);
        Block* sx = b.add("motion_setx");
        b.lit(sx, "X", 1);
        CHECK(!compileRepeat(b, b.chain({rd, sx})).has_value());
    }
    // x 未書込み body での xposition 読み → 受理（一様）
    {
        BB b{e.bs};
        Block* rd = b.setVar("v", "vv");
        Block* xp = b.add("motion_xposition");
        b.inp(rd, "VALUE", xp);
        CHECK(compileRepeat(b, b.chain({rd})).has_value());
    }
}

void test_bounds() {
    using namespace gpu;
    {
        uint32_t code[] = {OP_HALT, 0};
        CHECK(!validateBytecode(code, 1, 97, 0, 0, nullptr));
        CHECK(!validateBytecode(code, 1, 0, 0, 129, nullptr));
        CHECK(validateBytecode(code, 1, 0, 0, 0, nullptr));
    }
    for (double step : {1.0000000000000002, 0.9999999999999999}) {
        Env e;
        BB b{e.bs};
        Block* cv = b.changeVar("i", "vi");
        b.lit(cv, "VALUE", step);
        CHECK(!compileRepeat(b, cv).has_value());
    }
    // 末尾が HALT でない直進/JMPIF0 fallthrough → 拒否
    {
        uint32_t code[] = {OP_PUSH, 0};
        std::string w;
        CHECK(!validateBytecode(code, 1, 4, 0, 0, &w));
    }
    {
        uint32_t code[] = {OP_JMPIF0, 0, OP_HALT, 0};
        std::string w;
        CHECK(!validateBytecode(code, 2, 4, 0, 0, &w));
    }
    // 範囲外ジャンプ
    {
        uint32_t code[] = {OP_JMP, 9, OP_HALT, 0};
        std::string w;
        CHECK(!validateBytecode(code, 2, 4, 0, 0, &w));
    }
    // 空コード
    CHECK(!validateBytecode(nullptr, 0, 0, 0, 0, nullptr));
    // スタックアンダーフロー（ADD に operand 無し）
    {
        uint32_t code[] = {OP_ADD, 0, OP_HALT, 0};
        std::string w;
        CHECK(!validateBytecode(code, 2, 4, 0, 0, &w));
    }
    // スタック高さ不一致の合流
    {
        // 0: JMPIF0 -> 2 (pop1) / fallthrough 1
        // 1: PUSH x2 -> JMP 3, 2: PUSH -> JMP 3 で高さ不一致にする
        uint32_t code[] = {OP_PUSH, 0, OP_JMPIF0, 4,
                           OP_PUSH, 0, OP_PUSH, 0, OP_JMP, 5,
                           OP_PUSH, 0, OP_HALT, 0};
        std::string w;
        CHECK(!validateBytecode(code, 6, 4, 0, 0, &w));
    }
    // analyzeCfg の OOB ガード（空 / スロット過大）
    {
        BodyScan scan;
        CHECK(!analyzeCfg(nullptr, 0, 0, scan).ok);
        uint32_t code[] = {OP_HALT, 0};
        CHECK(!analyzeCfg(code, 1, 200, scan).ok);
    }
    // recurrence の slot ガード
    CHECK(!analyzeRecurrence(nullptr, 0, 4, 0).proven);
    // オーダー: 合計が UINT32_MAX を超える → 拒否
    {
        uint32_t counts[] = {0x7fffffffu, 0x7fffffffu, 2u};
        uint32_t offs[3];
        uint64_t total = 0;
        CHECK(!penOrderOffsets(counts, 3, 0x80000000u, offs, &total));
    }
    // sqrt 由来の step（sqrt は証明で Unk → 拒否）
    {
        Env e;
        BB b{e.bs};
        Block* sq = b.add("operator_mathop");
        sq->fields["OPERATOR"] =
            BlockField{Value::string(SStr::create(u"sqrt")), ""};
        b.lit(sq, "NUM", 4);
        Block* cv = b.changeVar("i", "vi");
        b.inp(cv, "VALUE", sq);
        CHECK(!compileRepeat(b, b.chain({cv})).has_value());
    }
    // mod 由来の step → 拒否
    {
        Env e;
        BB b{e.bs};
        Block* md = b.add("operator_mod");
        b.varInp(md, "NUM1", "i", "vi");
        b.lit(md, "NUM2", 3);
        Block* sv = b.setVar("i", "vi");
        b.inp(sv, "VALUE", md);
        CHECK(!compileRepeat(b, b.chain({sv})).has_value());
    }
    // ソースが f32 に丸められる小数 step（0.1 等は f32 非整数）→
    // 再帰証明しない → carried 読みで拒否
    {
        Env e;
        BB b{e.bs};
        Block* cv = b.changeVar("i", "vi");
        b.lit(cv, "VALUE", 0.1);
        CHECK(!compileRepeat(b, b.chain({cv})).has_value());
    }
    // 中間 +2^24 打ち消し: i = (i + 16777216) - 16777216 は step0 →
    // repeat では step0 再帰（snapshot）として受理される場合がある。
    // 受理されたら minOff/maxOff が中間値を含むことを確認。
    {
        Env e;
        BB b{e.bs};
        Block* a1 = b.add("operator_add");
        b.varInp(a1, "NUM1", "i", "vi");
        b.lit(a1, "NUM2", 16777216);
        Block* a2 = b.add("operator_subtract");
        b.inp(a2, "NUM1", a1);
        b.lit(a2, "NUM2", 16777216);
        Block* sv = b.setVar("i", "vi");
        b.inp(sv, "VALUE", a2);
        auto p = compileRepeat(b, b.chain({sv}));
        if (p.has_value() && !p->induction.empty()) {
            CHECK(p->induction[0].minOff <= 0);
            CHECK(p->induction[0].maxOff >= 16777216);
        } else {
            CHECK(!p.has_value());   // 拒否も安全側
        }
    }
    // ソースの非数値 text / 外側引数を境界にする while → 拒否
    {
        Env e;
        BB b{e.bs};
        Block* w = b.add("control_while");
        Block* lt = b.add("operator_lt");
        b.varInp(lt, "OPERAND1", "i", "vi");
        Block* tx = b.add("text");
        tx->fields["TEXT"] =
            BlockField{Value::string(SStr::create(u"abc")), ""};
        b.inp(lt, "OPERAND2", tx);
        b.inp(w, "CONDITION", lt);
        Block* inc = b.changeVar("i", "vi");
        b.lit(inc, "VALUE", 1);
        b.inp(w, "SUBSTACK", inc);
        CHECK(!gpu::compileParallelLoop(b.bs, w));
    }
    // substack が親を指すサイクル → 拒否（無限再帰しない）
    {
        Env e;
        BB b{e.bs};
        Block* r = b.add("control_repeat");
        b.lit(r, "TIMES", 2);
        b.inp(r, "SUBSTACK", r);
        CHECK(!gpu::compileParallelLoop(b.bs, r));
    }
    // reporter 自己サイクル → 拒否
    {
        Env e;
        BB b{e.bs};
        Block* ad = b.add("operator_add");
        ad->inputs.set("NUM1", BlockInput{ad->id, ""});
        b.lit(ad, "NUM2", 1);
        Block* sv = b.setVar("v", "vv");
        b.inp(sv, "VALUE", ad);
        CHECK(!compileRepeat(b, b.chain({sv})).has_value());
    }
    {
        Env e;
        BB b{e.bs};
        Block* nested = b.add("control_all_at_once");
        b.inp(nested, "SUBSTACK", nested);
        CHECK(!compileRepeat(b, nested).has_value());
    }
    {
        Env e;
        BB b{e.bs};
        Block* inner = b.add("control_repeat");
        b.lit(inner, "TIMES", 2);
        Block* sv = b.setVar("v", "__t0");
        b.lit(sv, "VALUE", 1);
        b.inp(inner, "SUBSTACK", sv);
        auto p = compileRepeat(b, inner);
        CHECK(p.has_value());
        if (p) CHECK(p->nSlots >= 2);
    }
    // >96 スロット（distinct 変数 100 個 set）→ 拒否
    {
        Env e;
        BB b{e.bs};
        std::vector<Block*> l;
        for (int i = 0; i < 100; ++i) {
            char nm[16], id[16];
            snprintf(nm, 16, "x%d", i);
            snprintf(id, 16, "vx%d", i);
            Block* sv = b.setVar(nm, id);
            b.lit(sv, "VALUE", 1);
            l.push_back(sv);
        }
        Block* h = l[0];
        for (size_t i = 1; i < l.size(); ++i) l[i-1]->next = l[i]->id;
        CHECK(!compileRepeat(b, h).has_value());
    }
    // >128 wb — 129 個の再帰（affine）変数は slot 上限で先に落ちるか
    // wb 上限で落ちるか — どちらでも拒否されればよい。
    {
        Env e;
        BB b{e.bs};
        std::vector<Block*> l;
        for (int i = 0; i < 129; ++i) {
            char nm[16], id[16];
            snprintf(nm, 16, "y%d", i);
            snprintf(id, 16, "vy%d", i);
            Block* cv = b.changeVar(nm, id);
            b.lit(cv, "VALUE", 1);
            l.push_back(cv);
        }
        for (size_t i = 1; i < l.size(); ++i) l[i-1]->next = l[i]->id;
        CHECK(!compileRepeat(b, l[0]).has_value());
    }
}

// ---- 64bit 順序 id / オフセット ------------------------------------------

void test_domains_compile() {
    Env e;
    CHECK(e.ok);
    // for_each: ループ変数の暗黙 write を含む
    {
        BB b{e.bs};
        Block* fe = b.add("control_for_each");
        fe->fields["VARIABLE"] = BlockField{
            Value::string(SStr::create(u"j")), "vj"};
        b.lit(fe, "VALUE", 10);
        Block* s2 = b.setVar("v", "vv");
        b.varInp(s2, "VALUE", "j", "vj");
        b.inp(fe, "SUBSTACK", s2);
        auto pl = gpu::compileParallelLoop(b.bs, fe);
        CHECK(pl.has_value());
        if (pl) {
            CHECK(pl->program.mode == gpu::LoopMode::ForEach);
            bool found = false;
            for (auto& w : pl->program.wbVars)
                if (w.id == "vj") found = true;
            CHECK(found);
        }
    }
    auto mkWhile = [&](BB& b, const char* cmpOp, bool until,
                       bool swap, double step) {
        Block* w = b.add(until ? "control_repeat_until" : "control_while");
        Block* cmp = b.add(cmpOp);
        const char* a1 = swap ? "OPERAND2" : "OPERAND1";
        const char* a2 = swap ? "OPERAND1" : "OPERAND2";
        b.varInp(cmp, a1, "i", "vi");
        b.varInp(cmp, a2, "bound", "vb");
        b.inp(w, "CONDITION", cmp);
        Block* inc = b.changeVar("i", "vi");
        b.lit(inc, "VALUE", step);
        b.inp(w, "SUBSTACK", inc);
        return w;
    };
    // while i < bound → LoopMode::While, cmp=1, limit=OPERAND2
    {
        BB b{e.bs};
        Block* w = mkWhile(b, "operator_lt", false, false, 1);
        Block* lt = b.bs->getBlock(
            w->inputs.find("CONDITION")->block);
        auto pl = gpu::compileParallelLoop(b.bs, w);
        CHECK(pl.has_value());
        if (pl) {
            CHECK(pl->program.mode == gpu::LoopMode::While);
            CHECK_EQ(pl->program.whileCmp, 1u);
            CHECK(pl->limitParent == lt);
            CHECK_EQ(pl->limitInput, "OPERAND2");
            CHECK_EQ(pl->program.nBoundGuards, 1);
        }
    }
    // until i > bound → 継続 i<=bound（反転 → cmp=2）
    {
        BB b{e.bs};
        Block* w = mkWhile(b, "operator_gt", true, false, 1);
        auto pl = gpu::compileParallelLoop(b.bs, w);
        CHECK(pl.has_value());
        if (pl) CHECK_EQ(pl->program.whileCmp, 2u);
    }
    // bound > i ≡ i < bound（swap → cmp=1, limit=OPERAND1）
    {
        BB b{e.bs};
        Block* w = mkWhile(b, "operator_gt", false, true, 1);
        auto pl = gpu::compileParallelLoop(b.bs, w);
        CHECK(pl.has_value());
        if (pl) {
            CHECK_EQ(pl->program.whileCmp, 1u);
            CHECK_EQ(pl->limitInput, "OPERAND1");
        }
    }
    // not ラッパー: while not(i<bound) → i>=bound（step=-1）
    {
        BB b{e.bs};
        Block* w = b.add("control_while");
        Block* nt = b.add("operator_not");
        Block* lt = b.add("operator_lt");
        b.varInp(lt, "OPERAND1", "i", "vi");
        b.varInp(lt, "OPERAND2", "bound", "vb");
        b.inp(nt, "OPERAND", lt);
        b.inp(w, "CONDITION", nt);
        Block* dec = b.changeVar("i", "vi");
        b.lit(dec, "VALUE", -1);
        b.inp(w, "SUBSTACK", dec);
        auto pl = gpu::compileParallelLoop(b.bs, w);
        CHECK(pl.has_value());
        if (pl) {
            CHECK_EQ(pl->program.whileCmp, 4u);
            CHECK(pl->limitParent == lt);
        }
    }
    // 境界が本体書き変数 → 拒否 / 非比較条件 → 拒否 / step0 → 拒否
    {
        BB b{e.bs};
        Block* w = b.add("control_while");
        Block* lt = b.add("operator_lt");
        b.varInp(lt, "OPERAND1", "i", "vi");
        b.varInp(lt, "OPERAND2", "n", "vn");
        b.inp(w, "CONDITION", lt);
        Block* inc = b.changeVar("i", "vi");
        b.lit(inc, "VALUE", 1);
        Block* sn = b.setVar("n", "vn");
        b.lit(sn, "VALUE", 0);
        b.inp(w, "SUBSTACK", b.chain({inc, sn}));
        CHECK(!gpu::compileParallelLoop(b.bs, w));
    }
    {
        BB b{e.bs};
        Block* w = b.add("control_while");
        Block* eq = b.add("operator_equals");
        b.varInp(eq, "OPERAND1", "i", "vi");
        b.lit(eq, "OPERAND2", 10);
        b.inp(w, "CONDITION", eq);
        Block* inc = b.changeVar("i", "vi");
        b.lit(inc, "VALUE", 1);
        b.inp(w, "SUBSTACK", inc);
        CHECK(!gpu::compileParallelLoop(b.bs, w));
    }
    {
        BB b{e.bs};
        Block* w = mkWhile(b, "operator_lt", false, false, 0);
        CHECK(!gpu::compileParallelLoop(b.bs, w));
    }
    // 逆方向 step（i<bound で -1）— コンパイルは受理、実行時拒否
    {
        BB b{e.bs};
        Block* w = mkWhile(b, "operator_lt", false, false, -1);
        auto pl = gpu::compileParallelLoop(b.bs, w);
        CHECK(pl.has_value());
        if (pl) CHECK(pl->program.induction[0].step == -1.0f);
    }
}

void test_order() {
    CHECK_EQ(gpu::penOrderLane(gpu::penOrderPack(0, 0)), 0u);
    CHECK_EQ(gpu::penOrderStroke(gpu::penOrderPack(0, 0)), 0u);
    auto id = gpu::penOrderPack(1u << 20, UINT32_MAX);
    CHECK_EQ(gpu::penOrderLane(id), 1u << 20);
    CHECK_EQ(gpu::penOrderStroke(id), UINT32_MAX);
    CHECK_EQ(gpu::penOrderPack(1, 0), gpu::PenOrderId(1) << 32);
    // 昇順ソート性: (lane,stroke) 辞書順 = 数値順
    CHECK(gpu::penOrderPack(0, UINT32_MAX) < gpu::penOrderPack(1, 0));
    CHECK(gpu::penOrderPack(2, 5) < gpu::penOrderPack(2, 6));

    uint32_t counts[] = {2, 0, 1, 3};
    uint32_t offs[4];
    uint64_t total = 0;
    CHECK(gpu::penOrderOffsets(counts, 4, 4, offs, &total));
    CHECK_EQ(total, 6u);
    CHECK_EQ(offs[0], 0u);
    CHECK_EQ(offs[1], 2u);
    CHECK_EQ(offs[2], 2u);
    CHECK_EQ(offs[3], 3u);
    // guarded ビット → 拒否
    uint32_t bad[] = {1, 0x80000000u};
    CHECK(!gpu::penOrderOffsets(bad, 2, 4, offs, &total));
    // maxPerLane 超過 → 拒否
    uint32_t over[] = {5};
    CHECK(!gpu::penOrderOffsets(over, 1, 4, offs, &total));
}

#if defined(XYO_JIT)
// ---- ヘルパ（xyo_gpu_repeat）のレンダラスタブ検証 ------------------------

struct CapRen : RendererAPI {
    int calls = 0;
    uint32_t lanes = 0, nWb = 0, nSlots = 0;
    bool failExec = false;
    bool fillOut = false;
    std::vector<float> out;        // 偽 laneOut（呼出し側が充填可）
    std::vector<uint32_t> codeCopy;
    const uint32_t* codeSeen = nullptr;
    size_t codeLenSeen = 0;
    void setFlags(uint32_t lane, uint32_t stride, uint32_t bits) {
        float f;
        std::memcpy(&f, &bits, 4);
        out[size_t(lane) * stride] = f;
    }
    void setWb(uint32_t lane, uint32_t stride, uint32_t nwb,
               uint32_t wi, float v) {
        uint32_t mw = (nwb + 31) / 32;
        uint32_t u;
        std::memcpy(&u,
                    &out[size_t(lane) * stride + 8 + wi / 32], 4);
        u |= 1u << (wi % 32);
        float mf;
        std::memcpy(&mf, &u, 4);
        out[size_t(lane) * stride + 8 + wi / 32] = mf;
        out[size_t(lane) * stride + 8 + mw + wi] = v;
    }
    bool gpuExecParallel(gpu::ExecRequest& req) override {
        ++calls;
        lanes = req.lanes;
        nWb = req.nWb;
        nSlots = req.nSlots;
        codeSeen = req.code;
        codeLenSeen = req.codeLen;
        codeCopy.assign(req.code, req.code + req.codeLen);
        if (failExec) return false;
        size_t stride = gpu::laneOutStride(req.nWb);
        if (!fillOut || out.size() < size_t(req.lanes) * stride)
            out.assign(size_t(req.lanes) * stride, 0.0f);
        req.laneOut = out.data();
        req.executed = true;
        req.nRecords = 0;
        return true;
    }
};

JitCtx makeCtx(Runtime& rt, Target* t) {
    JitCtx c{};
    c.runtime = &rt;
    c.target = t;
    c.isWarp = 1;
    return c;
}

void test_helper_repeat_for_each() {
    Env e;
    CHECK(e.ok);
    BB b{e.bs};
    // 単純本体（変数 set のみ）
    Block* s = b.setVar("v", "vv");
    b.lit(s, "VALUE", 1);
    Block* rep = b.add("control_repeat");
    b.lit(rep, "TIMES", 20);
    b.inp(rep, "SUBSTACK", s);
    auto pl = gpu::compileParallelLoop(b.bs, rep);
    CHECK(pl.has_value());
    if (!pl) return;
    CapRen ren;
    e.rt.renderer = &ren;
    JitCtx c = makeCtx(e.rt, e.sp);
    // TIMES=10.6 → floor(+0.5)=11
    CHECK_EQ(xyo_gpu_repeat(&c, &pl->program, 10.6), 1);
    CHECK_EQ(ren.calls, 1);
    CHECK_EQ(ren.lanes, 11u);
    // TIMES=10.4 → 10
    CHECK_EQ(xyo_gpu_repeat(&c, &pl->program, 10.4), 1);
    CHECK_EQ(ren.lanes, 10u);

    // for_each: floor(VALUE)
    BB b2{e.bs};
    Block* fe = b2.add("control_for_each");
    fe->fields["VARIABLE"] = BlockField{
        Value::string(SStr::create(u"j")), "vj"};
    b2.lit(fe, "VALUE", 10);
    Block* fs = b2.setVar("v", "vv");
    b2.lit(fs, "VALUE", 1);
    b2.inp(fe, "SUBSTACK", fs);
    auto pf = gpu::compileParallelLoop(b2.bs, fe);
    CHECK(pf.has_value());
    if (pf) {
        CHECK_EQ(xyo_gpu_repeat(&c, &pf->program, 7.9), 1);
        CHECK_EQ(ren.lanes, 7u);
        // ゼロターン: floor(0.4)=0 → dispatch なし
        CHECK_EQ(xyo_gpu_repeat(&c, &pf->program, 0.4), 0);
    }
    e.rt.renderer = nullptr;
}

// counted while の lanes 導出
void test_helper_while() {
    Env e;
    CHECK(e.ok);
    auto mkWhile = [&](BB& b, const char* cmpOp, bool until,
                       bool swap, double step) {
        Block* w = b.add(until ? "control_repeat_until" : "control_while");
        Block* cmp = b.add(cmpOp);
        const char* a1 = swap ? "OPERAND2" : "OPERAND1";
        const char* a2 = swap ? "OPERAND1" : "OPERAND2";
        b.varInp(cmp, a1, "i", "vi");
        b.varInp(cmp, a2, "bound", "vb");
        b.inp(w, "CONDITION", cmp);
        Block* inc = b.changeVar("i", "vi");
        b.lit(inc, "VALUE", step);
        b.inp(w, "SUBSTACK", inc);
        return w;
    };
    CapRen ren;
    e.rt.renderer = &ren;
    JitCtx c = makeCtx(e.rt, e.sp);

    // while i < bound, i+=1, i=0, bound=10 → 10 レーン
    {
        BB b{e.bs};
        Block* w = mkWhile(b, "operator_lt", false, false, 1);
        auto pl = gpu::compileParallelLoop(b.bs, w);
        CHECK(pl.has_value());
        if (!pl) return;
        e.var("vi")->value = Value::num(0);
        CHECK_EQ(xyo_gpu_repeat(&c, &pl->program, 10.0), 1);
        CHECK_EQ(ren.lanes, 10u);
        // i0=10 → 0 ターン（dispatch なし）
        e.var("vi")->value = Value::num(10);
        CHECK_EQ(xyo_gpu_repeat(&c, &pl->program, 10.0), 0);
        // 逆方向 step=-1 + while i<bound → 拒否
        BB b2{e.bs};
        Block* w2 = mkWhile(b2, "operator_lt", false, false, -1);
        auto pl2 = gpu::compileParallelLoop(b2.bs, w2);
        CHECK(pl2.has_value());
        if (pl2) {
            e.var("vi")->value = Value::num(0);
            int before = ren.calls;
            CHECK_EQ(xyo_gpu_repeat(&c, &pl2->program, 10.0), 0);
            CHECK_EQ(ren.calls, before);   // 境界チェックで早期 return
        }
    }
    // until i > bound → 継続 i<=bound、i0=0,bound=10,step1 → 11
    {
        BB b{e.bs};
        Block* w = mkWhile(b, "operator_gt", true, false, 1);
        auto pl = gpu::compileParallelLoop(b.bs, w);
        CHECK(pl.has_value());
        if (pl) {
            e.var("vi")->value = Value::num(0);
            CHECK_EQ(xyo_gpu_repeat(&c, &pl->program, 10.0), 1);
            CHECK_EQ(ren.lanes, 11u);
        }
    }
    // swapped: bound > i ≡ i < bound
    {
        BB b{e.bs};
        Block* w = mkWhile(b, "operator_gt", false, true, 1);
        auto pl = gpu::compileParallelLoop(b.bs, w);
        CHECK(pl.has_value());
        if (pl) {
            e.var("vi")->value = Value::num(0);
            CHECK_EQ(xyo_gpu_repeat(&c, &pl->program, 10.0), 1);
            CHECK_EQ(ren.lanes, 10u);
        }
    }
    // 負ステップ inclusive: until i < 0（継続 i >= 0）、i0=10,step=-1 → 11
    {
        BB b{e.bs};
        Block* w = mkWhile(b, "operator_lt", true, false, -1);
        auto pl = gpu::compileParallelLoop(b.bs, w);
        CHECK(pl.has_value());
        if (pl) {
            e.var("vi")->value = Value::num(10);
            CHECK_EQ(xyo_gpu_repeat(&c, &pl->program, 0.0), 1);
            CHECK_EQ(ren.lanes, 11u);
        }
    }
    // 非数値/NaN/Inf/非整数開始 → 全て拒否
    {
        BB b{e.bs};
        Block* w = mkWhile(b, "operator_lt", false, false, 1);
        auto pl = gpu::compileParallelLoop(b.bs, w);
        CHECK(pl.has_value());
        if (!pl) return;
        int before = ren.calls;
        e.var("vi")->value = Value::string(SStr::create(u"abc"));
        CHECK_EQ(xyo_gpu_repeat(&c, &pl->program, 10.0), 0);
        e.var("vi")->value = Value::nan();
        CHECK_EQ(xyo_gpu_repeat(&c, &pl->program, 10.0), 0);
        e.var("vi")->value = Value::num(
            std::numeric_limits<double>::infinity());
        CHECK_EQ(xyo_gpu_repeat(&c, &pl->program, 10.0), 0);
        e.var("vi")->value = Value::num(0.5);
        CHECK_EQ(xyo_gpu_repeat(&c, &pl->program, 10.0), 0);
        CHECK_EQ(ren.calls, before);
        // exactness: 2^24 超の範囲 → 拒否
        e.var("vi")->value = Value::num(0);
        CHECK_EQ(xyo_gpu_repeat(&c, &pl->program, 20000000.0), 0);
        CHECK_EQ(ren.calls, before);
        // 非数値境界 → 拒否（helper limit は double だが NaN で）
        e.var("vi")->value = Value::num(0);
        CHECK_EQ(xyo_gpu_repeat(&c, &pl->program,
                                std::numeric_limits<double>::quiet_NaN()),
                 0);
        CHECK_EQ(ren.calls, before);
    }
    e.rt.renderer = nullptr;
}

// 偽 laneOut による成分別 writeback 検証
void test_helper_writeback() {
    Env e;
    CHECK(e.ok);
    BB b{e.bs};
    // 本体: setx/changex/sety + pen size/color param + set v
    Block* sx = b.setVar("v", "vv");   // まず wb 変数を確保
    b.lit(sx, "VALUE", 0);
    Block* mx = b.add("motion_setx");
    b.varInp(mx, "X", "i", "vi");
    Block* pd = b.add("pen_penDown");
    Block* sz = b.add("pen_setPenSizeTo");
    b.lit(sz, "SIZE", 9);
    Block* cn = b.add("pen_setPenColorToColor");
    b.lit(cn, "COLOR", 0xFF0000);
    Block* pu = b.add("pen_penUp");
    auto pl = compileRepeat(b, b.chain({mx, sz, cn, pd, pu, sx}));
    CHECK(pl.has_value());
    if (!pl) return;
    gpu::Program& p = *pl;
    CHECK_EQ(p.wbVars.size(), 1);
    uint32_t nWb = uint32_t(p.wbVars.size());
    size_t stride = gpu::laneOutStride(nWb);

    CapRen ren;
    e.rt.renderer = &ren;
    JitCtx c = makeCtx(e.rt, e.sp);
    CHECK_EQ(xyo_gpu_repeat(&c, &p, 4.0), 1);
    CHECK_EQ(ren.calls, 1);
    CHECK_EQ(ren.lanes, 4u);

    // 偽 laneOut を構築: lane3 が X・size・shade(PENCOLN)、
    // lane2 が Y、hue、penDown の最終ライタ、lane0 が v の最終ライタ
    // （それぞれ独立に最終ライタを持てるか）。
    ren.out.assign(4 * stride, 0.0f);
    auto setF = [&](uint32_t lane, size_t off, float v) {
        ren.out[lane * stride + off] = v;
    };
    // lane1: X=111
    ren.setFlags(1, stride, gpu::kLaneFlagX | 1u);
    setF(1, 1, 111);
    // lane3: Y=222, size=9, hue, PENCOLN(shade=33)
    ren.setFlags(3, stride,
                 gpu::kLaneFlagY | gpu::kLaneFlagSize |
                 gpu::kLaneFlagHue | 16u);
    setF(3, 2, 222);
    setF(3, 3, 9);
    setF(3, 4, 55);
    setF(3, gpu::laneOutShadeOffset(nWb), 33);
    // lane2: penDown writer
    ren.setFlags(2, stride, 4u | 8u);   // pdDirty + penDown
    // lane0: v の wb（wbIdx 0）
    ren.setWb(0, stride, nWb, 0, 77.5f);

    // gpuExecParallel をもう一度通すには laneOut を差し替える —
    // stub は out.assign で上書きするので、代わりに fillOut モードで
    // 保持したい → ここでは直接書き戻し部分を別経路で検証する。
    // 上記の偽データが消えないよう、再実行用に stub の out を
    // あらかじめ充填してから呼ぶ設計にはできない（assign で消える）。
    // → writeback のみを直接検証: 一度目の呼出しで req.laneOut が
    // ren.out.data() を指しているので、上記データを埋めたうえで
    // もう一度 xyo_gpu_repeat は呼べない。代わりに stub を「充填済み
    // データを返す」モードにする。
    // （fillOut=true なら out を上書きしない）
    ren.fillOut = true;
    e.sp->x = 0; e.sp->y = 0;
    CHECK_EQ(xyo_gpu_repeat(&c, &p, 4.0), 1);
    CHECK_EQ(e.sp->x, 111.0);
    CHECK_EQ(e.sp->y, 222.0);
    CHECK(e.sp->penState != nullptr);
    if (e.sp->penState) {
        CHECK_EQ(e.sp->penState->penSize, 9.0);
        CHECK_EQ(e.sp->penState->color, 55.0);
        CHECK_EQ(e.sp->penState->shade, 33.0);
        CHECK(e.sp->penState->penDown);
        CHECK(e.sp->onTargetMoved != nullptr);
    }
    CHECK_EQ(e.var("vv")->value.asNumberUnchecked(), 77.5);

    // 失敗 dispatch は何も変えない（変数/RNG/pen state）
    ren.failExec = true;
    ren.fillOut = false;
    e.var("vv")->value = Value::num(5);
    double px = e.sp->x;
    Rng r0 = Rng::global();
    uint32_t seq0 = r0.next();
    CHECK_EQ(xyo_gpu_repeat(&c, &p, 4.0), 0);
    CHECK_EQ(e.var("vv")->value.asNumberUnchecked(), 5.0);
    CHECK_EQ(e.sp->x, px);
    Rng r1 = Rng::global();
    CHECK_EQ(r1.next(), seq0);   // 失敗で RNG は進まない
    ren.failExec = false;
    e.rt.renderer = nullptr;
}

// 未解決変数（globals 参照）/ エイリアス → 拒否・非破壊
void test_helper_unresolved() {
    Env e;
    CHECK(e.ok);
    BB b{e.bs};
    // 本体が存在しない変数 id "nope" を読む（本体未書込み → global）
    Block* s = b.setVar("v", "vv");
    b.varInp(s, "VALUE", "ghost", "noghost");
    auto pl = compileRepeat(b, b.chain({s}));
    CHECK(pl.has_value());
    if (!pl) return;
    CapRen ren;
    e.rt.renderer = &ren;
    JitCtx c = makeCtx(e.rt, e.sp);
    size_t nVarsBefore = e.sp->variables.size();
    CHECK_EQ(xyo_gpu_repeat(&c, &*pl, 10.0), 0);
    CHECK_EQ(ren.calls, 0);
    CHECK_EQ(e.sp->variables.size(), nVarsBefore);   // 生成されない
    Block* a = b.setVar("v", "alias1");
    Block* z = b.setVar("v", "alias2");
    b.lit(a, "VALUE", 1);
    b.lit(z, "VALUE", 2);
    auto aliases = compileRepeat(b, b.chain({a, z}));
    CHECK(aliases.has_value());
    if (aliases) {
        Value before = e.var("vv")->value;
        CHECK_EQ(xyo_gpu_repeat(&c, &*aliases, 10.0), 0);
        CHECK_EQ(ren.calls, 0);
        CHECK_EQ(e.var("vv")->value.bits, before.bits);
        CHECK_EQ(e.sp->variables.size(), nVarsBefore);
    }
    Block* ro = b.setVar("v", "vv");
    b.varInp(ro, "VALUE", "bound", "vb");
    auto snapshots = compileRepeat(b, ro);
    CHECK(snapshots.has_value());
    if (snapshots) {
        e.var("vb")->value = Value::num(1e300);
        CHECK_EQ(xyo_gpu_repeat(&c, &*snapshots, 10.0), 0);
        CHECK_EQ(ren.calls, 0);
        e.var("vb")->value = Value::num(1);
        CHECK_EQ(xyo_gpu_repeat(&c, &*snapshots, 10.0), 1);
        CHECK_EQ(ren.calls, 1);
    }
    e.rt.renderer = nullptr;
}
#endif // XYO_JIT

// ---- ライブテスト（XYO_TEST_GPU 時のみ、実 GL4.3）------------------------

static bool liveGpu() { return std::getenv("XYO_TEST_GPU") != nullptr; }

#if defined(XYO_JIT)
// gpuExecParallel を強制失敗させて CPU パスを走らせるラッパ。
// gpuExecParallel を強制失敗させて CPU パスを走らせるラッパ。
struct NoGpuRen : SdlRenderer {
    int calls = 0;
    bool gpuExecParallel(gpu::ExecRequest& req) override {
        ++calls;
        return false;
    }
};
struct CountGpuRen : SdlRenderer {
    int calls = 0, executed = 0, overflowFailures = 0;
    bool forceOverflow = false;
    bool gpuExecParallel(gpu::ExecRequest& req) override;
};

// ライブフィクスチャは SB3 JSON で構築（ローダが shadow/argValues/
// スクリプト登録を正しく行う — プログラマティック生成は interp の
// 入力評価経路を通らない）。
// kind: 0=repeat 1=for_each 2=while 3=until 4=二ディスパッチ境界
// 各反復: i=1..16、penUp; gotoxy(0,0); size8; i 奇=赤(0xFF0000)
// 偶=青(0x0000FF); transparency50; pendown; gotoxy(8,0);
// gotoxy(0,0); penup — 3 重なりストローク/反復、最終(i=16)は青優勢。
std::string liveProjectJson(int kind) {
    if (kind == 5) return R"JSON({"targets":[
{"isStage":true,"name":"Stage","variables":{},"lists":{},"broadcasts":{},"blocks":{},"costumes":[],"sounds":[]},
{"isStage":false,"name":"S","variables":{"vi":["i",16],"vd":["done",0]},"lists":{},"broadcasts":{},"costumes":[],"sounds":[],"blocks":{
"hat":{"opcode":"event_whenflagclicked","next":"call","topLevel":true,"inputs":{},"fields":{}},
"call":{"opcode":"procedures_call","next":null,"parent":"hat","inputs":{},"fields":{},"mutation":{"proccode":"lv","argumentids":"[]","warp":"true"}},
"PD":{"opcode":"procedures_definition","next":"w","topLevel":true,"inputs":{"custom_block":[1,"pp"]},"fields":{}},
"pp":{"opcode":"procedures_prototype","parent":"PD","shadow":true,"inputs":{},"fields":{},"mutation":{"proccode":"lv","argumentids":"[]","argumentnames":"[]","argumentdefaults":"[]","warp":"true"}},
"w":{"opcode":"control_while","next":"dn","parent":"PD","inputs":{"CONDITION":[1,"gt"],"SUBSTACK":[2,"dec"]},"fields":{}},
"gt":{"opcode":"operator_gt","parent":"w","inputs":{"OPERAND1":[3,"vr",[12,"i","vi"]],"OPERAND2":[1,"nan"]},"fields":{}},
"vr":{"opcode":"data_variable","parent":"gt","inputs":{},"fields":{"VARIABLE":["i","vi"]}},
"nan":{"opcode":"operator_mathop","parent":"gt","inputs":{"NUM":[1,[4,"-1"]]},"fields":{"OPERATOR":["sqrt",null]}},
"dec":{"opcode":"data_changevariableby","next":null,"parent":"w","inputs":{"VALUE":[1,[4,"-1"]]},"fields":{"VARIABLE":["i","vi"]}},
"dn":{"opcode":"data_setvariableto","next":null,"parent":"PD","inputs":{"VALUE":[1,[4,"1"]]},"fields":{"VARIABLE":["done","vd"]}}
},"x":0,"y":0,"size":100,"direction":90,"visible":true,"rotationStyle":"all around"}
],"monitors":[],"extensions":[],"meta":{"semver":"3.0.0","vm":"0.0.0","agent":""}})JSON";
    // ループブロック + 条件参照
    std::string loop, pre = R"BLK("pre_inc":{"opcode":"data_changevariableby","next":"LB","parent":"PD","inputs":{"VALUE":[1,[4,"1"]]},"fields":{"VARIABLE":["i","vi"]},"shadow":false},)BLK";
    std::string bodyFirst = "pre_inc";  // repeat/while/until: i++ が先
    if (kind == 1) { bodyFirst = "LB"; pre.clear(); }
    // 本体（LB..）
    const char* body = R"BLK(
"LB":{"opcode":"pen_penUp","next":"g0","parent":"PD","inputs":{},"fields":{},"shadow":false},
"g0":{"opcode":"motion_gotoxy","next":"sz","parent":"PD","inputs":{"X":[1,[4,"0"]],"Y":[1,[4,"0"]]},"fields":{},"shadow":false},
"sz":{"opcode":"pen_setPenSizeTo","next":"ie","parent":"PD","inputs":{"SIZE":[1,[4,"8"]]},"fields":{},"shadow":false},
"ie":{"opcode":"control_if_else","next":"tr","parent":"PD","inputs":{"CONDITION":[1,"eq"],"SUBSTACK":[2,"cr"],"SUBSTACK2":[2,"cb"]},"fields":{},"shadow":false},
"eq":{"opcode":"operator_equals","next":null,"parent":"ie","inputs":{"OPERAND1":[1,"md"],"OPERAND2":[1,[4,"1"]]},"fields":{},"shadow":false},
"md":{"opcode":"operator_mod","next":null,"parent":"eq","inputs":{"NUM1":[3,"viref",[12,"i","vi"]],"NUM2":[1,[4,"2"]]},"fields":{},"shadow":false},
"viref":{"opcode":"data_variable","next":null,"parent":"md","inputs":{},"fields":{"VARIABLE":["i","vi"]},"shadow":false},
"cr":{"opcode":"pen_setPenColorToColor","next":null,"parent":"ie","inputs":{"COLOR":[1,[4,"16711680"]]},"fields":{},"shadow":false},
"cb":{"opcode":"pen_setPenColorToColor","next":null,"parent":"ie","inputs":{"COLOR":[1,[4,"255"]]},"fields":{},"shadow":false},
"tr":{"opcode":"pen_setPenColorParamTo","next":"pd","parent":"PD","inputs":{"COLOR_PARAM":[1,"mn"],"VALUE":[1,[4,"50"]]},"fields":{},"shadow":false},
"mn":{"opcode":"pen_menu_colorParam","next":null,"parent":"tr","inputs":{},"fields":{"colorParam":["transparency",null]},"shadow":true},
"pd":{"opcode":"pen_penDown","next":"g8","parent":"PD","inputs":{},"fields":{},"shadow":false},
"g8":{"opcode":"motion_gotoxy","next":"g0b","parent":"PD","inputs":{"X":[1,[4,"8"]],"Y":[1,[4,"0"]]},"fields":{},"shadow":false},
"g0b":{"opcode":"motion_gotoxy","next":"pu2","parent":"PD","inputs":{"X":[1,[4,"0"]],"Y":[1,[4,"0"]]},"fields":{},"shadow":false},
"pu2":{"opcode":"pen_penUp","next":null,"parent":"PD","inputs":{},"fields":{},"shadow":false}
)BLK";
    std::string tail;   // kind4 の2周目・CPU ドット
    switch (kind) {
    case 1:
        loop = R"BLK("LOOP":{"opcode":"control_for_each","next":"dn","parent":"PD","inputs":{"VALUE":[1,[4,"16"]],"SUBSTACK":[2,"LB"]},"fields":{"VARIABLE":["i","vi"]},"shadow":false},)BLK";
        break;
    case 2:
        loop = R"BLK("LOOP":{"opcode":"control_while","next":"dn","parent":"PD","inputs":{"CONDITION":[1,"clt"],"SUBSTACK":[2,"pre_inc"]},"fields":{},"shadow":false},
"clt":{"opcode":"operator_lt","next":null,"parent":"LOOP","inputs":{"OPERAND1":[3,"vir2",[12,"i","vi"]],"OPERAND2":[1,[4,"16"]]},"fields":{},"shadow":false},
"vir2":{"opcode":"data_variable","next":null,"parent":"clt","inputs":{},"fields":{"VARIABLE":["i","vi"]},"shadow":false},)BLK";
        break;
    case 3:
        loop = R"BLK("LOOP":{"opcode":"control_repeat_until","next":"dn","parent":"PD","inputs":{"CONDITION":[1,"cgt"],"SUBSTACK":[2,"pre_inc"]},"fields":{},"shadow":false},
"cgt":{"opcode":"operator_gt","next":null,"parent":"LOOP","inputs":{"OPERAND1":[3,"vir3",[12,"i","vi"]],"OPERAND2":[1,[4,"15"]]},"fields":{},"shadow":false},
"vir3":{"opcode":"data_variable","next":null,"parent":"cgt","inputs":{},"fields":{"VARIABLE":["i","vi"]},"shadow":false},)BLK";
        break;
    case 4:
        loop = R"BLK("LOOP":{"opcode":"control_repeat","next":"cd1","parent":"PD","inputs":{"TIMES":[1,[4,"16"]],"SUBSTACK":[2,"pre_inc"]},"fields":{},"shadow":false},)BLK";
        tail = R"BLK(,
"cd1":{"opcode":"motion_gotoxy","next":"cdg","parent":"PD","inputs":{"X":[1,[4,"0"]],"Y":[1,[4,"0"]]},"fields":{},"shadow":false},
"cdg":{"opcode":"pen_setPenColorToColor","next":"cdp","parent":"PD","inputs":{"COLOR":[1,[4,"65280"]]},"fields":{},"shadow":false},
"cdp":{"opcode":"pen_penDown","next":"cdu","parent":"PD","inputs":{},"fields":{},"shadow":false},
"cdu":{"opcode":"pen_penUp","next":"L2","parent":"PD","inputs":{},"fields":{},"shadow":false},
"L2":{"opcode":"control_repeat","next":"fd1","parent":"PD","inputs":{"TIMES":[1,[4,"16"]],"SUBSTACK":[2,"inc2"]},"fields":{},"shadow":false},
"inc2":{"opcode":"data_changevariableby","next":"b2up","parent":"PD","inputs":{"VALUE":[1,[4,"1"]]},"fields":{"VARIABLE":["i","vi"]},"shadow":false},
"b2up":{"opcode":"pen_penUp","next":"g20","parent":"PD","inputs":{},"fields":{},"shadow":false},
"g20":{"opcode":"motion_gotoxy","next":"sz2","parent":"PD","inputs":{"X":[1,[4,"0"]],"Y":[1,[4,"0"]]},"fields":{},"shadow":false},
"sz2":{"opcode":"pen_setPenSizeTo","next":"ie2","parent":"PD","inputs":{"SIZE":[1,[4,"8"]]},"fields":{},"shadow":false},
"ie2":{"opcode":"control_if_else","next":"tr2","parent":"PD","inputs":{"CONDITION":[1,"eq2"],"SUBSTACK":[2,"cb2"],"SUBSTACK2":[2,"cr2"]},"fields":{},"shadow":false},
"eq2":{"opcode":"operator_equals","next":null,"parent":"ie2","inputs":{"OPERAND1":[1,"md2"],"OPERAND2":[1,[4,"1"]]},"fields":{},"shadow":false},
"md2":{"opcode":"operator_mod","next":null,"parent":"eq2","inputs":{"NUM1":[3,"viref2",[12,"i","vi"]],"NUM2":[1,[4,"2"]]},"fields":{},"shadow":false},
"viref2":{"opcode":"data_variable","next":null,"parent":"md2","inputs":{},"fields":{"VARIABLE":["i","vi"]},"shadow":false},
"cb2":{"opcode":"pen_setPenColorToColor","next":null,"parent":"ie2","inputs":{"COLOR":[1,[4,"255"]]},"fields":{},"shadow":false},
"cr2":{"opcode":"pen_setPenColorToColor","next":null,"parent":"ie2","inputs":{"COLOR":[1,[4,"16711680"]]},"fields":{},"shadow":false},
"tr2":{"opcode":"pen_setPenColorParamTo","next":"pd2","parent":"PD","inputs":{"COLOR_PARAM":[1,"mn2"],"VALUE":[1,[4,"50"]]},"fields":{},"shadow":false},
"mn2":{"opcode":"pen_menu_colorParam","next":null,"parent":"tr2","inputs":{},"fields":{"colorParam":["transparency",null]},"shadow":true},
"pd2":{"opcode":"pen_penDown","next":"g28","parent":"PD","inputs":{},"fields":{},"shadow":false},
"g28":{"opcode":"motion_gotoxy","next":"g20b","parent":"PD","inputs":{"X":[1,[4,"8"]],"Y":[1,[4,"0"]]},"fields":{},"shadow":false},
"g20b":{"opcode":"motion_gotoxy","next":"pu2c","parent":"PD","inputs":{"X":[1,[4,"0"]],"Y":[1,[4,"0"]]},"fields":{},"shadow":false},
"pu2c":{"opcode":"pen_penUp","next":null,"parent":"PD","inputs":{},"fields":{},"shadow":false},
"fd1":{"opcode":"motion_gotoxy","next":"fdb","parent":"PD","inputs":{"X":[1,[4,"0"]],"Y":[1,[4,"0"]]},"fields":{},"shadow":false},
"fdb":{"opcode":"pen_setPenColorToColor","next":"fdt","parent":"PD","inputs":{"COLOR":[1,[4,"255"]]},"fields":{},"shadow":false},
"fdt":{"opcode":"pen_setPenColorParamTo","next":"fdp","parent":"PD","inputs":{"COLOR_PARAM":[1,"fdm"],"VALUE":[1,[4,"50"]]},"fields":{},"shadow":false},
"fdm":{"opcode":"pen_menu_colorParam","next":null,"parent":"fdt","inputs":{},"fields":{"colorParam":["transparency",null]},"shadow":true},
"fdp":{"opcode":"pen_penDown","next":"fdu","parent":"PD","inputs":{},"fields":{},"shadow":false},
"fdu":{"opcode":"pen_penUp","next":"dn","parent":"PD","inputs":{},"fields":{},"shadow":false}
)BLK";
        break;
    default:
        loop = R"BLK("LOOP":{"opcode":"control_repeat","next":"dn","parent":"PD","inputs":{"TIMES":[1,[4,"16"]],"SUBSTACK":[2,"pre_inc"]},"fields":{},"shadow":false},)BLK";
        break;
    }
    std::string s = R"HDR({"targets":[
{"isStage":true,"name":"Stage","variables":{},"lists":{},"broadcasts":{},"blocks":{},"comments":{},"costumes":[],"sounds":[],"currentCostume":0,"volume":100,"layerOrder":0},
{"isStage":false,"name":"S","variables":{"vi":["i",0],"vd":["done",0]},"lists":{},"broadcasts":{},"blocks":{
"hat":{"opcode":"event_whenflagclicked","next":"call","parent":null,"inputs":{},"fields":{},"topLevel":true,"shadow":false,"x":0,"y":0},
"call":{"opcode":"procedures_call","next":null,"parent":"hat","inputs":{},"fields":{},"shadow":false,"mutation":{"tagName":"mutation","children":[],"proccode":"lv","argumentids":"[]","argumentnames":"[]","argumentdefaults":"[]","warp":"true"}},
"PD":{"opcode":"procedures_definition","next":"s0","parent":null,"inputs":{"custom_block":[1,"pp"]},"fields":{},"topLevel":true,"shadow":false,"x":0,"y":40},
"pp":{"opcode":"procedures_prototype","next":null,"parent":"PD","inputs":{},"fields":{},"topLevel":false,"shadow":true,"mutation":{"tagName":"mutation","children":[],"proccode":"lv","argumentids":"[]","argumentnames":"[]","argumentdefaults":"[]","warp":"true"}},
"s0":{"opcode":"data_setvariableto","next":"clr","parent":"PD","inputs":{"VALUE":[1,"zero"]},"fields":{"VARIABLE":["i","vi"]},"shadow":false},
"zero":{"opcode":"operator_add","next":null,"parent":"PD","inputs":{"NUM1":[1,[4,"0"]],"NUM2":[1,[4,"0"]]},"fields":{},"shadow":false},
"clr":{"opcode":"pen_clear","next":"up","parent":"PD","inputs":{},"fields":{},"shadow":false},
"up":{"opcode":"pen_penUp","next":"LOOP","parent":"PD","inputs":{},"fields":{},"shadow":false},
)HDR";
    s += loop;
    s += pre;
    s += body;
    s += tail;
    s += R"TLR(,
"dn":{"opcode":"data_setvariableto","next":null,"parent":"PD","inputs":{"VALUE":[1,[4,"1"]]},"fields":{"VARIABLE":["done","vd"]},"shadow":false}
},"comments":{},"costumes":[],"sounds":[],"currentCostume":0,"x":0,"y":0,"size":100,"direction":90,"rotationStyle":"all around","visible":true,"layerOrder":1}],"monitors":[],"extensions":["pen"],"meta":{"semver":"3.0.0","vm":"0.0.0","agent":""}})TLR";
    return s;
}

bool runLiveFixture(int kind, RendererAPI& ren, Runtime& rt,
                    double* outI, double* outDone) {
    std::string proj = liveProjectJson(kind);
    Sb3LoadResult r = loadSb3(rt, proj.c_str(), nullptr);
    CHECK(r.ok);
    if (!r.ok) return false;
    Target* sp = nullptr;
    for (Target* t : rt.targets)
        if (t->sprite && !t->isStage) { sp = t; break; }
    CHECK(sp != nullptr);
    if (!sp) return false;
    rt.renderer = &ren;
    rt.greenFlag();
    rt.runUntilDone(64);
    Variable* vi = sp->lookupVariableById("vi");
    Variable* vd = sp->lookupVariableById("vd");
    CHECK(vi != nullptr);
    CHECK(vd != nullptr);
    if (!vi || !vd) return false;
    *outI = cast::toNumber(vi->value);
    *outDone = cast::toNumber(vd->value);
    CHECK_EQ(sp->x, 0.0);
    CHECK_EQ(sp->y, 0.0);
    return true;
}

void test_jit_nonfinite_bound() {
    double ci = 0, cd = 0, gi = 0, gd = 0;
    {
        Runtime rt;
        CapRen ren;
        ren.failExec = true;
        CHECK(runLiveFixture(5, ren, rt, &ci, &cd));
        rt.renderer = nullptr;
    }
    {
        Runtime rt;
        CapRen ren;
        CHECK(runLiveFixture(5, ren, rt, &gi, &gd));
        CHECK_EQ(ren.calls, 0);
        rt.renderer = nullptr;
    }
    CHECK_EQ(ci, 16.0);
    CHECK_EQ(cd, 1.0);
    CHECK_EQ(gi, ci);
    CHECK_EQ(gd, cd);
}

} // namespace
namespace xyo {
// SdlRenderer friend 名（test_renderer.cpp と同名の access 構造体 —
// 別バイナリなので独自に定義する）。
struct SdlRendererTestAccess {
    static const uint8_t* penPx(SdlRenderer& r) { return r.penPx_.data(); }
    static int penW(SdlRenderer& r) { return r.penW_; }
    static int penH(SdlRenderer& r) { return r.penH_; }
    static void penSyncCpu(SdlRenderer& r) { r.penSyncCpu(); }
};
}
namespace {
using TA = xyo::SdlRendererTestAccess;

bool CountGpuRen::gpuExecParallel(gpu::ExecRequest& req) {
    ++calls;
    const uint32_t maxPerLane = req.maxPerLane;
    std::vector<uint8_t> before;
    Rng rngBefore = Rng::global();
    if (forceOverflow) {
        TA::penSyncCpu(*this);
        const uint8_t* p = TA::penPx(*this);
        before.assign(p, p + size_t(TA::penW(*this)) *
                                 TA::penH(*this) * 4);
        req.maxPerLane = 1;
    }
    bool ok = SdlRenderer::gpuExecParallel(req);
    req.maxPerLane = maxPerLane;
    if (ok && req.executed) ++executed;
    if (forceOverflow) {
        CHECK(!ok);
        CHECK(!req.executed);
        CHECK(req.overflow);
        CHECK(req.laneOut == nullptr);
        CHECK_EQ(req.nRecords, 0u);
        TA::penSyncCpu(*this);
        CHECK_EQ(before.size(), size_t(TA::penW(*this)) *
                                    TA::penH(*this) * 4);
        CHECK(std::memcmp(before.data(), TA::penPx(*this),
                          before.size()) == 0);
        Rng rngAfter = Rng::global();
        CHECK_EQ(rngBefore.next(), rngAfter.next());
        if (!ok && req.overflow) ++overflowFailures;
    }
    return ok;
}

void writePpm(const char* path, const uint8_t* px, int w, int h) {
    FILE* f = std::fopen(path, "wb");
    CHECK(f != nullptr);
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i)
        std::fwrite(px + i * 4, 1, 3, f);
    std::fclose(f);
}

void test_live_vm() {
    if (!liveGpu()) {
        std::printf("SKIP live vm parallel (XYO_TEST_GPU unset)\n");
        return;
    }
    const char* names[] = {"repeat", "foreach", "while", "until",
                           "two-dispatch", "overflow"};
    for (int kind = 0; kind < 6; ++kind) {
        const int domain = kind == 5 ? 0 : kind;
        std::vector<uint8_t> cpuPx, gpuPx;
        int w = 0, h = 0;
        double ci = 0, cd = 0;
        {
            Runtime rt;
            NoGpuRen ren;
            if (!ren.init(rt, 1)) {
                std::printf("FAIL live %s: renderer init\n",
                            names[kind]);
                ++p_fail; ++p_checks;
                return;
            }
            Target* sp = nullptr;
            CHECK(runLiveFixture(domain, ren, rt, &ci, &cd));
            ren.draw();
            TA::penSyncCpu(ren);
            w = TA::penW(ren); h = TA::penH(ren);
            const uint8_t* p = TA::penPx(ren);
            cpuPx.assign(p, p + size_t(w) * h * 4);
            char path[128];
            std::snprintf(path, sizeof(path),
                          "/tmp/xyo-gpu-check-cpu-%s.ppm", names[kind]);
            writePpm(path, cpuPx.data(), w, h);
            CHECK(ren.calls > 0);
        }
        double gi = 0, gd = 0;
        {
            Runtime rt;
            CountGpuRen ren;
            ren.forceOverflow = kind == 5;
            if (!ren.init(rt, 1)) {
                std::printf("FAIL live %s gpu: renderer init\n",
                            names[kind]);
                ++p_fail; ++p_checks;
                return;
            }
            Target* sp = nullptr;
            CHECK(runLiveFixture(domain, ren, rt, &gi, &gd));
            ren.draw();
            TA::penSyncCpu(ren);
            CHECK_EQ(TA::penW(ren), w);
            CHECK_EQ(TA::penH(ren), h);
            const uint8_t* p = TA::penPx(ren);
            gpuPx.assign(p, p + size_t(w) * h * 4);
            char path[128];
            std::snprintf(path, sizeof(path),
                          "/tmp/xyo-gpu-check-gpu-%s.ppm", names[kind]);
            writePpm(path, gpuPx.data(), w, h);
            if (kind == 5) {
                CHECK_EQ(ren.executed, 0);
                CHECK(ren.overflowFailures > 0);
            } else {
                CHECK(ren.executed > 0);
            }
            if (kind == 4) CHECK(ren.executed >= 2);
        }
        if (kind != 4) {
            CHECK_EQ(ci, 16.0);
            CHECK_EQ(gi, 16.0);
        } else {
            CHECK_EQ(ci, 32.0);
            CHECK_EQ(gi, 32.0);
        }
        CHECK_EQ(cd, 1.0);
        CHECK_EQ(gd, 1.0);
        CHECK_EQ(cpuPx.size(), gpuPx.size());
        if (cpuPx.size() == gpuPx.size() && !cpuPx.empty()) {
            size_t diff = 0;
            for (size_t i = 0; i < cpuPx.size(); ++i)
                if (cpuPx[i] != gpuPx[i]) ++diff;
            CHECK_EQ(diff, 0u);
            // 中央ピクセルは最終反復（青）が優勢 — 両空白検証を防ぐ
            size_t c = (size_t(h / 2) * w + w / 2) * 4;
            CHECK(gpuPx[c + 2] > gpuPx[c]);
        }
    }
}
#endif // XYO_JIT

} // namespace

int test_parallel_gpu() {
    // ヘルパ内部の static 初期化より先に最小レーン閾値を下げる
    setenv("XYO_GPU_MIN", "1", 0);
    test_order();
    test_cfg_locals();
    test_cfg_recurrence();
    test_cfg_inner_loops();
    test_cfg_rejects();
    test_cfg_proc();
    test_cfg_pen();
    test_bounds();
    test_domains_compile();
#if defined(XYO_JIT)
    test_helper_repeat_for_each();
    test_helper_while();
    test_helper_writeback();
    test_helper_unresolved();
    test_jit_nonfinite_bound();
    test_live_vm();
#endif
    std::printf("parallel gpu tests: %d checks, %d failures\n",
                p_checks, p_fail);
    return p_fail;
}
