// xyo-cpp: MLIR 風フロントエンド（xyo dialect / pass / LLVM lowering）のテスト。
//
//  1. IR の構築・verifier・canonicalize/CSE/DCE・数値 SSA 昇格
//  2. verifier が「定義前使用」を拒否すること
//  3. 実 sb3 を interpreter と MLIR 経路で走らせ、最終状態が一致すること
//  4. サブセット外のスクリプトが既存 codegen へフォールバックすること
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/cast.hpp"
#include "engine/runtime.hpp"
#include "jit/jit.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"
#include "model/variable.hpp"

#if defined(XYO_JIT) && !defined(__EMSCRIPTEN__)
#include "mlir/frontend.hpp"
#include "mlir/ir.hpp"
#include "mlir/lower_llvm.hpp"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#endif

namespace {

int checks = 0;
int failures = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL mlir: %s\n", what);
    }
}

using namespace xyo;

#if defined(XYO_JIT) && !defined(__EMSCRIPTEN__)

// ---------------------------------------------------------------- IR/pass ----

mlir::Value makeConst(mlir::Module& m, mlir::Builder& b, double d) {
    return b.cst(Value::numCanonical(d).bits);
}

void testIrAndPasses() {
    mlir::Module m;
    auto* fn = m.createFunction("test_fn", {mlir::Type::ctx()});
    mlir::Builder b(m);
    b.setBlock(fn->body);
    b.setVar("vx", "x", makeConst(m, b, 0));
    mlir::Value count = makeConst(m, b, 10);
    mlir::Op* loop = b.create("xyo.repeat", {count}, {});
    mlir::Region* region = loop->addRegion();
    mlir::Block* body = region->addBlock();
    {
        mlir::Builder nb(m);
        nb.setBlock(body);
        mlir::Value cur = nb.getVar("vx", "x");
        mlir::Value one = makeConst(m, nb, 1);
        nb.setVar("vx", "x", nb.add(cur, one));
        nb.createTerminator("xyo.loop_yield", {});
        nb.createTerminator("xyo.yield", {});
    }
    b.createTerminator("func.return", {});

    check(mlir::verify(m), "hand-built module verifies");
    const std::string before = m.print();
    check(before.find("xyo.repeat") != std::string::npos, "prints xyo.repeat");
    check(before.find("xyo.add") != std::string::npos, "prints xyo.add");

    mlir::PassStats stats;
    mlir::runStandardPipeline(m, &stats);
    check(stats.promotedLoops == 1, "promoted exactly one loop");
    check(stats.promotedVars == 1, "promoted exactly one variable");
    check(mlir::verify(m), "module verifies after passes");

    bool hasTypedAdd = false;
    bool hasTypedLoopArg = false;
    m.forEachOp([&](mlir::Op& op) {
        if (op.name == "xyo.add.f") hasTypedAdd = true;
        for (auto& r : op.regions)
            for (auto& blk : r->blocks)
                for (auto& arg : blk->args)
                    if (arg.type == mlir::Type::f64()) hasTypedLoopArg = true;
    });
    check(hasTypedAdd, "numeric promotion introduced xyo.add.f");
    check(hasTypedLoopArg, "loop carries an f64 block argument");
    const std::string after = m.print();
    // 昇格後の spill は d-only 運搬の導入で box.f に変わった（from_num は
    // 互換のため lowering に残っている）。どちらでも「box して戻す」意図は
    // 満たすので両方を受ける。
    check(after.find("xyo.box.f") != std::string::npos ||
              after.find("xyo.from_num") != std::string::npos,
          "spills boxed value back to the variable");

    llvm::LLVMContext lc;
    llvm::Module lm("mlir_test", lc);
    const mlir::LoweredFunction lowered = mlir::lowerToLLVM(m, lm, lc, "test_fn");
    check(lowered.ok && lowered.fn != nullptr, "lowering produced the entry");
    check(lowered.mayYield, "lowered loop reports mayYield");
    if (lowered.fn) {
        check(lowered.fn->arg_size() == 1, "entry takes JitCtx*");
        check(lowered.fn->getReturnType()->isVoidTy(), "entry returns void");
    }
}

