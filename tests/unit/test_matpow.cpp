// xyo-cpp: 線形漸化式ループの行列累乗（matpow）のテスト。
//   - 検出器: fib 形 / 単純 change / 線形合成の受理、非 affine 形の拒否
//   - 実行: warp 手続き内の repeat が xyo_matpow で O(log n) に置き換わり、
//     変数の最終値が C++ で逐次シミュレートした値と一致する
//   - 拒否: 初期値が非整数・-0・範囲外のとき逐次へフォールバックし、
//     結果が逐次実行と一致する
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/cast.hpp"
#include "core/value.hpp"
#include "engine/runtime.hpp"
#include "jit/jit.hpp"
#include "jit/matpow.hpp"
#if defined(XYO_JIT) && !defined(__EMSCRIPTEN__)
#include "mlir/frontend.hpp"
#endif
#include "load/sb3.hpp"
#include "load/zip.hpp"
#include "model/blocks.hpp"
#include "model/target.hpp"

using namespace xyo;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; \
    if (!((a) == (b))) { ++g_fail; \
    std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, \
                #a, #b, (long long)(a), (long long)(b)); } } while (0)

extern "C" int64_t xyo_matpow_runs;
extern "C" int64_t xyo_matpow_rejects;

namespace {

// --- 手組みブロック木で検出器を試す ---------------------------------------

struct Fixture {
    Runtime rt;
    Sprite sprite{&rt};
    Target target{&rt, &sprite};

    Fixture() {
        addVar("va", "a");
        addVar("vb", "b");
        addVar("vt", "t");
        addVar("vx", "x");
    }
    static Block make(const char* id, const char* opcode, const char* next = "",
                      bool shadow = false) {
        Block b;
        b.id = id;
        b.opcode = opcode;
        b.next = next;
        b.shadow = shadow;
        return b;
    }
    void add(Block b) { sprite.blocks->createBlock(std::move(b)); }
    void addVar(const char* id, const char* name) {
        target.variables.emplace(id, Variable::create(id, utf8ToU16(name),
                                                      Variable::kScalarType,
                                                      false));
    }
    Block* blk(const char* id) { return sprite.blocks->getBlock(id); }
    void numShadow(const char* id, double v) {
        add(make(id, "math_number", "", true));
        blk(id)->fields["NUM"] = BlockField{Value::num(v), ""};
    }
    void varField(Block* b, const char* varName, const char* varId) {
        b->fields["VARIABLE"] =
            BlockField{Value::string(SStr::create(utf8ToU16(varName))), varId};
    }
    /// "a + b" の operator_add を作り id で登録
    void addExpr(const char* id, const char* op, const char* l, const char* r) {
        add(make(id, op));
        blk(id)->inputs.set("NUM1", BlockInput{l, ""});
        blk(id)->inputs.set("NUM2", BlockInput{r, ""});
    }
    void varReporter(const char* id, const char* varName, const char* varId) {
        add(make(id, "data_variable"));
        varField(blk(id), varName, varId);
    }

    /// repeat(n) { set t to a+b; set a to b; set b to t } — fib
    Block* buildFib() {
        numShadow("n", 5);
        add(make("loop", "control_repeat"));
        blk("loop")->inputs.set("TIMES", BlockInput{"n", ""});
        blk("loop")->inputs.set("SUBSTACK", BlockInput{"s1", ""});
        varReporter("va_r", "a", "va");
        varReporter("vb_r", "b", "vb");
        varReporter("vb_r2", "b", "vb");
        varReporter("vt_r", "t", "vt");
        addExpr("sum", "operator_add", "va_r", "vb_r");
        add(make("s1", "data_setvariableto", "s2"));
        varField(blk("s1"), "t", "vt");
        blk("s1")->inputs.set("VALUE", BlockInput{"sum", ""});
        add(make("s2", "data_setvariableto", "s3"));
        varField(blk("s2"), "a", "va");
        blk("s2")->inputs.set("VALUE", BlockInput{"vb_r2", ""});
        add(make("s3", "data_setvariableto"));
        varField(blk("s3"), "b", "vb");
        blk("s3")->inputs.set("VALUE", BlockInput{"vt_r", ""});
        return blk("loop");
    }
    /// repeat(n) { change x by 1 }
    Block* buildIncr() {
        numShadow("n", 3);
        numShadow("one", 1);
        add(make("loop", "control_repeat"));
        blk("loop")->inputs.set("TIMES", BlockInput{"n", ""});
        blk("loop")->inputs.set("SUBSTACK", BlockInput{"c", ""});
        add(make("c", "data_changevariableby"));
        varField(blk("c"), "x", "vx");
        blk("c")->inputs.set("VALUE", BlockInput{"one", ""});
        return blk("loop");
    }
};

void testDetect() {
    // fib 形の受理: vars = {t,a,b}（出現順）、行列は 3x3 + affine 列
    {
        Fixture f;
        Block* loop = f.buildFib();
        std::string why;
        auto plan = jit::analyzeMatPow(*f.sprite.blocks, loop, &why);
        CHECK(plan.has_value());
        if (!plan) return;
        CHECK_EQ(plan->vars.size(), 3u);
        const int dim = 4;
        CHECK_EQ(plan->mat.size(), size_t(dim * dim));
        // 状態を (t,a,b) とすると T の拡大行列は
        //   [0 1 1 | 0]   t' = a + b
        //   [0 0 1 | 0]   a' = b
        //   [0 1 1 | 0]   b' = t（更新後の t = a+b）
        //   [0 0 0 | 1]
        const int64_t want[16] = {0,1,1,0, 0,0,1,0, 0,1,1,0, 0,0,0,1};
        for (int i = 0; i < 16; ++i) CHECK_EQ(plan->mat[size_t(i)], want[i]);
    }
    // change x by 1 → [[1,1],[0,1]]
    {
        Fixture f;
        Block* loop = f.buildIncr();
        std::string why;
        auto plan = jit::analyzeMatPow(*f.sprite.blocks, loop, &why);
        CHECK(plan.has_value());
        if (plan) {
            CHECK_EQ(plan->vars.size(), 1u);
            CHECK_EQ(plan->mat.size(), 4u);
            CHECK_EQ(plan->mat[0], 1);
            CHECK_EQ(plan->mat[1], 1);
            CHECK_EQ(plan->mat[2], 0);
            CHECK_EQ(plan->mat[3], 1);
        }
    }
    // var * var（非線形）→ 拒否
    {
        Fixture f;
        f.buildIncr();
        f.addExpr("mul", "operator_multiply", "va_r", "vb_r");
        f.varReporter("va_r", "a", "va");
        f.varReporter("vb_r", "b", "vb");
        f.blk("c")->opcode = "data_setvariableto";
        f.varField(f.blk("c"), "x", "vx");
        f.blk("c")->inputs.set("VALUE", BlockInput{"mul", ""});
        std::string why;
        CHECK(!jit::analyzeMatPow(*f.sprite.blocks, f.blk("loop"), &why)
                   .has_value());
        CHECK(why.find("nonlinear") != std::string::npos);
    }
    // x/2（非整数係数）→ 拒否
    {
        Fixture f;
        f.buildIncr();
        f.numShadow("two", 2);
        f.varReporter("vx_r", "x", "vx");
        f.addExpr("dv", "operator_divide", "vx_r", "two");
        f.blk("c")->opcode = "data_setvariableto";
        f.blk("c")->inputs.set("VALUE", BlockInput{"dv", ""});
        std::string why;
        CHECK(!jit::analyzeMatPow(*f.sprite.blocks, f.blk("loop"), &why)
                   .has_value());
        CHECK(why.find("divide") != std::string::npos);
    }
    // (x*3)/3 は受理（整数係数に正規化される）
    {
        Fixture f;
        f.buildIncr();
        f.numShadow("three", 3);
        f.numShadow("three2", 3);
        f.varReporter("vx_r", "x", "vx");
        f.addExpr("m3", "operator_multiply", "vx_r", "three");
        f.addExpr("d3", "operator_divide", "m3", "three2");
        f.blk("c")->opcode = "data_setvariableto";
        f.blk("c")->inputs.set("VALUE", BlockInput{"d3", ""});
        std::string why;
        auto plan = jit::analyzeMatPow(*f.sprite.blocks, f.blk("loop"), &why);
        CHECK(plan.has_value());
        if (plan) { CHECK_EQ(plan->mat[0], 1); CHECK_EQ(plan->mat[1], 0); }
    }
    // mod（非 affine）→ 拒否
    {
        Fixture f;
        f.buildIncr();
        f.numShadow("seven", 7);
        f.varReporter("vx_r", "x", "vx");
        f.addExpr("md", "operator_mod", "vx_r", "seven");
        f.blk("c")->opcode = "data_setvariableto";
        f.blk("c")->inputs.set("VALUE", BlockInput{"md", ""});
        CHECK(!jit::analyzeMatPow(*f.sprite.blocks, f.blk("loop"), nullptr)
                   .has_value());
    }
    // 制御構文（if）→ 拒否
    {
        Fixture f;
        f.buildIncr();
        f.add(Fixture::make("iff", "control_if"));
        f.blk("c")->next = "iff";
        CHECK(!jit::analyzeMatPow(*f.sprite.blocks, f.blk("loop"), nullptr)
                   .has_value());
    }
    // 非定数 TIMES の内側 repeat → 拒否、定数 TIMES → 受理
    {
        Fixture f;
        // repeat(2) { change x by 2 } を内側に持つ
        f.numShadow("n", 4);
        f.numShadow("two", 2);
        f.numShadow("k", 2);
        f.add(Fixture::make("loop", "control_repeat"));
        f.blk("loop")->inputs.set("TIMES", BlockInput{"n", ""});
        f.blk("loop")->inputs.set("SUBSTACK", BlockInput{"inner", ""});
        f.add(Fixture::make("inner", "control_repeat"));
        f.blk("inner")->inputs.set("TIMES", BlockInput{"two", ""});
        f.blk("inner")->inputs.set("SUBSTACK", BlockInput{"ic", ""});
        f.add(Fixture::make("ic", "data_changevariableby"));
        f.varField(f.blk("ic"), "x", "vx");
        f.blk("ic")->inputs.set("VALUE", BlockInput{"k", ""});
        std::string why;
        auto plan = jit::analyzeMatPow(*f.sprite.blocks, f.blk("loop"), &why);
        CHECK(plan.has_value());
        if (plan) {
            // x += 2 を 2 回 = x += 4
            CHECK_EQ(plan->mat[0], 1);
            CHECK_EQ(plan->mat[1], 4);
        }
    }
    // リスト読み → 拒否
    {
        Fixture f;
        f.buildIncr();
        f.add(Fixture::make("rd", "data_itemoflist"));
        f.blk("c")->inputs.set("VALUE", BlockInput{"rd", ""});
        CHECK(!jit::analyzeMatPow(*f.sprite.blocks, f.blk("loop"), nullptr)
                   .has_value());
    }
}

// --- 実行検証 -------------------------------------------------------------

/// warp 手続き run() 内に repeat(argN) { <body json> } を置いたプロジェクト。
/// vars: a,b,t,x（全てスプライトローカル、初期値はセット文で入れる）。
std::string makeProject(const std::string& initStmtsIn,
                        const std::string& bodyStmtsIn,
                        const std::string& times) {
    // init 断片は次に "loop" が続くので末尾カンマが必要。
    // body 断片はブロック辞書の最後なので末尾の余分なカンマを取る。
    auto trimComma = [](std::string s) {
        while (!s.empty() && (s.back() == ',' || s.back() == '\n' ||
                              s.back() == ' '))
            s.pop_back();
        return s;
    };
    const std::string& initStmts = initStmtsIn;
    const std::string bodyStmts = trimComma(bodyStmtsIn);
    return std::string(R"JSON({"targets": [
    {"isStage": true, "name": "Stage", "variables": {}, "lists": {},
     "broadcasts": {}, "blocks": {}, "comments": {}, "costumes": [],
     "sounds": [], "currentCostume": 0, "volume": 100, "layerOrder": 0},
    {"isStage": false, "name": "S", "layerOrder": 1,
     "variables": {"va": ["a", 0], "vb": ["b", 0], "vt": ["t", 0],
                   "vx": ["x", 0]},
     "lists": {}, "broadcasts": {},
     "blocks": {
       "hat": {"opcode": "event_whenflagclicked", "next": "call",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 0, "y": 0},
       "call": {"opcode": "procedures_call", "next": null, "parent": "hat",
                "inputs": {"argN": [1, "argn"]}, "fields": {},
                "topLevel": false, "shadow": false,
                "mutation": {"tagName": "mutation", "children": [],
                             "proccode": "run %s", "argumentids": "[\"argN\"]",
                             "argumentnames": "[\"n\"]",
                             "argumentdefaults": "[\"0\"]", "warp": "true"}},
       "argn": {"opcode": "math_number", "next": null, "parent": "call",
                "inputs": {}, "fields": {"NUM": [)JSON") +
           times +
           std::string(R"JSON(, null]},
                "topLevel": false, "shadow": true},
       "def": {"opcode": "procedures_definition", "next": "init0",
               "parent": null, "inputs": {"custom_block": [1, "proto"]},
               "fields": {}, "topLevel": true, "shadow": false, "x": 0, "y": 400},
       "proto": {"opcode": "procedures_prototype", "next": null, "parent": "def",
                 "inputs": {"argN": [1, "argn2"]}, "fields": {},
                 "topLevel": false, "shadow": true,
                 "mutation": {"tagName": "mutation", "children": [],
                              "proccode": "run %s",
                              "argumentids": "[\"argN\"]",
                              "argumentnames": "[\"n\"]",
                              "argumentdefaults": "[\"0\"]", "warp": "true"}},
       "argn2": {"opcode": "argument_reporter_string_number", "next": null,
                 "parent": "proto", "inputs": {},
                 "fields": {"VALUE": ["n", null]},
                 "topLevel": false, "shadow": true},
       )JSON") +
           initStmts +
           std::string(R"JSON(
       "loop": {"opcode": "control_repeat", "next": null, "parent": "last",
                "inputs": {"TIMES": [3, "argr", [6, "0"]],
                           "SUBSTACK": [2, "body0"]},
                "fields": {}, "topLevel": false, "shadow": false},
       "argr": {"opcode": "argument_reporter_string_number", "next": null,
                "parent": "loop", "inputs": {}, "fields": {"VALUE": ["n", null]},
                "topLevel": false, "shadow": false},
       )JSON") +
           bodyStmts +
           std::string(R"JSON(
     },
     "comments": {}, "costumes": [], "sounds": [], "currentCostume": 0,
     "volume": 100, "visible": true, "x": 0, "y": 0, "size": 100,
     "direction": 90, "draggable": false, "rotationStyle": "all around"}
  ],
  "monitors": [], "extensions": [], "meta": {"semver": "3.0.0"}
})JSON");
}

/// set <var> to <数値リテラル> のチェーン断片。"init0" から始め、
/// 最後は "last" を loop の前に置く。initStmts はブロック定義列（末尾に
/// "last" id のブロックが loop へ繋がるよう next を持つ）。
std::string initSet(const char* id, const char* var, const char* varId,
                    const char* num, const char* nextId) {
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  R"JSON("%s": {"opcode": "data_setvariableto",
                "next": "%s", "parent": "def",
                "inputs": {"VALUE": [1, [4, "%s"]]},
                "fields": {"VARIABLE": ["%s", "%s"]},
                "topLevel": false, "shadow": false},)JSON",
                  id, nextId, num, var, varId);
    return buf;
}

