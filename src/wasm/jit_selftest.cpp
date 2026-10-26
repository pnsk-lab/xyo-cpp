// xyo-cpp: wasm JIT ブリッジの自己テスト（Phase C の土台検証）。
//
// 実ランタイム（wasm）の中で、
//   1. C++ のエンコーダで wasm モジュールを生成し
//   2. JS ブリッジで instantiate し
//   3. 生成コードにランタイムのメモリへ書かせ、ヘルパを呼び戻させる
// という全経路を 1 コマンドで確認する。
//
//   node <loader> wasm-selftest      （EMSCRIPTEN ビルド）
//   ネイティブでは 0 を返すだけ（テスト互換）。
#include "host_bridge.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#include "wasm_compile.hpp"
#include "wasm_emit.hpp"

#if defined(__EMSCRIPTEN__)

/// 生成コードから呼ばれるヘルパディスパッチャ。
/// JS ブリッジが `Module['_xyo_wasm_h']` として env.h に渡す。
extern "C" int xyo_wasm_h(int id, int a, int b) {
    switch (id) {
        case 1: return a + b;
        case 2: return a * b;
        default: return 0;
    }
}

#endif

extern "C" int xyo_wasm_selftest() {
#if !defined(__EMSCRIPTEN__)
    std::printf(
        "wasm_jit selftest: skipped (native build — run the Emscripten "
        "XYO_WASM_JIT build)\n");
    return 0;
#else
    using namespace xyo::wasm;

    // f(ctx): mem[ctx] = h(1, 7, 5)   → 12
    Module m;
    const uint32_t tf = m.type({ValType::I32}, {});
    const uint32_t th =
        m.type({ValType::I32, ValType::I32, ValType::I32}, {ValType::I32});
    m.importMemory("env", "memory", 256);
    m.importFunc("env", "h", th);
    const uint32_t f = m.addFunc(tf);
    m.exportFunc("f", f);
    Code& c = m.code(f);
    c.localGet(0);          // addr = ctx
    c.i32Const(1);          // id
    c.i32Const(7);          // a
    c.i32Const(5);          // b
    c.call(0);              // h(1, 7, 5) -> 12
    c.i32Store(2, 0);       // mem[ctx] = 12
    c.end();

    const std::vector<uint8_t> bytes = m.finish();
    int32_t cell = 0;
    const int handle =
        instantiate(bytes.data(), int(bytes.size()));
    if (handle < 0) {
        std::printf("wasm_jit selftest: instantiate failed\n");
        return 1;
    }
    invoke(handle, int(reinterpret_cast<intptr_t>(&cell)));
    const bool ok = cell == 12;
    std::printf(
        "wasm_jit selftest: bytes=%zu handle=%d cell=%d\n",
        bytes.size(), handle, int(cell));
    std::printf("wasm_jit selftest: %s\n", ok ? "PASS" : "FAIL");

    // --- 第 2 段: スクリプト→wasm コンパイラを実ランタイム内で回す ---
    // v0 = 0; repeat 1000 { v0 = v0 + 1 }; v1 = v0 * 2
    Program p;
    p.nSlots = 2;
    Stmt init;
    init.k = SK::SetVar;
    init.var = 0;
    init.expr = p.exprNum(0.0);
    Stmt body;
    body.k = SK::SetVar;
    body.var = 0;
    body.expr = p.exprBin(EK::Add, p.exprVar(0), p.exprNum(1.0));
    const uint32_t bodyId = p.stmt(std::move(body));
    Stmt rep;
    rep.k = SK::Repeat;
    rep.expr = p.exprNum(1000.0);
    rep.body.push_back(bodyId);
    Stmt dbl;
    dbl.k = SK::SetVar;
    dbl.var = 1;
    dbl.expr = p.exprBin(EK::Mul, p.exprVar(0), p.exprNum(2.0));
    p.main.push_back(p.stmt(std::move(init)));
    p.main.push_back(p.stmt(std::move(rep)));
    p.main.push_back(p.stmt(std::move(dbl)));

    const std::vector<uint8_t> code = compileProgram(p);
    int64_t slots[2] = {0, 0};
    const int h2 = instantiate(code.data(), int(code.size()));
    bool ok2 = false;
    if (h2 >= 0) {
        invoke(h2, int(reinterpret_cast<intptr_t>(slots)));
        double v0 = 0, v1 = 0;
        std::memcpy(&v0, &slots[0], 8);
        std::memcpy(&v1, &slots[1], 8);
        ok2 = (v0 == 1000.0 && v1 == 2000.0);
        std::printf(
            "wasm_jit selftest2: code=%zu handle=%d v0=%.1f v1=%.1f\n",
            code.size(), h2, v0, v1);
    } else {
        std::printf("wasm_jit selftest2: instantiate failed\n");
    }
    std::printf("wasm_jit selftest2: %s\n", ok2 ? "PASS" : "FAIL");
    return (ok && ok2) ? 0 : 1;
#endif
}
