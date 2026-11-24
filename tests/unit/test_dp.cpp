// xyo-cpp: DP 変換（純粋な自己再帰 reporter 手続きの自動メモ化）のテスト。
//   - 受理: 純粋・warp・1 引数・自己再帰・return あり → メモ化リストが作られる
//   - 拒否: 変数を書く / 非 warp / 再帰しない → 変換しない
//   - 実行: メモ化の有無で結果が一致し、メモ用リストの長さが引数の種類数になる
//   - MLIR（既定の Mega 経路）で fib（2 か所再帰）が正しく動く
//     = 変換後もコンパイルでき、if の then で return した後続文が消えない
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#include "core/cast.hpp"
#include "core/value.hpp"
#include "engine/runtime.hpp"
#include "jit/jit.hpp"
#include "load/dp_transform.hpp"
#include "load/sb3.hpp"
#include "load/zip.hpp"
#include "model/blocks.hpp"
#include "model/target.hpp"
#if defined(XYO_JIT) && !defined(__EMSCRIPTEN__)
#include "mlir/frontend.hpp"
#endif

using namespace xyo;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; \
    if (!((a) == (b))) { ++g_fail; \
    std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, \
                #a, #b, (long long)(a), (long long)(b)); } } while (0)

namespace {

std::vector<uint8_t> zipWithProject(const std::string& json) {
    ZipWriter zw;
    zw.addFile("project.json",
               std::vector<uint8_t>(json.begin(), json.end()));
    return zw.finish();
}

// 手続きの本体。@BODY@ を差し替えて受理/拒否のケースを作る。
//   hat: r = f(15); add r to L
const char* kProject = R"JSON({"targets":[
{"isStage":true,"name":"Stage","variables":{},"lists":{},"broadcasts":{},
 "blocks":{},"comments":{},"costumes":[],"sounds":[],"currentCostume":0,
 "volume":100,"layerOrder":0},
{"isStage":false,"name":"S","layerOrder":1,
 "variables":{"vr":["r",0],"vz":["z",0]},
 "lists":{"vL":["L",[]]},"broadcasts":{},
 "blocks":{
 "hat":{"opcode":"event_whenflagclicked","next":"setR","parent":null,
        "inputs":{},"fields":{},"topLevel":true,"shadow":false},
 "setR":{"opcode":"data_setvariableto","next":"addL","parent":"hat",
         "inputs":{"VALUE":[3,"call0",[4,"0"]]},
         "fields":{"VARIABLE":["r","vr"]},"topLevel":false,"shadow":false},
 "call0":{"opcode":"procedures_call","next":null,"parent":"setR",
          "inputs":{"argN":[1,"argn"]},"fields":{},"topLevel":false,
          "shadow":false,
          "mutation":{"tagName":"mutation","children":[],"proccode":"f %s",
                      "argumentids":"[\"argN\"]","argumentnames":"[\"n\"]",
                      "argumentdefaults":"[\"0\"]","warp":"@WARP@",
                      "return":"true"}},
 "argn":{"opcode":"math_number","next":null,"parent":"call0","inputs":{},
         "fields":{"NUM":[15,null]},"topLevel":false,"shadow":true},
 "addL":{"opcode":"data_addtolist","next":null,"parent":"setR",
         "inputs":{"ITEM":[3,"rr",[4,"0"]]},"fields":{"LIST":["L","vL"]},
         "topLevel":false,"shadow":false},
 "rr":{"opcode":"data_variable","next":null,"parent":"addL","inputs":{},
       "fields":{"VARIABLE":["r","vr"]},"topLevel":false,"shadow":false},
 "def":{"opcode":"procedures_definition","next":"@HEAD@","parent":null,
        "inputs":{"custom_block":[1,"proto"]},"fields":{},
        "topLevel":true,"shadow":false},
 "proto":{"opcode":"procedures_prototype","next":null,"parent":"def",
          "inputs":{"argN":[1,"argn2"]},"fields":{},"topLevel":false,
          "shadow":true,
          "mutation":{"tagName":"mutation","children":[],"proccode":"f %s",
                      "argumentids":"[\"argN\"]","argumentnames":"[\"n\"]",
                      "argumentdefaults":"[\"0\"]","warp":"@WARP@",
                      "return":"true"}},
 "argn2":{"opcode":"argument_reporter_string_number","next":null,
          "parent":"proto","inputs":{},"fields":{"VALUE":["n",null]},
          "topLevel":false,"shadow":true},
@BODY@
 },
 "comments":{},"costumes":[],"sounds":[],"currentCostume":0,"volume":100,
 "visible":true,"x":0,"y":0,"size":100,"direction":90,"draggable":false,
 "rotationStyle":"all around"}],
 "monitors":[],"extensions":[],"meta":{"semver":"3.0.0"}})JSON";