void testVerifierRejectsUseBeforeDef() {
    mlir::Module m;
    auto* fn = m.createFunction("bad_fn", {mlir::Type::ctx()});
    mlir::Builder b(m);
    b.setBlock(fn->body);
    mlir::Op* user = m.create("xyo.round", {}, {mlir::Type::value()});
    mlir::Op* def = m.create("xyo.const", {}, {mlir::Type::value()},
                             {{"bits", mlir::Attr::integer(1)}});
    user->operands.push_back(def->result(0));
    user->parent = fn->body;
    def->parent = fn->body;
    fn->body->ops.push_back(user);
    fn->body->ops.push_back(def);
    b.createTerminator("func.return", {});
    std::vector<mlir::Diagnostic> errors;
    check(!mlir::verify(m, &errors), "verifier rejects use-before-def");
    check(!errors.empty(), "verifier reports a diagnostic");
}

// ------------------------------------------------------------- end-to-end ----

const char* kLoopProject = R"JSON({
 "targets": [
  {"isStage": true, "name": "Stage", "variables": {}, "lists": {},
   "broadcasts": {}, "blocks": {}, "comments": {}, "costumes": [],
   "sounds": [], "currentCostume": 0, "volume": 100, "layerOrder": 0},
  {"isStage": false, "name": "S", "layerOrder": 1,
   "variables": {"vx": ["x", 0]}, "lists": {"vL": ["L", []]},
   "broadcasts": {},
   "blocks": {
     "hat": {"opcode": "event_whenflagclicked", "next": "set", "parent": null,
             "inputs": {}, "fields": {}, "topLevel": true, "shadow": false,
             "x": 0, "y": 0},
     "set": {"opcode": "data_setvariableto", "next": "loop", "parent": "hat",
             "inputs": {"VALUE": [1, [4, "0"]]},
             "fields": {"VARIABLE": ["x", "vx"]}, "topLevel": false,
             "shadow": false},
     "loop": {"opcode": "control_repeat", "next": "tag", "parent": "set",
              "inputs": {"TIMES": [1, [6, "500"]], "SUBSTACK": [2, "chg"]},
              "fields": {}, "topLevel": false, "shadow": false},
     "chg": {"opcode": "data_changevariableby", "next": null, "parent": "loop",
             "inputs": {"VALUE": [1, [4, "2"]]},
             "fields": {"VARIABLE": ["x", "vx"]}, "topLevel": false,
             "shadow": false},
     "tag": {"opcode": "data_addtolist", "next": null, "parent": "loop",
             "inputs": {"ITEM": [3, "rd", [10, "0"]]},
             "fields": {"LIST": ["L", "vL"]}, "topLevel": false,
             "shadow": false},
     "rd": {"opcode": "data_variable", "next": null, "parent": "tag",
            "inputs": {}, "fields": {"VARIABLE": ["x", "vx"]},
            "topLevel": false, "shadow": false}
   },
   "comments": {}, "costumes": [], "sounds": [], "currentCostume": 0,
   "volume": 100, "visible": true, "x": 0, "y": 0, "size": 100,
   "direction": 90, "draggable": false, "rotationStyle": "all around"}
 ],
 "monitors": [], "extensions": [], "meta": {"semver": "3.0.0"}
})JSON";