std::string setStmt(const char* id, const char* var, const char* varId,
                    const std::string& valueInput, const char* nextId) {
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  R"JSON("%s": {"opcode": "data_setvariableto",
                "next": %s, "parent": "loop",
                "inputs": {"VALUE": %s},
                "fields": {"VARIABLE": ["%s", "%s"]},
                "topLevel": false, "shadow": false},)JSON",
                  id, nextId[0] ? (std::string("\"") + nextId + "\"").c_str()
                                : "null",
                  valueInput.c_str(), var, varId);
    return buf;
}
std::string changeStmt(const char* id, const char* var, const char* varId,
                       const std::string& valueInput, const char* nextId) {
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  R"JSON("%s": {"opcode": "data_changevariableby",
                "next": %s, "parent": "loop",
                "inputs": {"VALUE": %s},
                "fields": {"VARIABLE": ["%s", "%s"]},
                "topLevel": false, "shadow": false},)JSON",
                  id, nextId[0] ? (std::string("\"") + nextId + "\"").c_str()
                                : "null",
                  valueInput.c_str(), var, varId);
    return buf;
}
std::string binopBlock(const char* id, const char* op,
                       const std::string& n1, const std::string& n2) {
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  R"JSON("%s": {"opcode": "%s", "next": null, "parent": "loop",
                "inputs": {"NUM1": %s, "NUM2": %s},
                "fields": {},
                "topLevel": false, "shadow": false},)JSON",
                  id, op, n1.c_str(), n2.c_str());
    return buf;
}
std::string varRef(const char* id, const char* var, const char* varId) {
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  R"JSON("%s": {"opcode": "data_variable", "next": null,
                "parent": "loop", "inputs": {},
                "fields": {"VARIABLE": ["%s", "%s"]},
                "topLevel": false, "shadow": false},)JSON",
                  id, var, varId);
    return buf;
}
std::string refInput(const char* blockId) {
    return std::string("[3, \"") + blockId + "\", [4, \"0\"]]";
}
std::string numInput(const char* numStr) {
    return std::string("[1, [4, \"") + numStr + "\"]]";
}

