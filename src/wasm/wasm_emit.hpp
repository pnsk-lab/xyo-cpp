// xyo-cpp: WebAssembly バイナリエンコーダ（Phase C の wasm JIT 用）。
//
// ランタイム自体が wasm にコンパイルされるため、実行時に wasm モジュールを
// 生成するには LLVM ではなく自前のエンコーダが要る。ここでは
//   - 型 / import / 関数 / export / code セクションの組み立て
//   - 関数ボディの命令エミッタ（i32/i64/f32/f64・制御構文・メモリアクセス）
// を提供する。依存は <cstdint>/<cstring>/<string>/<vector> のみで、
// ネイティブ・wasm32 の双方で動く（ホストの wasm ランタイム内で呼べる）。
//
// 使い方（tests/unit/test_wasm_emit.cpp 参照）:
//   Module m;
//   uint32_t t = m.type({ValType::I32}, {ValType::I32});
//   m.importMemory("env", "memory", /*minPages=*/256, /*maxPages=*/0, false);
//   m.importFunc("env", "helper_add", t);
//   uint32_t f = m.addFunc(t);
//   m.exportFunc("f", f);
//   auto& c = m.code(f);
//   c.localGet(0); c.localGet(0); c.call(0);
//   c.i32Store(2, 0); c.localGet(0); c.i32Load(2, 0); c.end();
//   auto bytes = m.finish();   // .wasm のバイト列
//
// 注意: 生成モジュールは import した memory を共有する（ランタイムの
// WebAssembly.Memory を JS から渡す）。ポインタは同じ線形メモリのオフセット。
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace xyo::wasm {

enum class ValType : uint8_t {
    I32 = 0x7f,
    I64 = 0x7e,
    F32 = 0x7d,
    F64 = 0x7c,
    FuncRef = 0x70,
    ExternRef = 0x6f,
};

/// バイト列ライタ（LEB128 / 固定幅 LE を含む）。
struct Writer {
    std::vector<uint8_t> b;

    void u8(uint8_t v) { b.push_back(v); }
    void uleb(uint64_t v) {
        do {
            uint8_t x = uint8_t(v & 0x7f);
            v >>= 7;
            if (v) x |= 0x80;
            b.push_back(x);
        } while (v);
    }
    void sleb(int64_t v) {
        bool more = true;
        while (more) {
            uint8_t x = uint8_t(v & 0x7f);
            v >>= 7;
            if ((v == 0 && !(x & 0x40)) || (v == -1 && (x & 0x40)))
                more = false;
            else
                x |= 0x80;
            b.push_back(x);
        }
    }
    void f64(double d) {
        uint64_t u;
        std::memcpy(&u, &d, 8);
        for (int i = 0; i < 8; ++i) b.push_back(uint8_t(u >> (8 * i)));
    }
    void f32(float f) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        for (int i = 0; i < 4; ++i) b.push_back(uint8_t(u >> (8 * i)));
    }
    void raw(const void* p, size_t n) {
        const uint8_t* q = static_cast<const uint8_t*>(p);
        b.insert(b.end(), q, q + n);
    }
    void name(const std::string& s) {
        uleb(s.size());
        raw(s.data(), s.size());
    }
    size_t size() const { return b.size(); }
};

struct FuncType {
    std::vector<ValType> params, results;
    bool operator==(const FuncType& o) const {
        return params == o.params && results == o.results;
    }
};

struct ImportEntry {
    std::string mod, name;
    uint8_t kind = 0;        // 0x00 = func, 0x02 = memory
    uint32_t typeIdx = 0;    // func: 型インデックス
    uint32_t minPages = 0;   // memory
    uint32_t maxPages = 0;   // memory
    bool hasMax = false;
};

struct ExportEntry {
    std::string name;
    uint8_t kind = 0;   // 0x00 = func
    uint32_t index = 0;
};

class Module;

/// 1 関数分のボディ（命令列）エミッタ。
/// 呼び出し規約: 命令は wasm のスタックマシンそのまま（順序に注意）。
class Code {
public:
    std::vector<std::pair<uint32_t, ValType>> locals;   // (count, type)
    Writer w;

    /// ローカル変数を n 個追加（エントリで宣言）。
    void local(ValType t, uint32_t n = 1) {
        if (n) locals.emplace_back(n, t);
    }

    // --- 制御 ---
    void unreachable() { op(0x00); }
    void nop() { op(0x01); }
    void block() { op(0x02); op(0x40); }          // void block
    void loop() { op(0x03); op(0x40); }           // void loop
    void if_() { op(0x04); op(0x40); }            // void if
    void else_() { op(0x05); }
    void end() { op(0x0b); }
    void br(uint32_t depth) { op(0x0c); w.uleb(depth); }
    void brIf(uint32_t depth) { op(0x0d); w.uleb(depth); }
    void return_() { op(0x0f); }
    void call(uint32_t f) { op(0x10); w.uleb(f); }