// f(n) = n<1 ? 1 : n + f(n-1)   （単一再帰。全実行経路で正しい）
const char* kSumBody = R"JSON(
 "ifb":{"opcode":"control_if_else","next":null,"parent":"def",
        "inputs":{"CONDITION":[2,"lt"],"SUBSTACK":[2,"ret1"],
                  "SUBSTACK2":[2,"ret2"]},
        "fields":{},"topLevel":false,"shadow":false},
 "lt":{"opcode":"operator_lt","next":null,"parent":"ifb",
       "inputs":{"OPERAND1":[3,"ar1",[4,"0"]],"OPERAND2":[1,[4,"1"]]},
       "fields":{},"topLevel":false,"shadow":false},
 "ar1":{"opcode":"argument_reporter_string_number","next":null,
        "parent":"lt","inputs":{},"fields":{"VALUE":["n",null]},
        "topLevel":false,"shadow":false},
 "ret1":{"opcode":"procedures_return","next":null,"parent":"ifb",
         "inputs":{"VALUE":[1,[4,"1"]]},"fields":{},
         "topLevel":false,"shadow":false},
 "ret2":{"opcode":"procedures_return","next":null,"parent":"ifb",
         "inputs":{"VALUE":[3,"sum",[4,"0"]]},"fields":{},
         "topLevel":false,"shadow":false},
 "sum":{"opcode":"operator_add","next":null,"parent":"ret2",
        "inputs":{"NUM1":[3,"ar2",[4,"0"]],"NUM2":[3,"c1",[4,"0"]]},
        "fields":{},"topLevel":false,"shadow":false},
 "ar2":{"opcode":"argument_reporter_string_number","next":null,
        "parent":"sum","inputs":{},"fields":{"VALUE":["n",null]},
        "topLevel":false,"shadow":false},
 "c1":{"opcode":"procedures_call","next":null,"parent":"sum",
       "inputs":{"argN":[3,"nm1",[4,"0"]]},"fields":{},"topLevel":false,
       "shadow":false,
       "mutation":{"tagName":"mutation","children":[],"proccode":"f %s",
                   "argumentids":"[\"argN\"]","argumentnames":"[\"n\"]",
                   "argumentdefaults":"[\"0\"]","warp":"true",
                   "return":"true"}},
 "nm1":{"opcode":"operator_subtract","next":null,"parent":"c1",
        "inputs":{"NUM1":[3,"ar3",[4,"0"]],"NUM2":[1,[4,"1"]]},
        "fields":{},"topLevel":false,"shadow":false},
 "ar3":{"opcode":"argument_reporter_string_number","next":null,
        "parent":"nm1","inputs":{},"fields":{"VALUE":["n",null]},
        "topLevel":false,"shadow":false})JSON";

// fib: f(n) = n<2 ? n : f(n-1)+f(n-2)  （2 か所再帰）
const char* kFibBody = R"JSON(
 "ifb":{"opcode":"control_if_else","next":null,"parent":"def",
        "inputs":{"CONDITION":[2,"lt"],"SUBSTACK":[2,"ret1"],
                  "SUBSTACK2":[2,"ret2"]},
        "fields":{},"topLevel":false,"shadow":false},
 "lt":{"opcode":"operator_lt","next":null,"parent":"ifb",
       "inputs":{"OPERAND1":[3,"ar1",[4,"0"]],"OPERAND2":[1,[4,"2"]]},
       "fields":{},"topLevel":false,"shadow":false},
 "ar1":{"opcode":"argument_reporter_string_number","next":null,
        "parent":"lt","inputs":{},"fields":{"VALUE":["n",null]},
        "topLevel":false,"shadow":false},
 "ret1":{"opcode":"procedures_return","next":null,"parent":"ifb",
         "inputs":{"VALUE":[3,"ar2",[4,"0"]]},"fields":{},
         "topLevel":false,"shadow":false},
 "ar2":{"opcode":"argument_reporter_string_number","next":null,
        "parent":"ret1","inputs":{},"fields":{"VALUE":["n",null]},
        "topLevel":false,"shadow":false},
 "ret2":{"opcode":"procedures_return","next":null,"parent":"ifb",
         "inputs":{"VALUE":[3,"sum",[4,"0"]]},"fields":{},
         "topLevel":false,"shadow":false},
 "sum":{"opcode":"operator_add","next":null,"parent":"ret2",
        "inputs":{"NUM1":[3,"c1",[4,"0"]],"NUM2":[3,"c2",[4,"0"]]},
        "fields":{},"topLevel":false,"shadow":false},
 "c1":{"opcode":"procedures_call","next":null,"parent":"sum",
       "inputs":{"argN":[3,"nm1",[4,"0"]]},"fields":{},"topLevel":false,
       "shadow":false,
       "mutation":{"tagName":"mutation","children":[],"proccode":"f %s",
                   "argumentids":"[\"argN\"]","argumentnames":"[\"n\"]",
                   "argumentdefaults":"[\"0\"]","warp":"true",
                   "return":"true"}},
 "nm1":{"opcode":"operator_subtract","next":null,"parent":"c1",
        "inputs":{"NUM1":[3,"ar3",[4,"0"]],"NUM2":[1,[4,"1"]]},
        "fields":{},"topLevel":false,"shadow":false},
 "ar3":{"opcode":"argument_reporter_string_number","next":null,
        "parent":"nm1","inputs":{},"fields":{"VALUE":["n",null]},
        "topLevel":false,"shadow":false},
 "c2":{"opcode":"procedures_call","next":null,"parent":"sum",
       "inputs":{"argN":[3,"nm2",[4,"0"]]},"fields":{},"topLevel":false,
       "shadow":false,
       "mutation":{"tagName":"mutation","children":[],"proccode":"f %s",
                   "argumentids":"[\"argN\"]","argumentnames":"[\"n\"]",
                   "argumentdefaults":"[\"0\"]","warp":"true",
                   "return":"true"}},
 "nm2":{"opcode":"operator_subtract","next":null,"parent":"c2",
        "inputs":{"NUM1":[3,"ar4",[4,"0"]],"NUM2":[1,[4,"2"]]},
        "fields":{},"topLevel":false,"shadow":false},
 "ar4":{"opcode":"argument_reporter_string_number","next":null,
        "parent":"nm2","inputs":{},"fields":{"VALUE":["n",null]},
        "topLevel":false,"shadow":false})JSON";

