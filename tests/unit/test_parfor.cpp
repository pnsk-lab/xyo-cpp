// xyo-cpp: par-for（並列 append ループ）のテスト。
//   - 検出器: 受理する形 / 拒否する形（カウンタ 2 本・非整数ステップ・
//     append 先の読み取り・副作用 reporter・制御構文・同一リスト重複 append）
//   - 実行: warp 手続き内の「カウンタ + append」ループが並列実行され、
//     リスト内容・カウンタ最終値が逐次実行と完全一致する
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/cast.hpp"
#include "engine/runtime.hpp"
#include "jit/jit.hpp"
#include "jit/parfor.hpp"
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

extern "C" int64_t xyo_parfor_runs;
extern "C" int64_t xyo_parfor_iters;

namespace {

// --- 手組みブロック木で検出器を試す ---------------------------------------

struct Fixture {
    Runtime rt;
    Sprite sprite{&rt};
    Target target{&rt, &sprite};

    Fixture() {
        // rt.targets には登録しない（Runtime 破棄時に非所有 Target を触らない）
        addVar("vi", "i");
        addVar("vj", "j");
        addList("vL", "L");
        addList("vM", "M");
        // カウンタ初期化（ループ外）
        add(make("set", "data_setvariableto"));
        sprite.blocks->getBlock("set")->fields["VARIABLE"] =
            BlockField{Value::string(SStr::create(u"i")), "vi"};
        sprite.blocks->getBlock("set")->inputs.set("VALUE", BlockInput{"lit0", ""});
        add(make("lit0", "math_number", "", true));
        sprite.blocks->getBlock("lit0")->fields["NUM"] =
            BlockField{Value::num(0), ""};
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
                                                      Variable::kScalarType, false));
    }
    void addList(const char* id, const char* name) {
        target.variables.emplace(id, Variable::create(id, utf8ToU16(name),
                                                      Variable::kListType, false));
    }
    Block* blk(const char* id) { return sprite.blocks->getBlock(id); }

    /// repeat(3) { change i by 1; add 7 to L }
    Block* buildLoop() {
        Block loop = make("loop", "control_repeat");
        loop.inputs.set("TIMES", BlockInput{"n3", ""});
        loop.inputs.set("SUBSTACK", BlockInput{"chg", ""});
        add(std::move(loop));
        add(make("n3", "math_number", "", true));
        blk("n3")->fields["NUM"] = BlockField{Value::num(3), ""};
        add(make("chg", "data_changevariableby", "add"));
        blk("chg")->fields["VARIABLE"] =
            BlockField{Value::string(SStr::create(u"i")), "vi"};
        blk("chg")->inputs.set("VALUE", BlockInput{"k1", ""});
        add(make("k1", "math_number", "", true));
        blk("k1")->fields["NUM"] = BlockField{Value::num(1), ""};
        add(make("add", "data_addtolist"));
        blk("add")->fields["LIST"] =
            BlockField{Value::string(SStr::create(u"L")), "vL"};
        blk("add")->inputs.set("ITEM", BlockInput{"item", ""});
        add(make("item", "data_variable"));
        blk("item")->fields["VARIABLE"] =
            BlockField{Value::string(SStr::create(u"i")), "vi"};
        return blk("loop");
    }
};