const char* kFallbackProject = R"JSON({
 "targets": [
  {"isStage": true, "name": "Stage", "variables": {}, "lists": {},
   "broadcasts": {}, "blocks": {}, "comments": {}, "costumes": [],
   "sounds": [], "currentCostume": 0, "volume": 100, "layerOrder": 0},
  {"isStage": false, "name": "S", "layerOrder": 1,
   "variables": {"vx": ["x", 0]}, "lists": {"vL": ["L", []]},
   "broadcasts": {},
   "blocks": {
     "hat": {"opcode": "event_whenflagclicked", "next": "set", "parent": null,
             "inputs": {}, "fields": {}, "topLevel": true, "shadow": false,
             "x": 0, "y": 0},
     "set": {"opcode": "data_setvariableto", "next": "say", "parent": "hat",
             "inputs": {"VALUE": [1, [4, "0"]]},
             "fields": {"VARIABLE": ["x", "vx"]}, "topLevel": false,
             "shadow": false},
     "say": {"opcode": "looks_say", "next": "loop", "parent": "set",
             "inputs": {"MESSAGE": [1, [10, "hi"]]}, "fields": {},
             "topLevel": false, "shadow": false},
     "loop": {"opcode": "control_repeat", "next": "tag", "parent": "say",
              "inputs": {"TIMES": [1, [6, "100"]], "SUBSTACK": [2, "chg"]},
              "fields": {}, "topLevel": false, "shadow": false},
     "chg": {"opcode": "data_changevariableby", "next": null, "parent": "loop",
             "inputs": {"VALUE": [1, [4, "1"]]},
             "fields": {"VARIABLE": ["x", "vx"]}, "topLevel": false,
             "shadow": false},
     "tag": {"opcode": "data_addtolist", "next": null, "parent": "loop",
             "inputs": {"ITEM": [3, "rd", [10, "0"]]},
             "fields": {"LIST": ["L", "vL"]}, "topLevel": false,
             "shadow": false},
     "rd": {"opcode": "data_variable", "next": null, "parent": "tag",
            "inputs": {}, "fields": {"VARIABLE": ["x", "vx"]},
            "topLevel": false, "shadow": false}
   },
   "comments": {}, "costumes": [], "sounds": [], "currentCostume": 0,
   "volume": 100, "visible": true, "x": 0, "y": 0, "size": 100,
   "direction": 90, "draggable": false, "rotationStyle": "all around"}
 ],
 "monitors": [], "extensions": [], "meta": {"semver": "3.0.0"}
})JSON";

const char* kStringProject = R"JSON({
 "targets": [
  {"isStage": true, "name": "Stage", "variables": {}, "lists": {},
   "broadcasts": {}, "blocks": {}, "comments": {}, "costumes": [],
   "sounds": [], "currentCostume": 0, "volume": 100, "layerOrder": 0},
  {"isStage": false, "name": "S", "layerOrder": 1,
   "variables": {"vs": ["s", ""]}, "lists": {"vL": ["L", []]},
   "broadcasts": {},
   "blocks": {
     "hat": {"opcode": "event_whenflagclicked", "next": "set", "parent": null,
             "inputs": {}, "fields": {}, "topLevel": true, "shadow": false,
             "x": 0, "y": 0},
     "set": {"opcode": "data_setvariableto", "next": "loop", "parent": "hat",
             "inputs": {"VALUE": [1, [10, ""]]},
             "fields": {"VARIABLE": ["s", "vs"]}, "topLevel": false,
             "shadow": false},
     "loop": {"opcode": "control_repeat", "next": "tag", "parent": "set",
              "inputs": {"TIMES": [1, [6, "2000"]], "SUBSTACK": [2, "cat"]},
              "fields": {}, "topLevel": false, "shadow": false},
     "cat": {"opcode": "data_setvariableto", "next": null, "parent": "loop",
             "inputs": {"VALUE": [3, "join", [10, "0"]]},
             "fields": {"VARIABLE": ["s", "vs"]}, "topLevel": false,
             "shadow": false},
     "join": {"opcode": "operator_join", "next": null, "parent": "cat",
              "inputs": {"STRING1": [3, "rs", [10, "0"]],
                         "STRING2": [1, [10, "ab"]]},
              "fields": {}, "topLevel": false, "shadow": false},
     "rs": {"opcode": "data_variable", "next": null, "parent": "join",
            "inputs": {}, "fields": {"VARIABLE": ["s", "vs"]},
            "topLevel": false, "shadow": false},
     "tag": {"opcode": "data_addtolist", "next": null, "parent": "loop",
             "inputs": {"ITEM": [2, "len"]},
             "fields": {"LIST": ["L", "vL"]}, "topLevel": false,
             "shadow": false},
     "len": {"opcode": "operator_length", "next": null, "parent": "tag",
             "inputs": {"STRING": [3, "rs2", [10, "0"]]}, "fields": {},
             "topLevel": false, "shadow": false},
     "rs2": {"opcode": "data_variable", "next": null, "parent": "len",
             "inputs": {}, "fields": {"VARIABLE": ["s", "vs"]},
             "topLevel": false, "shadow": false}
   },
   "comments": {}, "costumes": [], "sounds": [], "currentCostume": 0,
   "volume": 100, "visible": true, "x": 0, "y": 0, "size": 100,
   "direction": 90, "draggable": false, "rotationStyle": "all around"}
 ],
 "monitors": [], "extensions": [], "meta": {"semver": "3.0.0"}
})JSON";