std::vector<uint8_t> zipWithProject(const std::string& json) {
    ZipWriter zw;
    zw.addFile("project.json",
               std::vector<uint8_t>(json.begin(), json.end()));
    return zw.finish();
}

struct RunResult {
    bool ok = false;
    double a = 0, b = 0, t = 0, x = 0;
    int64_t runs = 0, rejects = 0;
};

/// bodyStmts の先頭ブロック id は "body0"。initStmts は "init0"→"last" の
/// 鎖で、loop の parent に "last" を置く（seti の next が loop）。
RunResult runProject(const std::string& initStmts,
                     const std::string& bodyStmts,
                     const std::string& times) {
    const int64_t runsBefore = xyo_matpow_runs;
    const int64_t rejBefore = xyo_matpow_rejects;
    // initStmts の最後のブロック id は "last" で next="loop"。
    // def の next は "init0"。body の先頭 id は "body0"。
    std::string proj = makeProject(initStmts, bodyStmts, times);
    const auto sb3 = zipWithProject(proj);
    Runtime rt;
    RunResult res;
    Sb3LoadResult lr = loadSb3File(rt, sb3.data(), sb3.size());
    CHECK(lr.ok);
    if (!lr.ok) return res;
    rt.greenFlag();
    rt.runUntilDone(10000);
    Target* sp = rt.getSpriteTargetByName(u"S");
    CHECK(sp != nullptr);
    if (!sp) return res;
    auto getNum = [&](const char* id) -> double {
        Variable* v = sp->lookupVariableById(id);
        return v ? cast::toNumber(v->value) : -1.0;
    };
    res.a = getNum("va");
    res.b = getNum("vb");
    res.t = getNum("vt");
    res.x = getNum("vx");
    res.runs = xyo_matpow_runs - runsBefore;
    res.rejects = xyo_matpow_rejects - rejBefore;
    res.ok = true;
    return res;
}