void testDetect() {
    {
        Fixture f;
        Block* loop = f.buildLoop();
        std::string why;
        auto plan = jit::analyzeParFor(*f.sprite.blocks, loop, &why);
        CHECK(plan.has_value());
        if (plan) {
            CHECK_EQ(plan->delta, 1.0);
            CHECK_EQ(plan->lists.size(), 1u);
        }
    }
    // カウンタ 2 本 → 拒否
    {
        Fixture f;
        Block* loop = f.buildLoop();
        f.add(Fixture::make("chg2", "data_changevariableby", "add"));
        f.blk("chg2")->fields["VARIABLE"] =
            BlockField{Value::string(SStr::create(u"j")), "vj"};
        f.blk("chg2")->inputs.set("VALUE", BlockInput{"k2", ""});
        f.add(Fixture::make("k2", "math_number", "", true));
        f.blk("k2")->fields["NUM"] = BlockField{Value::num(2), ""};
        f.blk("chg")->next = "chg2";
        std::string why;
        CHECK(!jit::analyzeParFor(*f.sprite.blocks, loop, &why).has_value());
        CHECK(why.find("two induction counters") != std::string::npos);
    }
    // 非整数ステップ → 拒否
    {
        Fixture f;
        Block* loop = f.buildLoop();
        f.blk("k1")->fields["NUM"] = BlockField{Value::num(0.5), ""};
        CHECK(!jit::analyzeParFor(*f.sprite.blocks, loop, nullptr).has_value());
    }
    // append 先リストを本体で読む → 拒否
    {
        Fixture f;
        Block* loop = f.buildLoop();
        f.add(Fixture::make("rd", "data_itemoflist"));
        f.blk("rd")->fields["LIST"] =
            BlockField{Value::string(SStr::create(u"L")), "vL"};
        f.blk("add")->inputs.set("ITEM", BlockInput{"rd", ""});
        std::string why;
        CHECK(!jit::analyzeParFor(*f.sprite.blocks, loop, &why).has_value());
        CHECK(why.find("appended list") != std::string::npos);
    }
    // 副作用 reporter（sensing_askandwait 等）→ 拒否
    {
        Fixture f;
        Block* loop = f.buildLoop();
        f.add(Fixture::make("sns", "sensing_mousedown"));
        f.blk("add")->inputs.set("ITEM", BlockInput{"sns", ""});
        CHECK(!jit::analyzeParFor(*f.sprite.blocks, loop, nullptr).has_value());
    }
    // 同一リストへ 2 回 append → 拒否
    {
        Fixture f;
        Block* loop = f.buildLoop();
        f.add(Fixture::make("add2", "data_addtolist"));
        f.blk("add2")->fields["LIST"] =
            BlockField{Value::string(SStr::create(u"L")), "vL"};
        f.blk("add2")->inputs.set("ITEM", BlockInput{"item", ""});
        f.blk("add")->next = "add2";
        std::string why;
        CHECK(!jit::analyzeParFor(*f.sprite.blocks, loop, &why).has_value());
        CHECK(why.find("twice") != std::string::npos);
    }
    // 制御構文（if）→ 拒否
    {
        Fixture f;
        Block* loop = f.buildLoop();
        f.add(Fixture::make("iff", "control_if"));
        f.blk("add")->next = "iff";
        CHECK(!jit::analyzeParFor(*f.sprite.blocks, loop, nullptr).has_value());
    }
    // control_repeat 以外 → 拒否
    {
        Fixture f;
        Block* loop = f.buildLoop();
        f.blk("loop")->opcode = "control_repeat_until";
        CHECK(!jit::analyzeParFor(*f.sprite.blocks, loop, nullptr).has_value());
    }
}

// --- 実行: warp 手続きの「カウンタ + append」ループ ----------------------

