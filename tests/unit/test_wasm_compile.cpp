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