    // --- ローカル / グローバル ---
    void localGet(uint32_t i) { op(0x20); w.uleb(i); }
    void localSet(uint32_t i) { op(0x21); w.uleb(i); }
    void localTee(uint32_t i) { op(0x22); w.uleb(i); }
    void globalGet(uint32_t i) { op(0x23); w.uleb(i); }
    void globalSet(uint32_t i) { op(0x24); w.uleb(i); }
    void drop() { op(0x1a); }
    void select() { op(0x1b); }

    // --- メモリ ---
    void memarg(uint8_t opcode, uint32_t alignLog2, uint32_t offset) {
        op(opcode);
        w.uleb(alignLog2);
        w.uleb(offset);
    }
    void i32Load(uint32_t a, uint32_t o = 0) { memarg(0x28, a, o); }
    void i64Load(uint32_t a, uint32_t o = 0) { memarg(0x29, a, o); }
    void f64Load(uint32_t a, uint32_t o = 0) { memarg(0x2b, a, o); }
    void i32Load8U(uint32_t a, uint32_t o = 0) { memarg(0x2d, a, o); }
    void i32Load16U(uint32_t a, uint32_t o = 0) { memarg(0x2f, a, o); }
    void i32Store(uint32_t a, uint32_t o = 0) { memarg(0x36, a, o); }
    void i64Store(uint32_t a, uint32_t o = 0) { memarg(0x37, a, o); }
    void f64Store(uint32_t a, uint32_t o = 0) { memarg(0x39, a, o); }
    void i32Store8(uint32_t a, uint32_t o = 0) { memarg(0x3a, a, o); }
    void i32Store16(uint32_t a, uint32_t o = 0) { memarg(0x3b, a, o); }
    void memorySize() { op(0x3f); w.uleb(0); }
    void memoryGrow() { op(0x40); w.uleb(0); }

    // --- 定数 ---
    void i32Const(int32_t v) { op(0x41); w.sleb(v); }
    void i64Const(int64_t v) { op(0x42); w.sleb(v); }
    void f32Const(float v) { op(0x43); w.f32(v); }
    void f64Const(double v) { op(0x44); w.f64(v); }

    // --- i32 比較 ---
    void i32Eqz() { op(0x45); }
    void i32Eq() { op(0x46); }
    void i32Ne() { op(0x47); }
    void i32LtS() { op(0x48); }
    void i32LtU() { op(0x49); }
    void i32GtS() { op(0x4a); }
    void i32GtU() { op(0x4b); }
    void i32LeS() { op(0x4c); }
    void i32GeS() { op(0x4e); }
    // --- i64 比較 ---
    void i64Eqz() { op(0x50); }
    void i64Eq() { op(0x51); }
    void i64Ne() { op(0x52); }
    void i64LtS() { op(0x53); }
    void i64GtS() { op(0x55); }
    void i64LeS() { op(0x57); }
    void i64GeS() { op(0x59); }
    // --- f64 比較 ---
    void f64Eq() { op(0x61); }
    void f64Ne() { op(0x62); }
    void f64Lt() { op(0x63); }
    void f64Gt() { op(0x64); }
    void f64Le() { op(0x65); }
    void f64Ge() { op(0x66); }

    // --- i32 演算 ---
    void i32Add() { op(0x6a); }
    void i32Sub() { op(0x6b); }
    void i32Mul() { op(0x6c); }
    void i32DivS() { op(0x6d); }
    void i32DivU() { op(0x6e); }
    void i32RemS() { op(0x6f); }
    void i32And() { op(0x71); }
    void i32Or() { op(0x72); }
    void i32Xor() { op(0x73); }
    void i32Shl() { op(0x74); }
    void i32ShrS() { op(0x75); }
    void i32ShrU() { op(0x76); }
    // --- i64 演算 ---
    void i64Add() { op(0x7c); }
    void i64Sub() { op(0x7d); }
    void i64Mul() { op(0x7e); }
    void i64And() { op(0x83); }
    void i64Or() { op(0x84); }
    void i64Xor() { op(0x85); }
    void i64Shl() { op(0x86); }
    void i64ShrS() { op(0x87); }
    void i64ShrU() { op(0x88); }
    // --- f64 演算 ---
    void f64Abs() { op(0x99); }
    void f64Neg() { op(0x9a); }
    void f64Ceil() { op(0x9b); }
    void f64Floor() { op(0x9c); }
    void f64Trunc() { op(0x9d); }
    void f64Nearest() { op(0x9e); }
    void f64Sqrt() { op(0x9f); }
    void f64Add() { op(0xa0); }
    void f64Sub() { op(0xa1); }
    void f64Mul() { op(0xa2); }
    void f64Div() { op(0xa3); }
    void f64Min() { op(0xa4); }
    void f64Max() { op(0xa5); }
    void f64CopySign() { op(0xa6); }

    // --- 変換 ---
    void i32WrapI64() { op(0xa7); }
    void i32TruncF64S() { op(0xaa); }
    void i32TruncF64U() { op(0xab); }
    void i64ExtendI32S() { op(0xac); }
    void i64ExtendI32U() { op(0xad); }
    void i64TruncF64S() { op(0xb0); }
    void i64TruncF64U() { op(0xb1); }
    void f64ConvertI32S() { op(0xb7); }
    void f64ConvertI32U() { op(0xb8); }
    void f64ConvertI64S() { op(0xb9); }
    void f64ConvertI64U() { op(0xba); }
    void i64ReinterpretF64() { op(0xbd); }
    void f64ReinterpretI64() { op(0xbf); }