const char* kProject = R"JSON({
  "targets": [
    {"isStage": true, "name": "Stage", "variables": {}, "lists": {},
     "broadcasts": {}, "blocks": {}, "comments": {}, "costumes": [],
     "sounds": [], "currentCostume": 0, "volume": 100, "layerOrder": 0},
    {"isStage": false, "name": "S", "layerOrder": 1,
     "variables": {"vi": ["i", 0]},
     "lists": {"vL": ["L", []]},
     "broadcasts": {},
     "blocks": {
       "hat": {"opcode": "event_whenflagclicked", "next": "call",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 0, "y": 0},
       "call": {"opcode": "procedures_call", "next": null, "parent": "hat",
                "inputs": {"argN": [1, "argn"]}, "fields": {},
                "topLevel": false, "shadow": false,
                "mutation": {"tagName": "mutation", "children": [],
                             "proccode": "fill %s", "argumentids": "[\"argN\"]",
                             "argumentnames": "[\"n\"]",
                             "argumentdefaults": "[\"0\"]", "warp": "true"}},
       "argn": {"opcode": "math_number", "next": null, "parent": "call",
                "inputs": {}, "fields": {"NUM": [4000, null]},
                "topLevel": false, "shadow": true},
       "def": {"opcode": "procedures_definition", "next": "seti",
               "parent": null, "inputs": {"custom_block": [1, "proto"]},
               "fields": {}, "topLevel": true, "shadow": false, "x": 0, "y": 400},
       "proto": {"opcode": "procedures_prototype", "next": null, "parent": "def",
                 "inputs": {"argN": [1, "argn2"]}, "fields": {},
                 "topLevel": false, "shadow": true,
                 "mutation": {"tagName": "mutation", "children": [],
                              "proccode": "fill %s",
                              "argumentids": "[\"argN\"]",
                              "argumentnames": "[\"n\"]",
                              "argumentdefaults": "[\"0\"]", "warp": "true"}},
       "argn2": {"opcode": "argument_reporter_string_number", "next": null,
                 "parent": "proto", "inputs": {},
                 "fields": {"VALUE": ["n", null]},
                 "topLevel": false, "shadow": true},
       "seti": {"opcode": "data_setvariableto", "next": "loop",
                "parent": "def", "inputs": {"VALUE": [1, [10, "0"]]},
                "fields": {"VARIABLE": ["i", "vi"]},
                "topLevel": false, "shadow": false},
       "loop": {"opcode": "control_repeat", "next": null, "parent": "seti",
                "inputs": {"TIMES": [3, "argr", [6, "0"]],
                           "SUBSTACK": [2, "chg"]},
                "fields": {}, "topLevel": false, "shadow": false},
       "argr": {"opcode": "argument_reporter_string_number", "next": null,
                "parent": "loop", "inputs": {}, "fields": {"VALUE": ["n", null]},
                "topLevel": false, "shadow": false},
       "chg": {"opcode": "data_changevariableby", "next": "add",
               "parent": "loop", "inputs": {"VALUE": [1, [4, "1"]]},
               "fields": {"VARIABLE": ["i", "vi"]},
               "topLevel": false, "shadow": false},
       "add": {"opcode": "data_addtolist", "next": null, "parent": "chg",
               "inputs": {"ITEM": [3, "sq", [10, "0"]]},
               "fields": {"LIST": ["L", "vL"]},
               "topLevel": false, "shadow": false},
       "sq": {"opcode": "operator_multiply", "next": null, "parent": "add",
              "inputs": {"NUM1": [3, "rdi", [4, "0"]],
                         "NUM2": [3, "rdi2", [4, "1"]]},
              "fields": {}, "topLevel": false, "shadow": false},
       "rdi": {"opcode": "data_variable", "next": null, "parent": "sq",
               "inputs": {}, "fields": {"VARIABLE": ["i", "vi"]},
               "topLevel": false, "shadow": false},
       "rdi2": {"opcode": "data_variable", "next": null, "parent": "sq",
                "inputs": {}, "fields": {"VARIABLE": ["i", "vi"]},
                "topLevel": false, "shadow": false}
     },
     "comments": {}, "costumes": [], "sounds": [], "currentCostume": 0,
     "volume": 100, "visible": true, "x": 0, "y": 0, "size": 100,
     "direction": 90, "draggable": false, "rotationStyle": "all around"}
  ],
  "monitors": [], "extensions": [], "meta": {"semver": "3.0.0"}
})JSON";

std::vector<uint8_t> zipWithProject(const char* json) {
    ZipWriter zw;
    const std::string s = json;
    zw.addFile("project.json", std::vector<uint8_t>(s.begin(), s.end()));
    return zw.finish();
}

void testRun() {
    const int64_t n = 4000;
    const int64_t before = xyo_parfor_runs;
    const std::vector<uint8_t> sb3 = zipWithProject(kProject);
    Runtime rt;
    Sb3LoadResult lr = loadSb3File(rt, sb3.data(), sb3.size());
    CHECK(lr.ok);
    if (!lr.ok) return;
    rt.greenFlag();
    rt.runUntilDone(10000);

    Target* sp = rt.getSpriteTargetByName(u"S");
    CHECK(sp != nullptr);
    if (!sp) return;
    // カウンタ最終値
    Variable* i = sp->lookupVariableById("vi");
    CHECK(i != nullptr);
    if (i) CHECK_EQ(cast::toNumber(i->value), double(n));
    // リスト内容: i*i (i = 1..n)
    Variable* L = sp->lookupVariableById("vL");
    CHECK(L != nullptr && L->list != nullptr);
    if (L && L->list) {
        CHECK_EQ(L->list->len, uint32_t(n));
        bool allOk = L->list->len == uint32_t(n);
        for (uint32_t k = 0; k < L->list->len && allOk; ++k) {
            const double i2 = double(k + 1);
            const double want = i2 * i2;
            const double got = cast::toNumber(L->list->items[k]);
            if (got != want) allOk = false;
        }
        CHECK(allOk);
    }
#ifndef XYO_JIT_DISABLED
    if (jitEnabled())
        CHECK(xyo_parfor_runs > before);   // 並列パスが実際に走った
#endif
}

}  // namespace

int test_parfor_main() {
    testDetect();
    testRun();
    std::printf("parfor: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}
