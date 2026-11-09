// xyo-cpp: スクリプト→wasm コンパイラ（src/wasm/wasm_compile.cpp）のテスト。
//
// 生成したモジュールの意味論検証は tests/wasm/compile_check.mjs が行う
// （ネイティブからは wasm を実行できないため、ダンプして Node で回す）。
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

#include "model/blocks.hpp"
#include "wasm/wasm_compile.hpp"

using namespace xyo;
using namespace xyo::wasm;

namespace {
int g_checks = 0, g_fails = 0;
void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) {
        ++g_fails;
        std::printf("FAIL wasm_compile: %s\n", what);
    }
}
} // namespace

int test_wasm_compile_main() {
    // v0 = 0; repeat 1000 { v0 = v0 + 1 }; v1 = v0*2; v2 = 7-2; v3 = 10/4
    Program p;
    p.nSlots = 4;

    const uint32_t zero = p.exprNum(0.0);
    Stmt init;
    init.k = SK::SetVar;
    init.var = 0;
    init.expr = zero;

    const uint32_t one = p.exprNum(1.0);
    const uint32_t inc = p.exprBin(EK::Add, p.exprVar(0), one);
    Stmt body;
    body.k = SK::SetVar;
    body.var = 0;
    body.expr = inc;
    const uint32_t bodyId = p.stmt(std::move(body));

    Stmt rep;
    rep.k = SK::Repeat;
    rep.expr = p.exprNum(1000.0);
    rep.body.push_back(bodyId);

    Stmt dbl;
    dbl.k = SK::SetVar;
    dbl.var = 1;
    dbl.expr = p.exprBin(EK::Mul, p.exprVar(0), p.exprNum(2.0));

    Stmt sub;
    sub.k = SK::SetVar;
    sub.var = 2;
    sub.expr = p.exprBin(EK::Sub, p.exprNum(7.0), p.exprNum(2.0));

    Stmt div;
    div.k = SK::SetVar;
    div.var = 3;
    div.expr = p.exprBin(EK::Div, p.exprNum(10.0), p.exprNum(4.0));

    p.main.push_back(p.stmt(std::move(init)));
    p.main.push_back(p.stmt(std::move(rep)));
    p.main.push_back(p.stmt(std::move(dbl)));
    p.main.push_back(p.stmt(std::move(sub)));
    p.main.push_back(p.stmt(std::move(div)));

    const std::vector<uint8_t> bytes = compileProgram(p);
    check(!bytes.empty(), "module non-empty");
    check(bytes.size() > 8 && bytes[0] == 0x00 && bytes[1] == 0x61 &&
              bytes[2] == 0x73 && bytes[3] == 0x6d,
          "magic \\0asm");
    check(bytes.size() > 8 && bytes[4] == 0x01, "version 1");

    // Conditional-loop lowering is opt-in: whole-script and nonwarp
    // callers have no valid synchronous scheduler continuation.
    // The nested JIT verifier alone may enable bounded atomic loops.
    for (const SK kind : {SK::Until, SK::While, SK::ForEach}) {
        Program guarded;
        guarded.nSlots = 2;
        guarded.useHostMath = true;
        const auto increment = guarded.exprBin(
            EK::Add, guarded.exprVar(0), guarded.exprNum(1.0));
        Stmt body;
        body.k = SK::SetVar;
        body.var = 0;
        body.expr = increment;
        const uint32_t bodyIndex = guarded.stmt(std::move(body));
        Stmt loop;
        loop.k = kind;
        loop.var = 1;
        loop.expr = kind == SK::ForEach
            ? guarded.exprNum(7.0)
            : guarded.exprBin(EK::LessThan, guarded.exprVar(0),
                              guarded.exprNum(7.0));
        loop.body.push_back(bodyIndex);
        guarded.main.push_back(guarded.stmt(std::move(loop)));
        check(compileProgram(guarded).empty(),
              "conditional loop must reject default scheduling");
        guarded.allowBoundedConditionalLoop = true;
        check(!compileProgram(guarded).empty(),
              "verified bounded warp loop should emit module");
    }

    // Synchronous warp tail recursion is prohibited for standalone
    // compilation, even though the encoder supports a verified bounded
    // nested warp execution after explicit opt-in.
    {
        Program recursion;
        recursion.nSlots = 1;
        recursion.useHostMath = true;
        Stmt change;
        change.k = SK::ChangeVar;
        change.var = 0;
        change.expr = recursion.exprNum(1.0);
        const uint32_t changeId = recursion.stmt(std::move(change));
        Stmt tail;
        tail.k = SK::WarpTailRecurse;
        tail.inlineWarp = true;
        tail.expr = recursion.exprBin(
            EK::LessThan, recursion.exprVar(0), recursion.exprNum(5.0));
        tail.body.push_back(changeId);
        recursion.main.push_back(recursion.stmt(std::move(tail)));
        check(compileProgram(recursion).empty(),
              "tail recursion forbidden in default scheduling");
        recursion.allowBoundedConditionalLoop = true;
        check(!compileProgram(recursion).empty(),
              "verified bounded warp tail recursion emits module");
    }

    // Non-tail recursion has a continuation after the recursive call.
    // The bounded warp-only emitter must reject it by default and encode
    // both the entry loop and LIFO suffix-unwinding loop only after opt-in.
    {
        Program recursion;
        recursion.nSlots = 2;
        recursion.useHostMath = true;
        Stmt prefix;
        prefix.k = SK::ChangeVar;
        prefix.var = 0;
        prefix.expr = recursion.exprNum(1.0);
        const uint32_t prefixId = recursion.stmt(std::move(prefix));
        Stmt suffix;
        suffix.k = SK::ChangeVar;
        suffix.var = 1;
        suffix.expr = recursion.exprNum(2.0);
        const uint32_t suffixId = recursion.stmt(std::move(suffix));
        Stmt returnStack;
        returnStack.k = SK::WarpReturnStack;
        returnStack.inlineWarp = true;
        returnStack.expr = recursion.exprBin(
            EK::LessThan, recursion.exprVar(0), recursion.exprNum(5.0));
        returnStack.body.push_back(prefixId);
        returnStack.elseBody.push_back(suffixId);
        recursion.main.push_back(recursion.stmt(std::move(returnStack)));
        check(compileProgram(recursion).empty(),
              "non-tail recursion forbidden in default scheduling");
        recursion.allowBoundedConditionalLoop = true;
        check(!compileProgram(recursion).empty(),
              "bounded warp non-tail return-stack emits module");
    }

    // Explicit multi-return-PC recursion requires an interpreter-owned
    // pure warp root; standalone modules may not skip Scratch yields.
    {
        Program recursion;
        recursion.nSlots = 1;
        recursion.slotIds = {"variable"};
        recursion.useHostMath = true;
        Stmt prefix;
        prefix.k = SK::ChangeVar;
        prefix.var = 0;
        prefix.expr = recursion.exprNum(1.0);
        const uint32_t increment = recursion.stmt(std::move(prefix));
        Stmt suffix;
        suffix.k = SK::ChangeVar;
        suffix.var = 0;
        suffix.expr = recursion.exprNum(2.0);
        const uint32_t after = recursion.stmt(std::move(suffix));
        Stmt call;
        call.k = SK::WarpReturnPcStack;
        call.inlineWarp = true;
        for (int i = 0; i < 2; ++i) {
            WarpReturnSite site;
            site.before.push_back(increment);
            site.condition = recursion.exprBin(
                EK::LessThan, recursion.exprVar(0),
                recursion.exprNum(5 + i));
            call.returnSites.push_back(std::move(site));
        }
        call.elseBody.push_back(after);
        recursion.main.push_back(recursion.stmt(std::move(call)));
        check(compileProgram(recursion).empty(),
              "multi-return-PC recursion forbidden in default scheduling");
        recursion.allowBoundedConditionalLoop = true;
        check(!compileProgram(recursion).empty(),
              "bounded multi-return-PC numeric warp recursion emits");
    }

    if (const char* out = std::getenv("XYO_WASM_COMPILE_OUT")) {
        std::ofstream f(out, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                std::streamsize(bytes.size()));
        if (f)
            std::printf("wasm_compile: wrote %zu bytes -> %s\n", bytes.size(),
                        out);
    }


    // --- 実ブロック木 → Program → wasm ---------------------------------
    // x = 0; repeat 1000 { x = x + 1 }  （x は 1 変数 = slot 0）
    {
        Blocks b(nullptr);
        auto add = [&](const char* id, const char* op, bool topLevel) {
            Block blk;
            blk.id = id;
            blk.opcode = op;
            blk.topLevel = topLevel;
            b.createBlock(std::move(blk));
        };
        auto link = [&](const char* a, const char* n) {
            b.getBlock(a)->next = n;
        };
        add("top", "event_whenflagclicked", true);
        add("s1", "data_setvariableto", false);
        add("lit0", "math_number", false);
        add("s2", "control_repeat", false);
        add("lit1", "math_number", false);
        add("s3", "data_setvariableto", false);
        add("add1", "operator_add", false);
        add("var0", "data_variable", false);
        add("lit2", "math_number", false);

        link("top", "s1");
        link("s1", "s2");
        link("s3", "");
        b.getBlock("s1")->fields["VARIABLE"].id = "v1";
        b.getBlock("lit0")->fields["NUM"] = BlockField{Value::num(0), ""};
        b.getBlock("lit0")->shadow = true;
        b.getBlock("s1")->inputs.items.push_back(
            {"VALUE", BlockInput{"", "lit0"}});
        b.getBlock("s2")->inputs.items.push_back(
            {"TIMES", BlockInput{"", "lit1"}});
        b.getBlock("s2")->inputs.items.push_back(
            {"SUBSTACK", BlockInput{"s3", ""}});
        b.getBlock("lit1")->fields["NUM"] = BlockField{Value::num(1000), ""};
        b.getBlock("lit1")->shadow = true;
        b.getBlock("s3")->fields["VARIABLE"].id = "v1";
        b.getBlock("s3")->inputs.items.push_back(
            {"VALUE", BlockInput{"add1", ""}});
        b.getBlock("add1")->inputs.items.push_back(
            {"NUM1", BlockInput{"var0", ""}});
        b.getBlock("add1")->inputs.items.push_back(
            {"NUM2", BlockInput{"", "lit2"}});
        b.getBlock("var0")->fields["VARIABLE"].id = "v1";
        b.getBlock("lit2")->fields["NUM"] = BlockField{Value::num(1), ""};
        b.getBlock("lit2")->shadow = true;

        Program lp;
        const bool lowered = lowerScript(b, b.getBlock("top"), lp);
        check(lowered, "lowerScript ok");
        check(lp.nSlots == 1, "lowerScript: 1 variable slot");
        check(lp.main.size() == 2, "lowerScript: 2 statements");
        const std::vector<uint8_t> lb = compileProgram(lp);
        check(!lb.empty(), "lowered module non-empty");
        if (const char* out = std::getenv("XYO_WASM_LOWER_OUT")) {
            std::ofstream f(out, std::ios::binary);
            f.write(reinterpret_cast<const char*>(lb.data()),
                    std::streamsize(lb.size()));
            if (f)
                std::printf("wasm_compile: lowered %zu bytes -> %s\n",
                            lb.size(), out);
        }
        // List IDs must remain distinct from scalar staging slots.
        {
            Blocks lists(nullptr);
            auto addBlock = [&](const char* id, const char* opcode) {
                Block block;
                block.id = id;
                block.opcode = opcode;
                lists.createBlock(std::move(block));
            };
            addBlock("hat", "event_whenflagclicked");
            addBlock("write", "data_setvariableto");
            addBlock("item", "data_itemoflist");
            addBlock("index", "math_number");
            lists.getBlock("hat")->next = "write";
            lists.getBlock("write")->fields["VARIABLE"].id = "result";
            lists.getBlock("write")->inputs.items.push_back(
                {"VALUE", BlockInput{"item", ""}});
            lists.getBlock("item")->fields["LIST"].id = "data-list-id";
            lists.getBlock("item")->inputs.items.push_back(
                {"INDEX", BlockInput{"", "index"}});
            lists.getBlock("index")->fields["NUM"] =
                BlockField{Value::num(1), ""};
            Program lp;
            check(lowerScript(lists, lists.getBlock("hat"), lp),
                  "numeric list reporter lowers");
            check(lp.slotIds.size() == 1 && lp.slotIds[0] == "result",
                  "list reporter scalar slot identity");
            check(lp.listIds.size() == 1 && lp.listIds[0] == "data-list-id",
                  "list reporter has distinct rooted identity");
            lp.useHostMath = true;
            check(!compileProgram(lp).empty(), "host list module compiles");
        }
        {
            // List writer nodes are lowered with the same separate list
            // binding namespace as numeric list reporters.
            Blocks lists(nullptr);
            auto addBlock = [&](const char* id, const char* opcode) {
                Block block;
                block.id = id;
                block.opcode = opcode;
                lists.createBlock(std::move(block));
            };
            addBlock("hat", "event_whenflagclicked");
            addBlock("append", "data_addtolist");
            addBlock("replace", "data_replaceitemoflist");
            addBlock("erase", "data_deleteoflist");
            addBlock("index", "math_number");
            addBlock("item", "math_number");
            lists.getBlock("hat")->next = "append";
            lists.getBlock("append")->next = "replace";
            lists.getBlock("replace")->next = "erase";
            for (const auto* id : {"append", "replace", "erase"})
                lists.getBlock(id)->fields["LIST"].id = "list-a";
            for (const auto* id : {"replace", "erase"})
                lists.getBlock(id)->inputs.items.push_back(
                    {"INDEX", BlockInput{"", "index"}});
            for (const auto* id : {"append", "replace"})
                lists.getBlock(id)->inputs.items.push_back(
                    {"ITEM", BlockInput{"", "item"}});
            lists.getBlock("index")->fields["NUM"] =
                BlockField{Value::num(1), ""};
            lists.getBlock("item")->fields["NUM"] =
                BlockField{Value::num(7), ""};
            Program lp;
            check(lowerScript(lists, lists.getBlock("hat"), lp),
                  "list writer nodes lower");
            check(lp.listIds.size() == 1 && lp.listIds[0] == "list-a",
                  "list writer uses a single host binding");
            check(lp.slotIds.empty(), "list writer has no scalar slots");
            check(lp.main.size() == 3 &&
                  lp.stmts[lp.main[0]].k == SK::ListAppend &&
                  lp.stmts[lp.main[1]].k == SK::ListReplace &&
                  lp.stmts[lp.main[2]].k == SK::ListDelete,
                  "list writer opcode ordering");
            lp.useHostMath = true;
            check(!compileProgram(lp).empty(), "host list writer compiles");
        }
        // 非対応 opcode は false（interp フォールバック）
        Blocks bad(nullptr);
        Block t;
        t.id = "t";
        t.opcode = "event_whenflagclicked";
        t.topLevel = true;
        t.next = "x";
        bad.createBlock(std::move(t));
        Block x;
        x.id = "x";
        x.opcode = "looks_say";
        bad.createBlock(std::move(x));
        Program bp;
        check(!lowerScript(bad, bad.getBlock("t"), bp),
              "unsupported opcode -> false");
    }

    std::printf("wasm_compile: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