    void op(uint8_t o) { w.u8(o); }
};

/// モジュールビルダー。セクション順序（type=1, import=2, func=3,
/// export=7, code=10）を finish() で正しく並べる。
class Module {
public:
    /// 型を登録（同一型は再利用）。
    uint32_t type(const std::vector<ValType>& params,
                  const std::vector<ValType>& results) {
        FuncType t{params, results};
        for (size_t i = 0; i < types_.size(); ++i)
            if (types_[i] == t) return uint32_t(i);
        types_.push_back(t);
        return uint32_t(types_.size() - 1);
    }

    void importMemory(const std::string& mod, const std::string& name,
                      uint32_t minPages, uint32_t maxPages = 0,
                      bool hasMax = false) {
        ImportEntry e;
        e.mod = mod;
        e.name = name;
        e.kind = 0x02;
        e.minPages = minPages;
        e.maxPages = maxPages;
        e.hasMax = hasMax;
        imports_.push_back(std::move(e));
    }

    uint32_t importFunc(const std::string& mod, const std::string& name,
                        uint32_t typeIdx) {
        ImportEntry e;
        e.mod = mod;
        e.name = name;
        e.kind = 0x00;
        e.typeIdx = typeIdx;
        imports_.push_back(std::move(e));
        ++importedFuncs_;
        return importedFuncs_ - 1;   // import された func のインデックス
    }

    /// 定義関数を追加し、その関数インデックスを返す。
    uint32_t addFunc(uint32_t typeIdx) {
        funcTypes_.push_back(typeIdx);
        codes_.emplace_back();
        return importedFuncs_ + uint32_t(funcTypes_.size() - 1);
    }

    /// 定義関数のボディ（addFunc の順に 1:1）。
    Code& code(uint32_t funcIdx) {
        return codes_[funcIdx - importedFuncs_];
    }

    void exportFunc(const std::string& name, uint32_t funcIdx) {
        exports_.push_back(ExportEntry{name, 0x00, funcIdx});
    }

    /// .wasm バイト列を生成。
    std::vector<uint8_t> finish() const {
        Writer out;
        // magic + version
        const uint8_t hdr[8] = {0x00, 0x61, 0x73, 0x6d,
                                0x01, 0x00, 0x00, 0x00};
        out.raw(hdr, 8);

        // --- 1: type ---
        if (!types_.empty()) {
            Writer s;
            s.uleb(types_.size());
            for (const auto& t : types_) {
                s.u8(0x60);
                s.uleb(t.params.size());
                for (ValType p : t.params) s.u8(uint8_t(p));
                s.uleb(t.results.size());
                for (ValType r : t.results) s.u8(uint8_t(r));
            }
            section(out, 1, s.b);
        }

        // --- 2: import ---
        if (!imports_.empty()) {
            Writer s;
            s.uleb(imports_.size());
            for (const auto& im : imports_) {
                s.name(im.mod);
                s.name(im.name);
                s.u8(im.kind);
                if (im.kind == 0x00) {
                    s.uleb(im.typeIdx);
                } else {   // memory
                    s.u8(im.hasMax ? 0x01 : 0x00);
                    s.uleb(im.minPages);
                    if (im.hasMax) s.uleb(im.maxPages);
                }
            }
            section(out, 2, s.b);
        }

        // --- 3: function ---
        if (!funcTypes_.empty()) {
            Writer s;
            s.uleb(funcTypes_.size());
            for (uint32_t t : funcTypes_) s.uleb(t);
            section(out, 3, s.b);
        }

        // --- 7: export ---
        if (!exports_.empty()) {
            Writer s;
            s.uleb(exports_.size());
            for (const auto& ex : exports_) {
                s.name(ex.name);
                s.u8(ex.kind);
                s.uleb(ex.index);
            }
            section(out, 7, s.b);
        }

        // --- 10: code ---
        if (!codes_.empty()) {
            Writer s;
            s.uleb(codes_.size());
            for (const auto& c : codes_) {
                Writer body;
                body.uleb(c.locals.size());
                for (const auto& [n, t] : c.locals) {
                    body.uleb(n);
                    body.u8(uint8_t(t));
                }
                body.raw(c.w.b.data(), c.w.b.size());
                s.uleb(body.size());
                s.raw(body.b.data(), body.b.size());
            }
            section(out, 10, s.b);
        }
        return out.b;
    }

    size_t funcCount() const {
        return importedFuncs_ + funcTypes_.size();
    }

private:
    static void section(Writer& out, uint8_t id,
                        const std::vector<uint8_t>& body) {
        out.u8(id);
        out.uleb(body.size());
        out.raw(body.data(), body.size());
    }

    std::vector<FuncType> types_;
    std::vector<ImportEntry> imports_;
    std::vector<uint32_t> funcTypes_;
    std::vector<ExportEntry> exports_;
    std::vector<Code> codes_;
    uint32_t importedFuncs_ = 0;
};

} // namespace xyo::wasm