// fib: repeat(n) { t=a+b; a=b; b=t }（a=0,b=1 → b = fib(n+1)）
std::string fibInit() {
    return initSet("init0", "a", "va", "0", "init1") +
           initSet("init1", "b", "vb", "1", "init2") +
           initSet("init2", "x", "vx", "0", "last") +
           initSet("last", "t", "vt", "0", "loop");
}
std::string fibBody() {
    return setStmt("body0", "t", "vt", "[3, \"sum\", [4, \"0\"]]", "body1") +
           setStmt("body1", "a", "va", refInput("b_r"), "body2") +
           setStmt("body2", "b", "vb", refInput("t_r"), "") +
           binopBlock("sum", "operator_add", refInput("a_r"), refInput("b_r")) +
           varRef("a_r", "a", "va") + varRef("b_r", "b", "vb") +
           varRef("t_r", "t", "vt");
}

void testRunFib() {
    // n = 50: fib(51) = 20365011074
    RunResult r = runProject(fibInit(), fibBody(), "50");
    CHECK(r.ok);
    // C++ 側の逐次シミュレーションで期待値を検算
    double a = 0, b = 1;
    for (int i = 0; i < 50; ++i) { double t = a + b; a = b; b = t; }
    CHECK_EQ(r.b, b);
    CHECK_EQ(r.a, a);
    CHECK(r.b == 20365011074.0);
#ifndef XYO_JIT_DISABLED
    if (jitEnabled()) {
        CHECK(r.runs >= 1);   // 行列累乗で実行された
        CHECK_EQ(r.rejects, 0);
    }
#endif
}