struct RunResult {
    bool ok = false;
    double x = 0;
    double y = 0;
    double sLength = -1;
    std::vector<double> list;
};

RunResult runProject(const char* json, JitMode mode, bool useMlir) {
    const JitMode savedMode = jitMode();
    mlir::setForceEnabled(useMlir ? 1 : 0);
    setJitMode(mode);
    RunResult out;
    {
        Runtime rt;
        if (!loadSb3(rt, json, nullptr).ok) {
            setJitMode(savedMode);
            mlir::setForceEnabled(-1);
            return out;
        }
        Target* target = rt.getSpriteTargetByName(u"S");
        if (!target) {
            setJitMode(savedMode);
            mlir::setForceEnabled(-1);
            return out;
        }
        rt.greenFlag();
        rt.runUntilDone(20000);
        Variable* x = target->lookupVariableById("vx");
        Variable* s = target->lookupVariableById("vs");
        Variable* l = target->lookupVariableById("vL");
        if (x) out.x = cast::toNumber(x->value);
        if (Variable* y = target->lookupVariableById("vy"))
            out.y = cast::toNumber(y->value);
        if (s) out.sLength =
            double(cast::toString(s->value)->view().size());
        if (l && l->list) {
            for (uint32_t i = 0; i < l->list->len; ++i)
                out.list.push_back(cast::toNumber(l->list->items[i]));
        }
        out.ok = x != nullptr || s != nullptr || l != nullptr;
    }
    setJitMode(savedMode);
    mlir::setForceEnabled(-1);
    return out;
}

// warp 手続き（procedures_definition / procedures_call / argument reporter）
// を含む sb3。repeat 本体は数値 2 変数の定数ステップ更新なので、MLIR では
// 64 反復チャンクの高速経路に入る。
const char* kWarpProject = R"JSON({
 "targets": [
  {"isStage": true, "name": "Stage", "variables": {}, "lists": {},
   "broadcasts": {}, "blocks": {}, "comments": {}, "costumes": [],
   "sounds": [], "currentCostume": 0, "volume": 100, "layerOrder": 0},
  {"isStage": false, "name": "S", "layerOrder": 1,
   "variables": {"vx": ["x", 0], "vy": ["y", 0], "vs": ["s", ""]},
   "lists": {"vL": ["L", []]}, "broadcasts": {},
   "blocks": {
     "hat": {"opcode": "event_whenflagclicked", "next": "call",
             "parent": null, "inputs": {}, "fields": {}, "topLevel": true,
             "shadow": false},
     "call": {"opcode": "procedures_call", "next": "after",
              "parent": "hat", "inputs": {"argN": [1, [6, "200000"]]},
              "fields": {}, "topLevel": false, "shadow": false,
              "mutation": {"tagName": "mutation", "children": [],
                           "proccode": "spin %s",
                           "argumentids": "[\"argN\"]",
                           "argumentnames": "[\"n\"]",
                           "argumentdefaults": "[\"0\"]",
                           "warp": "true"}},
     "after": {"opcode": "data_addtolist", "next": "after2",
               "parent": "call", "inputs": {"ITEM": [3, "rvx", [10, "0"]]},
               "fields": {"LIST": ["L", "vL"]}, "topLevel": false,
               "shadow": false},
     "rvx": {"opcode": "data_variable", "next": null, "parent": "after",
             "inputs": {}, "fields": {"VARIABLE": ["x", "vx"]},
             "topLevel": false, "shadow": false},
     "after2": {"opcode": "data_addtolist", "next": null,
                "parent": "after", "inputs": {"ITEM": [3, "rvy", [10, "0"]]},
                "fields": {"LIST": ["L", "vL"]}, "topLevel": false,
                "shadow": false},
     "rvy": {"opcode": "data_variable", "next": null, "parent": "after2",
             "inputs": {}, "fields": {"VARIABLE": ["y", "vy"]},
             "topLevel": false, "shadow": false},
     "def": {"opcode": "procedures_definition", "next": "setx",
             "parent": null, "inputs": {"custom_block": [1, "proto"]},
             "fields": {}, "topLevel": true, "shadow": false},
     "proto": {"opcode": "procedures_prototype", "next": null,
               "parent": "def", "inputs": {"argN": [1, "argsh"]},
               "fields": {}, "topLevel": false, "shadow": true,
               "mutation": {"tagName": "mutation", "children": [],
                            "proccode": "spin %s",
                            "argumentids": "[\"argN\"]",
                            "argumentnames": "[\"n\"]",
                            "argumentdefaults": "[\"0\"]",
                            "warp": "true"}},
     "argsh": {"opcode": "argument_reporter_string_number", "next": null,
               "parent": "proto", "inputs": {},
               "fields": {"VALUE": ["n", null]}, "topLevel": false,
               "shadow": true},
     "setx": {"opcode": "data_setvariableto", "next": "loop",
              "parent": "def", "inputs": {"VALUE": [1, [4, "0"]]},
              "fields": {"VARIABLE": ["x", "vx"]}, "topLevel": false,
              "shadow": false},
     "loop": {"opcode": "control_repeat", "next": null, "parent": "setx",
              "inputs": {"TIMES": [3, "argr", [6, "0"]],
                         "SUBSTACK": [2, "chg"]}, "fields": {},
              "topLevel": false, "shadow": false},
     "argr": {"opcode": "argument_reporter_string_number", "next": null,
              "parent": "loop", "inputs": {}, "fields": {"VALUE": ["n", null]},
              "topLevel": false, "shadow": false},
     "chg": {"opcode": "data_changevariableby", "next": "chg2",
             "parent": "loop", "inputs": {"VALUE": [1, [4, "1"]]},
             "fields": {"VARIABLE": ["x", "vx"]}, "topLevel": false,
             "shadow": false},
     "chg2": {"opcode": "data_changevariableby", "next": null,
              "parent": "chg", "inputs": {"VALUE": [1, [4, "2"]]},
              "fields": {"VARIABLE": ["y", "vy"]}, "topLevel": false,
              "shadow": false}
   },
   "comments": {}, "costumes": [], "sounds": [], "currentCostume": 0,
   "volume": 100, "visible": true, "x": 0, "y": 0, "size": 100,
   "direction": 90, "draggable": false, "rotationStyle": "all around"}
 ],
 "monitors": [], "extensions": [], "meta": {"semver": "3.0.0"}
})JSON";