// fib の本体先頭に「変数 z を書く」文を足す（純粋性の拒否テスト）。
const char* kImpureBody = R"JSON(
 "extra":{"opcode":"data_setvariableto","next":"ifb","parent":"def",
          "inputs":{"VALUE":[1,[4,"1"]]},
          "fields":{"VARIABLE":["z","vz"]},"topLevel":false,"shadow":false},
 "ifb":{"opcode":"control_if_else","next":null,"parent":"def",
        "inputs":{"CONDITION":[2,"lt"],"SUBSTACK":[2,"ret1"],
                  "SUBSTACK2":[2,"ret2"]},
        "fields":{},"topLevel":false,"shadow":false},
 "lt":{"opcode":"operator_lt","next":null,"parent":"ifb",
       "inputs":{"OPERAND1":[3,"ar1",[4,"0"]],"OPERAND2":[1,[4,"2"]]},
       "fields":{},"topLevel":false,"shadow":false},
 "ar1":{"opcode":"argument_reporter_string_number","next":null,
        "parent":"lt","inputs":{},"fields":{"VALUE":["n",null]},
        "topLevel":false,"shadow":false},
 "ret1":{"opcode":"procedures_return","next":null,"parent":"ifb",
         "inputs":{"VALUE":[3,"ar2",[4,"0"]]},"fields":{},
         "topLevel":false,"shadow":false},
 "ar2":{"opcode":"argument_reporter_string_number","next":null,
        "parent":"ret1","inputs":{},"fields":{"VALUE":["n",null]},
        "topLevel":false,"shadow":false},
 "ret2":{"opcode":"procedures_return","next":null,"parent":"ifb",
         "inputs":{"VALUE":[1,[4,"0"]]},"fields":{},
         "topLevel":false,"shadow":false})JSON";

// 再帰しない手続き: return n*2
const char* kNonRecBody = R"JSON(
 "ret":{"opcode":"procedures_return","next":null,"parent":"def",
        "inputs":{"VALUE":[3,"mul",[4,"0"]]},"fields":{},
        "topLevel":false,"shadow":false},
 "mul":{"opcode":"operator_multiply","next":null,"parent":"ret",
        "inputs":{"NUM1":[3,"ar9",[4,"0"]],"NUM2":[1,[4,"2"]]},
        "fields":{},"topLevel":false,"shadow":false},
 "ar9":{"opcode":"argument_reporter_string_number","next":null,
        "parent":"mul","inputs":{},"fields":{"VALUE":["n",null]},
        "topLevel":false,"shadow":false})JSON";