void testRunIncr() {
    // repeat(n) { change x by 1 }（x=0, n=1000000 → 1e6、O(log n)）
    std::string init = initSet("init0", "x", "vx", "0", "last") +
                       initSet("last", "a", "va", "0", "loop");
    std::string body =
        changeStmt("body0", "x", "vx", numInput("1"), "");
    RunResult r = runProject(init, body, "1000000");
    CHECK(r.ok);
    CHECK_EQ(r.x, 1000000.0);
#ifndef XYO_JIT_DISABLED
    if (jitEnabled()) CHECK(r.runs >= 1);
#endif
}

void testRunAffine() {
    // repeat(n) { set x to 2*x+3 }（x=1 → 反復で 2^n 増殖、bound で
    // 途中から拒否される。n=20 なら 2^21 程度で受理範囲内）
    std::string init = initSet("init0", "x", "vx", "1", "last") +
                       initSet("last", "a", "va", "0", "loop");
    std::string body =
        setStmt("body0", "x", "vx", "[3, \"expr\", [4, \"0\"]]", "") +
        binopBlock("expr", "operator_add", "[3, \"m2\", [4, \"0\"]]",
                   numInput("3")) +
        binopBlock("m2", "operator_multiply", refInput("x_r"),
                   numInput("2")) +
        varRef("x_r", "x", "vx");
    RunResult r = runProject(init, body, "20");
    CHECK(r.ok);
    double x = 1;
    for (int i = 0; i < 20; ++i) x = 2 * x + 3;
    CHECK_EQ(r.x, x);   // 4*2^20 - 3 = 4194301
    CHECK(r.x == 4194301.0);
#ifndef XYO_JIT_DISABLED
    if (jitEnabled()) CHECK(r.runs >= 1);
#endif
}

void testRunBoundReject() {
    // n = 40: 2^41 → 2.2e12 × 係数等の bound で 4e15 未満か?
    // 逐次値: x = 4*2^40 - 3 = 4398046511101 ≈ 4.4e12 → bound =
    // 2*2*4.4e12*P*1。P ≈ ||A^e||max ≈ 5 → 8.8e13 < 4e15 → 受理される。
    // n = 60: x ≈ 4.6e15 → bound 超過 → 逐次へ（結果は一致する）。
    std::string init = initSet("init0", "x", "vx", "1", "last") +
                       initSet("last", "a", "va", "0", "loop");
    std::string body =
        setStmt("body0", "x", "vx", "[3, \"expr\", [4, \"0\"]]", "") +
        binopBlock("expr", "operator_add", "[3, \"m2\", [4, \"0\"]]",
                   numInput("3")) +
        binopBlock("m2", "operator_multiply", refInput("x_r"),
                   numInput("2")) +
        varRef("x_r", "x", "vx");
    RunResult r = runProject(init, body, "60");
    CHECK(r.ok);
    double x = 1;
    for (int i = 0; i < 60; ++i) x = 2 * x + 3;
    CHECK_EQ(r.x, x);
    // 結果が範囲外（4.6e15 > 4.0e15）なら必ず逐次へ落ちたはず
#ifndef XYO_JIT_DISABLED
    if (jitEnabled() && x > 4.0e15) CHECK(r.runs == 0 || r.rejects >= 1);
#endif
}

void testRunNonIntegerFallback() {
    // 初期値が非整数 → 逐次フォールバック（結果は逐次と一致）
    // repeat(n) { change x by 1 }、x = 0.5 から開始 → x = 5.5
    std::string init = initSet("init0", "x", "vx", "0.5", "last") +
                       initSet("last", "a", "va", "0", "loop");
    std::string body =
        changeStmt("body0", "x", "vx", numInput("1"), "");
    RunResult r = runProject(init, body, "5");
    CHECK(r.ok);
    CHECK_EQ(r.x, 5.5);
#ifndef XYO_JIT_DISABLED
    if (jitEnabled()) CHECK_EQ(r.runs, 0);
#endif
}