// warp 手続きを 0 回呼ぶケース: 文字列変数を数値へ書き換えてはいけない。
const char* kWarpZeroProject = R"JSON({
 "targets": [
  {"isStage": true, "name": "Stage", "variables": {}, "lists": {},
   "broadcasts": {}, "blocks": {}, "comments": {}, "costumes": [],
   "sounds": [], "currentCostume": 0, "volume": 100, "layerOrder": 0},
  {"isStage": false, "name": "S", "layerOrder": 1,
   "variables": {"vx": ["x", 0], "vs": ["s", ""]},
   "lists": {"vL": ["L", []]}, "broadcasts": {},
   "blocks": {
     "hat": {"opcode": "event_whenflagclicked", "next": "sets",
             "parent": null, "inputs": {}, "fields": {}, "topLevel": true,
             "shadow": false},
     "sets": {"opcode": "data_setvariableto", "next": "call",
              "parent": "hat", "inputs": {"VALUE": [1, [10, "abc"]]},
              "fields": {"VARIABLE": ["s", "vs"]}, "topLevel": false,
              "shadow": false},
     "call": {"opcode": "procedures_call", "next": "after",
              "parent": "sets", "inputs": {"argN": [1, [6, "0"]]},
              "fields": {}, "topLevel": false, "shadow": false,
              "mutation": {"tagName": "mutation", "children": [],
                           "proccode": "spin %s",
                           "argumentids": "[\"argN\"]",
                           "argumentnames": "[\"n\"]",
                           "argumentdefaults": "[\"0\"]",
                           "warp": "true"}},
     "after": {"opcode": "data_addtolist", "next": null, "parent": "call",
               "inputs": {"ITEM": [3, "rvs", [10, "0"]]},
               "fields": {"LIST": ["L", "vL"]}, "topLevel": false,
               "shadow": false},
     "rvs": {"opcode": "data_variable", "next": null, "parent": "after",
             "inputs": {}, "fields": {"VARIABLE": ["s", "vs"]},
             "topLevel": false, "shadow": false},
     "def": {"opcode": "procedures_definition", "next": "loop",
             "parent": null, "inputs": {"custom_block": [1, "proto"]},
             "fields": {}, "topLevel": true, "shadow": false},
     "proto": {"opcode": "procedures_prototype", "next": null,
               "parent": "def", "inputs": {"argN": [1, "argsh"]},
               "fields": {}, "topLevel": false, "shadow": true,
               "mutation": {"tagName": "mutation", "children": [],
                            "proccode": "spin %s",
                            "argumentids": "[\"argN\"]",
                            "argumentnames": "[\"n\"]",
                            "argumentdefaults": "[\"0\"]",
                            "warp": "true"}},
     "argsh": {"opcode": "argument_reporter_string_number", "next": null,
               "parent": "proto", "inputs": {},
               "fields": {"VALUE": ["n", null]}, "topLevel": false,
               "shadow": true},
     "loop": {"opcode": "control_repeat", "next": null, "parent": "def",
              "inputs": {"TIMES": [3, "argr", [6, "0"]],
                         "SUBSTACK": [2, "chg"]}, "fields": {},
              "topLevel": false, "shadow": false},
     "argr": {"opcode": "argument_reporter_string_number", "next": null,
              "parent": "loop", "inputs": {}, "fields": {"VALUE": ["n", null]},
              "topLevel": false, "shadow": false},
     "chg": {"opcode": "data_changevariableby", "next": null, "parent": "loop",
             "inputs": {"VALUE": [1, [4, "1"]]},
             "fields": {"VARIABLE": ["s", "vs"]}, "topLevel": false,
             "shadow": false}
   },
   "comments": {}, "costumes": [], "sounds": [], "currentCostume": 0,
   "volume": 100, "visible": true, "x": 0, "y": 0, "size": 100,
   "direction": 90, "draggable": false, "rotationStyle": "all around"}
 ],
 "monitors": [], "extensions": [], "meta": {"semver": "3.0.0"}
})JSON";

