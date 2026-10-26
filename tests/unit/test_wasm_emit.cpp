// xyo-cpp: wasm エンコーダ（src/wasm/wasm_emit.hpp）の自己テスト。
//
// (1) 構造検証: magic/version、セクション順序・長さの整合（LEB の回帰）
// (2) 生成物のダンプ: XYO_WASM_EMIT_OUT=<path> で .wasm を書き出す。
//     これを Node で WebAssembly.instantiate して実際に動かす検証は
//     tests/wasm/instantiate_check.mjs が行う（エンコーダが壊れていない
//     ことの最終確認は実 wasm ランタイムでの instantiate）。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "wasm/wasm_emit.hpp"

using namespace xyo::wasm;

namespace {

int g_checks = 0, g_fails = 0;

void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) {
        ++g_fails;
        std::printf("FAIL wasm_emit: %s\n", what);
    }
}

uint32_t ulebAt(const std::vector<uint8_t>& b, size_t& p) {
    uint32_t v = 0;
    int shift = 0;
    for (;;) {
        uint8_t x = b[p++];
        v |= uint32_t(x & 0x7f) << shift;
        if (!(x & 0x80)) break;
        shift += 7;
    }
    return v;
}

/// 生成モジュールを構築（PoC: f(x) は helper_add(x) を mem[x] に書き、
/// mem[x] を返す）。ランタイムの memory とヘルパを import する。
std::vector<uint8_t> buildPoC() {
    Module m;
    const uint32_t t = m.type({ValType::I32}, {ValType::I32});
    m.importMemory("env", "memory", 256);
    m.importFunc("env", "helper_add", t);
    const uint32_t f = m.addFunc(t);
    m.exportFunc("f", f);
    Code& c = m.code(f);
    // mem[x] = helper_add(x); return mem[x];
    c.localGet(0);   // addr
    c.localGet(0);   // arg
    c.call(0);       // helper_add
    c.i32Store(2, 0);
    c.localGet(0);
    c.i32Load(2, 0);
    c.end();
    return m.finish();
}

} // namespace

int test_wasm_emit_main() {
    const std::vector<uint8_t> bytes = buildPoC();

    check(bytes.size() > 8, "module non-empty");
    check(bytes[0] == 0x00 && bytes[1] == 0x61 && bytes[2] == 0x73 &&
              bytes[3] == 0x6d,
          "magic \\0asm");
    check(bytes[4] == 0x01 && bytes[5] == 0 && bytes[6] == 0 &&
              bytes[7] == 0,
          "version 1");

    // セクションを走査し、id の昇順（1,2,3,7,10）と長さの整合を見る。
    std::vector<uint8_t> ids;
    size_t p = 8;
    bool lengthsOk = true;
    while (p < bytes.size()) {
        uint8_t id = bytes[p++];
        if (p > bytes.size()) { lengthsOk = false; break; }
        const uint32_t len = ulebAt(bytes, p);
        if (p + len > bytes.size()) { lengthsOk = false; break; }
        ids.push_back(id);
        p += len;
    }
    check(p == bytes.size(), "sections consume exactly the buffer");
    check(lengthsOk, "section lengths in range");
    check(ids.size() == 5, "5 sections (type/import/func/export/code)");
    if (ids.size() == 5)
        check(ids[0] == 1 && ids[1] == 2 && ids[2] == 3 && ids[3] == 7 &&
                  ids[4] == 10,
              "section order 1,2,3,7,10");

    // 型の重複排除（同じ型を 2 回登録しても type セクションは 1 個）
    Module m2;
    uint32_t a = m2.type({ValType::I64}, {ValType::F64});
    uint32_t b = m2.type({ValType::I64}, {ValType::F64});
    check(a == b, "type dedup");

    // LEB128 の境界値
    Writer w;
    w.uleb(0);
    w.uleb(127);
    w.uleb(128);
    w.uleb(16384);
    check(w.b.size() == 1 + 1 + 2 + 3, "uleb128 sizes");
    check(w.b[0] == 0 && w.b[1] == 127 && w.b[2] == 0x80 && w.b[3] == 0x01,
          "uleb128 values");
    Writer s;
    s.sleb(0);
    s.sleb(63);
    s.sleb(-1);
    s.sleb(-64);
    check(s.b[0] == 0 && s.b[1] == 63 && s.b[2] == 0x7f && s.b[3] == 0x40,
          "sleb128 values");

    // ダンプ（任意）
    if (const char* out = std::getenv("XYO_WASM_EMIT_OUT")) {
        std::ofstream f(out, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                std::streamsize(bytes.size()));
        if (f) std::printf("wasm_emit: wrote %zu bytes -> %s\n",
                           bytes.size(), out);
    }

    std::printf("wasm_emit: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