void testRunZeroTrips() {
    // n = 0（0 反復）: 変数は一切変わらず、書き戻しも行われない
    std::string init = initSet("init0", "x", "vx", "7", "last") +
                       initSet("last", "a", "va", "0", "loop");
    std::string body =
        changeStmt("body0", "x", "vx", numInput("1"), "");
    RunResult r = runProject(init, body, "0");
    CHECK(r.ok);
    CHECK_EQ(r.x, 7.0);
}

void testRunNonWarpLoop() {
    // 非 warp 手続きの同じループは matpow 対象外（warp ゲート）。
    // 意味論は同じ — 結果が一致することだけ見る。
    std::string proj = makeProject(
        initSet("init0", "x", "vx", "0", "last") +
            initSet("last", "a", "va", "0", "loop"),
        changeStmt("body0", "x", "vx", numInput("1"), ""), "100");
    // warp → false に差し替え
    size_t pos = 0;
    while ((pos = proj.find("\"warp\": \"true\"", pos)) != std::string::npos) {
        proj.replace(pos, 14, "\"warp\": \"false\"");
        pos += 15;
    }
    const auto sb3 = zipWithProject(proj);
    Runtime rt;
    Sb3LoadResult lr = loadSb3File(rt, sb3.data(), sb3.size());
    CHECK(lr.ok);
    if (!lr.ok) return;
    rt.greenFlag();
    rt.runUntilDone(10000);
    Target* sp = rt.getSpriteTargetByName(u"S");
    CHECK(sp != nullptr);
    if (sp) {
        Variable* v = sp->lookupVariableById("vx");
        CHECK(v && cast::toNumber(v->value) == 100.0);
    }
}

void testRunNested() {
    // repeat(n) { repeat(2) { change x by 2 } } — 内側畳み込み x += 4*n
    std::string init = initSet("init0", "x", "vx", "0", "last") +
                       initSet("last", "a", "va", "0", "loop");
    std::string body =
        std::string(
            R"JSON("body0": {"opcode": "control_repeat", "next": null,
            "parent": "loop",
            "inputs": {"TIMES": [1, [4, "2"]],
                       "SUBSTACK": [2, "inner0"]},
            "fields": {}, "topLevel": false, "shadow": false},)JSON") +
        changeStmt("inner0", "x", "vx", numInput("2"), "");
    RunResult r = runProject(init, body, "1000");
    CHECK(r.ok);
    CHECK_EQ(r.x, 4000.0);
#ifndef XYO_JIT_DISABLED
    if (jitEnabled()) CHECK(r.runs >= 1);
#endif
}


// --- 条件付きループ（repeat until / while）-------------------------------

/// repeat until / while ループのプロジェクト。ループ後に i と s を
/// リスト L へ追記して結果を観測できるようにする。
std::string makeCondProject(const std::string& loopOp,
                            const std::string& condOp,
                            const std::string& condRhsNum,
                            const std::string& initStmts,
                            const std::string& bodyStmts) {
    auto trimComma = [](std::string x) {
        while (!x.empty() && (x.back() == ',' || x.back() == '\n' ||
                              x.back() == ' '))
            x.pop_back();
        return x;
    };
    const std::string body = trimComma(bodyStmts);
    return std::string(R"JSON({"targets": [
    {"isStage": true, "name": "Stage", "variables": {}, "lists": {},
     "broadcasts": {}, "blocks": {}, "comments": {}, "costumes": [],
     "sounds": [], "currentCostume": 0, "volume": 100, "layerOrder": 0},
    {"isStage": false, "name": "S", "layerOrder": 1,
     "variables": {"va": ["a", 0], "vb": ["b", 0], "vt": ["t", 0],
                   "vx": ["x", 0], "vi": ["i", 0], "vs": ["s", 0],
                   "vk": ["k", 0]},
     "lists": {"vL": ["L", []]}, "broadcasts": {},
     "blocks": {
       "hat": {"opcode": "event_whenflagclicked", "next": "call",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 0, "y": 0},
       "call": {"opcode": "procedures_call", "next": null, "parent": "hat",
                "inputs": {}, "fields": {},
                "topLevel": false, "shadow": false,
                "mutation": {"tagName": "mutation", "children": [],
                             "proccode": "run", "argumentids": "[]",
                             "argumentnames": "[]",
                             "argumentdefaults": "[]", "warp": "true"}},
       "def": {"opcode": "procedures_definition", "next": "init0",
               "parent": null, "inputs": {"custom_block": [1, "proto"]},
               "fields": {}, "topLevel": true, "shadow": false, "x": 0, "y": 400},
       "proto": {"opcode": "procedures_prototype", "next": null, "parent": "def",
                 "inputs": {}, "fields": {},
                 "topLevel": false, "shadow": true,
                 "mutation": {"tagName": "mutation", "children": [],
                              "proccode": "run", "argumentids": "[]",
                              "argumentnames": "[]",
                              "argumentdefaults": "[]", "warp": "true"}},
       )JSON") +
           initStmts + std::string(R"JSON(
       "loop": {"opcode": ")JSON") + loopOp + std::string(R"JSON(", "next": "tail0",
                "parent": "last",
                "inputs": {"CONDITION": [2, "cond"],
                           "SUBSTACK": [2, "body0"]},
                "fields": {}, "topLevel": false, "shadow": false},
       "cond": {"opcode": ")JSON") + condOp + std::string(R"JSON(", "next": null,
                "parent": "loop",
                "inputs": {"OPERAND1": [3, "i_r", [4, "0"]],
                           "OPERAND2": [3, "rhs", [4, "0"]]},
                "fields": {}, "topLevel": false, "shadow": false},
       "i_r": {"opcode": "data_variable", "next": null, "parent": "cond",
               "inputs": {}, "fields": {"VARIABLE": ["i", "vi"]},
               "topLevel": false, "shadow": false},
       "rhs": {"opcode": "math_number", "next": null, "parent": "cond",
               "inputs": {}, "fields": {"NUM": [)JSON") + condRhsNum +
           std::string(R"JSON(, null]},
               "topLevel": false, "shadow": true},
       "tail0": {"opcode": "data_addtolist", "next": "tail1", "parent": "loop",
                 "inputs": {"ITEM": [3, "ti", [4, "0"]]},
                 "fields": {"LIST": ["L", "vL"]}, "topLevel": false,
                 "shadow": false},
       "ti": {"opcode": "data_variable", "next": null, "parent": "tail0",
              "inputs": {}, "fields": {"VARIABLE": ["i", "vi"]},
              "topLevel": false, "shadow": false},
       "tail1": {"opcode": "data_addtolist", "next": null, "parent": "tail0",
                 "inputs": {"ITEM": [3, "ts", [4, "0"]]},
                 "fields": {"LIST": ["L", "vL"]}, "topLevel": false,
                 "shadow": false},
       "ts": {"opcode": "data_variable", "next": null, "parent": "tail1",
              "inputs": {}, "fields": {"VARIABLE": ["s", "vs"]},
              "topLevel": false, "shadow": false},
       )JSON") +
           body + std::string(R"JSON(
     },
     "comments": {}, "costumes": [], "sounds": [], "currentCostume": 0,
     "volume": 100, "visible": true, "x": 0, "y": 0, "size": 100,
     "direction": 90, "draggable": false, "rotationStyle": "all around"}
  ],
  "monitors": [], "extensions": [], "meta": {"semver": "3.0.0"}
})JSON");
}