void testWarpProcedures() {
    const RunResult ref = runProject(kWarpProject, JitMode::Off, false);
    check(ref.ok && ref.x == 200000.0 && ref.y == 400000.0,
          "warp procedure interpreter reference");
    const RunResult jit = runProject(kWarpProject, JitMode::Eager, false);
    check(jit.ok && jit.x == ref.x && jit.y == ref.y,
          "warp procedure legacy JIT matches");
    const uint64_t before = mlir::compiledScripts();
    const uint64_t fallbacks = mlir::fallbackScripts();
    const RunResult mlirRun = runProject(kWarpProject, JitMode::Eager, true);
    check(mlir::compiledScripts() > before,
          "warp procedure compiled through MLIR");
    check(mlir::fallbackScripts() == fallbacks,
          "warp procedure is in the MLIR subset");
    check(mlirRun.ok && mlirRun.x == ref.x && mlirRun.y == ref.y,
          "warp procedure MLIR matches the interpreter");
    check(mlirRun.list.size() == ref.list.size() && mlirRun.list == ref.list,
          "warp procedure MLIR list matches");

    // 0 回反復では文字列のまま（chunked 経路の書き戻しガード）。
    const RunResult zref = runProject(kWarpZeroProject, JitMode::Off, false);
    check(zref.ok && zref.sLength == 3.0, "zero-warp interpreter keeps string");
    const RunResult zmlir =
        runProject(kWarpZeroProject, JitMode::Eager, true);
    check(zmlir.ok && zmlir.sLength == 3.0,
          "zero-warp MLIR keeps the string (0 iterations)");
}