std::string replaceAll(std::string s, const std::string& from,
                       const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string buildProject(const char* body, bool warp) {
    std::string json = kProject;
    // 本体の先頭ブロック id（体内の最初のキー）を def.next に置く。
    const std::string b = body;
    const size_t q = b.find('"');
    const size_t q2 = b.find('"', q + 1);
    const std::string head = b.substr(q + 1, q2 - q - 1);
    json = replaceAll(json, "@BODY@", b);
    json = replaceAll(json, "@HEAD@", head);
    json = replaceAll(json, "@WARP@", warp ? "true" : "false");
    return json;
}

struct RunOut {
    bool ok = false;
    double r = 0;
    size_t memoKeys = 0;
    bool hasMemoLists = false;
};

RunOut runProject(const std::string& json, bool useMlir = false) {
    RunOut out;
#if defined(XYO_JIT) && !defined(__EMSCRIPTEN__)
    const JitMode savedMode = jitMode();
    mlir::setForceEnabled(useMlir ? 1 : 0);
    setJitMode(JitMode::Eager);
    struct Restore {
        JitMode m;
        ~Restore() {
            setJitMode(m);
            mlir::setForceEnabled(-1);
        }
    } restore{savedMode};
#endif
    const auto sb3 = zipWithProject(json);
    Runtime rt;
    Sb3LoadResult lr = loadSb3File(rt, sb3.data(), sb3.size());
    CHECK(lr.ok);
    if (!lr.ok) return out;
    Target* sp = rt.getSpriteTargetByName(u"S");
    CHECK(sp != nullptr);
    if (!sp) return out;
    for (const auto& [id, var] : sp->variables) {
        (void)id;
        if (var.name.find(u"__xyo_dp") == std::u16string::npos) continue;
        out.hasMemoLists = true;
        if (var.type == Variable::kListType && var.list &&
            var.name.find(u"__xyo_dp_keys") != std::u16string::npos)
            out.memoKeys = var.list->len;
    }
    rt.greenFlag();
    rt.runUntilDone(4000000);
    Variable* rv = sp->lookupVariableById("vr");
    out.r = rv ? cast::toNumber(rv->value) : -1.0;
    for (const auto& [id, var] : sp->variables) {
        (void)id;
        if (var.type == Variable::kListType && var.list &&
            var.name.find(u"__xyo_dp_keys") != std::u16string::npos)
            out.memoKeys = var.list->len;
    }
    out.ok = true;
    return out;
}

// --- 検出の受理/拒否 -------------------------------------------------------

void testDetectAccept() {
    RunOut o = runProject(buildProject(kSumBody, true));
    CHECK(o.ok);
    CHECK(o.hasMemoLists);              // メモ化リストが作られる
    CHECK_EQ(o.memoKeys, size_t(16));   // f(15) は 0..15 の 16 通り
    CHECK_EQ(int64_t(o.r), int64_t(15 * 16 / 2 + 1));   // 121
}

void testDetectRejectImpure() {
    RunOut o = runProject(buildProject(kImpureBody, true));
    CHECK(o.ok);
    CHECK(!o.hasMemoLists);             // 変数を書くのでメモ化しない
}

void testDetectRejectNonWarp() {
    RunOut o = runProject(buildProject(kSumBody, false));
    CHECK(o.ok);
    CHECK(!o.hasMemoLists);             // 非 warp は対象外
}

void testDetectRejectNonRecursive() {
    RunOut o = runProject(buildProject(kNonRecBody, true));
    CHECK(o.ok);
    CHECK(!o.hasMemoLists);
    CHECK_EQ(int64_t(o.r), int64_t(30));   // 15*2
}

// --- 実行: メモ化の有無で結果が一致 ---------------------------------------

void testRunMatchesNoMemo() {
    const std::string json = buildProject(kSumBody, true);
    RunOut on = runProject(json);
    CHECK(on.ok);
    setenv("XYO_DP", "0", 1);
    RunOut off = runProject(json);
    unsetenv("XYO_DP");
    CHECK(off.ok);
    CHECK_EQ(int64_t(off.r), int64_t(on.r));
    CHECK(!off.hasMemoLists);
    CHECK_EQ(int64_t(on.r), int64_t(15 * 16 / 2 + 1));
}

// --- MLIR 経路: fib（2 か所再帰）が正しく動く -------------------------------
// 変換後もコンパイルできないと interpreter に落ち、interpreter は 2 か所
// 再帰の reporter 手続きを誤るため、この値一致がコンパイル成功の証明になる。

void testFibOnMlir() {
    RunOut o = runProject(buildProject(kFibBody, true), /*useMlir=*/true);
    CHECK(o.ok);
    CHECK(o.hasMemoLists);
    CHECK_EQ(o.memoKeys, size_t(16));
    CHECK_EQ(int64_t(o.r), int64_t(610));   // fib(15)
}

}  // namespace

int test_dp_main() {
    g_checks = 0;
    g_fail = 0;
    testDetectAccept();
    testDetectRejectImpure();
    testDetectRejectNonWarp();
    testDetectRejectNonRecursive();
    testRunMatchesNoMemo();
    testFibOnMlir();
    std::printf("dp: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}