struct CondResult {
    bool ok = false;
    std::vector<double> list;
    int64_t runs = 0;
};

CondResult runCondProject(const std::string& loopOp, const std::string& condOp,
                          const std::string& rhs, const std::string& init,
                          const std::string& body) {
    CondResult res;
    const int64_t runsBefore = xyo_matpow_runs;
    const std::string proj = makeCondProject(loopOp, condOp, rhs, init, body);
    const auto sb3 = zipWithProject(proj);
    Runtime rt;
    Sb3LoadResult lr = loadSb3File(rt, sb3.data(), sb3.size());
    CHECK(lr.ok);
    if (!lr.ok) return res;
    rt.greenFlag();
    rt.runUntilDone(100000);
    Target* sp = rt.getSpriteTargetByName(u"S");
    CHECK(sp != nullptr);
    if (!sp) return res;
    Variable* L = sp->lookupVariableById("vL");
    if (L && L->list)
        for (uint32_t i = 0; i < L->list->len; ++i)
            res.list.push_back(cast::toNumber(L->list->items[i]));
    res.runs = xyo_matpow_runs - runsBefore;
    res.ok = true;
    return res;
}

void testRunUntilCountdown() {
    // repeat until i < 0 { change i by -1; change s by i }
    const std::string init =
        initSet("init0", "i", "vi", "1000000", "init1") +
        initSet("init1", "s", "vs", "0", "last") +
        initSet("last", "x", "vx", "0", "loop");
    const std::string body =
        changeStmt("body0", "i", "vi", numInput("-1"), "body1") +
        changeStmt("body1", "s", "vs", refInput("i_r2"), "") +
        varRef("i_r2", "i", "vi");
    CondResult r = runCondProject("control_repeat_until", "operator_lt", "0",
                                  init, body);
    CHECK(r.ok);
    // Scratch の逐次意味論をそのまま C++ で回した期待値
    double i = 1000000, sv = 0;
    while (!(i < 0)) { i -= 1; sv += i; }
    CHECK_EQ(r.list.size(), 2u);
    if (r.list.size() == 2) {
        CHECK_EQ(r.list[0], i);
        CHECK_EQ(r.list[1], sv);
    }
#ifndef XYO_JIT_DISABLED
    if (jitEnabled()) CHECK(r.runs >= 1);  // 行列累乗（二分探索）で実行
#endif
}

void testRunWhileCountdown() {
    // repeat while i > 0 { change i by -1; change s by 1 }
    const std::string init =
        initSet("init0", "i", "vi", "2000000", "init1") +
        initSet("init1", "s", "vs", "0", "last") +
        initSet("last", "x", "vx", "0", "loop");
    const std::string body =
        changeStmt("body0", "i", "vi", numInput("-1"), "body1") +
        changeStmt("body1", "s", "vs", numInput("1"), "");
    CondResult r = runCondProject("control_while", "operator_gt", "0", init,
                                  body);
    CHECK(r.ok);
    double i = 2000000, sv = 0;
    while (i > 0) { i -= 1; sv += 1; }
    CHECK_EQ(r.list.size(), 2u);
    if (r.list.size() == 2) {
        CHECK_EQ(r.list[0], i);
        CHECK_EQ(r.list[1], sv);
    }
#ifndef XYO_JIT_DISABLED
    if (jitEnabled()) CHECK(r.runs >= 1);
#endif
}