// IR レベル: 手続き呼出し・引数 reporter・warp 属性の lowering。
void testProcedureLowering() {
    mlir::Module m;
    auto* entry = m.createFunction("xyo_w", {mlir::Type::ctx()});
    mlir::Builder b(m);
    b.setBlock(entry->body);
    b.create("xyo.call", {makeConst(m, b, 1)}, {},
             {{"callee", mlir::Attr::str("xyo_w_p0")}});
    b.createTerminator("func.return", {});
    auto* proc = m.createFunction(
        "xyo_w_p0", {mlir::Type::ctx(), mlir::Type::i64()},
        {{"warp", mlir::Attr::integer(1)}});
    mlir::Builder pb(m);
    pb.setBlock(proc->body);
    pb.create("xyo.arg", {}, {mlir::Type::value()},
              {{"index", mlir::Attr::integer(0)}});
    pb.createTerminator("func.return", {});

    llvm::LLVMContext lc;
    llvm::Module dest("test", lc);
    const mlir::LoweredFunction lowered =
        mlir::lowerToLLVM(m, dest, lc, "xyo_w");
    check(lowered.ok, "procedure module lowers");
    llvm::Function* entryFn = dest.getFunction("xyo_w");
    llvm::Function* procFn = dest.getFunction("xyo_w_p0");
    check(entryFn != nullptr && procFn != nullptr,
          "procedure function is emitted");
    check(procFn && procFn->arg_size() == 2,
          "procedure takes ctx + one argument");
    bool callsProc = false;
    if (entryFn) {
        for (llvm::Instruction& inst : entryFn->getEntryBlock())
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst))
                callsProc = callsProc || call->getCalledFunction() == procFn;
    }
    check(callsProc, "entry calls the procedure function");
    std::string text;
    llvm::raw_string_ostream os(text);
    if (procFn) procFn->print(os);
    // warp 手続きは入り口で isWarp=1 を立てる。
    check(text.find("store i32 1") != std::string::npos,
          "warp procedure sets isWarp at entry");
}

void testExecutionParity() {
    const RunResult ref = runProject(kLoopProject, JitMode::Off, false);
    check(ref.ok && ref.x == 1000.0, "interpreter reference value");
    check(ref.list.size() == 1 && ref.list[0] == 1000.0,
          "interpreter reference list");
    const RunResult interp = runProject(kLoopProject, JitMode::Eager, false);
    check(interp.ok && interp.x == ref.x, "legacy JIT matches reference");

    const uint64_t before = mlir::compiledScripts();
    const uint64_t fallbacksBefore = mlir::fallbackScripts();
    const RunResult mlirRun = runProject(kLoopProject, JitMode::Eager, true);
    check(mlir::compiledScripts() > before, "MLIR path compiled the script");
    check(mlir::fallbackScripts() == fallbacksBefore,
          "no fallback for the supported subset");
    check(mlirRun.ok && mlirRun.x == ref.x, "MLIR matches the interpreter");
    check(mlirRun.list.size() == ref.list.size() &&
              mlirRun.list == ref.list,
          "MLIR matches the interpreter list");

    // 文字列リテラル（GC 管理 SStr）を含むスクリプト。
    const RunResult strRef = runProject(kStringProject, JitMode::Off, false);
    check(strRef.ok && strRef.sLength == 4000.0, "string reference length");
    const uint64_t beforeStr = mlir::compiledScripts();
    const RunResult strMlir = runProject(kStringProject, JitMode::Eager, true);
    check(mlir::compiledScripts() > beforeStr,
          "string script compiled through MLIR");
    check(strMlir.ok && strMlir.sLength == strRef.sLength,
          "string script matches the interpreter");
    check(strMlir.list.size() == strRef.list.size() &&
              strMlir.list == strRef.list,
          "string list matches the interpreter");

    // looks_say は compat 経由で MLIR が扱うようになったため、ここでは
    // フォールバックを明示的に強制して（guarded native var slots は MLIR
    // 対象外）状態が保たれることを確認する。
    const uint64_t fallbacksBefore2 = mlir::fallbackScripts();
    setenv("XYO_RT_GUARD_NATIVE_VAR_SLOTS", "1", 1);
    const RunResult fallback = runProject(kFallbackProject, JitMode::Eager,
                                          true);
    unsetenv("XYO_RT_GUARD_NATIVE_VAR_SLOTS");
    check(mlir::fallbackScripts() > fallbacksBefore2,
          "unsupported script fell back to the legacy backend");
    check(fallback.ok && fallback.x == 100.0,
          "fallback script still produces the expected state");
}

#endif // XYO_JIT && !__EMSCRIPTEN__

} // namespace

int test_mlir_main() {
#if defined(XYO_JIT) && !defined(__EMSCRIPTEN__)
    const int before = failures;
    testIrAndPasses();
    testVerifierRejectsUseBeforeDef();
    testWarpProcedures();
    testProcedureLowering();
    testExecutionParity();
    std::printf("mlir frontend: %d checks, %d failures\n", checks,
                failures - before);
    return failures - before;
#else
    std::printf("mlir frontend: skipped (JIT disabled)\n");
    return 0;
#endif
}