void testRunUntilMonotoneMismatch() {
    // `repeat until i > 0` で i が減る → 単調性が逆なので検出しない。
    // 0 反復（i は変化しない）で逐次と一致する。
    const std::string init =
        initSet("init0", "i", "vi", "5", "init1") +
        initSet("init1", "s", "vs", "0", "last") +
        initSet("last", "x", "vx", "0", "loop");
    const std::string body =
        changeStmt("body0", "i", "vi", numInput("-1"), "body1") +
        changeStmt("body1", "s", "vs", numInput("1"), "");
    // i > 0 は最初から真なので repeat until は 0 反復。
    CondResult r = runCondProject("control_repeat_until", "operator_gt", "0",
                                  init, body);
    CHECK(r.ok);
    CHECK_EQ(r.list.size(), 2u);
    if (r.list.size() == 2) {
        CHECK_EQ(r.list[0], 5.0);
        CHECK_EQ(r.list[1], 0.0);
    }
}

void testRunNegativeStep() {
    // repeat n { change x by -3 } — 負の係数（以前は拒否していた）
    std::string init = initSet("init0", "x", "vx", "1000", "last") +
                       initSet("last", "a", "va", "0", "loop");
    std::string body = changeStmt("body0", "x", "vx", numInput("-3"), "");
    RunResult r = runProject(init, body, "200");
    CHECK(r.ok);
    CHECK_EQ(r.x, 1000.0 - 600.0);
#ifndef XYO_JIT_DISABLED
    if (jitEnabled()) CHECK(r.runs >= 1);
#endif
}

void testRunRoundIdentity() {
    // repeat n { set x to (round x) + 2 } — 整数値なら round は恒等
    std::string init = initSet("init0", "x", "vx", "0", "last") +
                       initSet("last", "a", "va", "0", "loop");
    std::string body =
        setStmt("body0", "x", "vx", refInput("add1"), "") +
        binopBlock("add1", "operator_add", refInput("mathop"), numInput("2")) +
        std::string(R"JSON("mathop": {"opcode": "operator_mathop",
            "next": null, "parent": "loop",
            "inputs": {"NUM": [3, "rd2", [4, "0"]]},
            "fields": {"OPERATOR": ["round", null]},
            "topLevel": false, "shadow": false},
          "rd2": {"opcode": "data_variable", "next": null, "parent": "mathop",
                  "inputs": {}, "fields": {"VARIABLE": ["x", "vx"]},
                  "topLevel": false, "shadow": false},)JSON");
    RunResult r = runProject(init, body, "1000");
    CHECK(r.ok);
    CHECK_EQ(r.x, 2000.0);
#ifndef XYO_JIT_DISABLED
    if (jitEnabled()) CHECK(r.runs >= 1);
#endif
}

void testRunDynamicInner() {
    // repeat n { repeat k { change x by 1 } } with k = 3 → x = 3n
    std::string init = initSet("init0", "x", "vx", "0", "init1") +
                       initSet("init1", "k", "vk", "3", "last") +
                       initSet("last", "a", "va", "0", "loop");
    std::string body = std::string(R"JSON("body0": {
        "opcode": "control_repeat", "next": null, "parent": "loop",
        "inputs": {"TIMES": [3, "k_r", [4, "0"]],
                   "SUBSTACK": [2, "inner0"]},
        "fields": {}, "topLevel": false, "shadow": false},
      "k_r": {"opcode": "data_variable", "next": null, "parent": "body0",
              "inputs": {}, "fields": {"VARIABLE": ["k", "vk"]},
              "topLevel": false, "shadow": false},)JSON") +
        changeStmt("inner0", "x", "vx", numInput("1"), "");
    RunResult r = runProject(init, body, "1000");
    CHECK(r.ok);
    CHECK_EQ(r.x, 3000.0);
#ifndef XYO_JIT_DISABLED
    if (jitEnabled()) CHECK(r.runs >= 1);
#endif
}

}  // namespace

int test_matpow_main() {
    // 行列累乗の適用を観測するため、この節は Eager JIT で実行する
    // （HotSpot だと 10000 ステップ内では昇格せず、インタプリタのまま走る）。
    const JitMode savedMode = jitMode();
    xyo::setJitMode(JitMode::Eager);
    testDetect();
    testRunFib();
    testRunIncr();
    testRunAffine();
    testRunBoundReject();
    testRunNonIntegerFallback();
    testRunZeroTrips();
    testRunNonWarpLoop();
    testRunNested();
    testRunUntilCountdown();
    testRunWhileCountdown();
    testRunUntilMonotoneMismatch();
    testRunNegativeStep();
    testRunRoundIdentity();
    testRunDynamicInner();
    xyo::setJitMode(savedMode);
    std::printf("matpow: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}
