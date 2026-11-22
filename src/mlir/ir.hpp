// xyo-cpp: LLVM の前段に置く Scratch 用の MLIR 風 IR。
//
// 環境に MLIR ライブラリが無いため、MLIR の設計（dialect/op/region/
// block-argument による SSA・verifier・pass pipeline・LLVM への lowering）
// を最小限に自前実装したもの。形状は MLIR の generic op 記法に合わせて
// あり、`print()` の出力はそのまま MLIR の generic syntax として読める。
//
// この層が担うこと:
//   * Scratch ブロック木 → SSA IR（`xyo` dialect）への変換
//   * 構造化制御フロー（`xyo.if` / `xyo.while` / `xyo.repeat` /
//     `xyo.forever`）の region と block argument
//   * verifier / canonicalize / CSE / DCE / 数値 SSA 昇格の pass
//   * LLVM IR への lowering（JitCtx* を受ける `void(ptr)` 関数）
//
// 型:
//   !xyo.value  — NaN-boxed Value のビット列（i64）
//   !xyo.f64    — 数値 SSA（昇格後のみ）
//   !xyo.bool   — i1（比較・条件）
//   !xyo.ctx    — JitCtx*（関数引数のみ）
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace xyo {
struct SStr;
}

namespace xyo::mlir {

// ================================================================ 型 ====

enum class TypeKind : uint8_t { None, Value, F64, Bool, I64, I32, Ptr, Ctx };

struct Type {
    TypeKind kind = TypeKind::None;
    constexpr Type() = default;
    explicit constexpr Type(TypeKind k) : kind(k) {}

    static constexpr Type value() { return Type(TypeKind::Value); }
    static constexpr Type f64() { return Type(TypeKind::F64); }
    static constexpr Type boolean() { return Type(TypeKind::Bool); }
    static constexpr Type i64() { return Type(TypeKind::I64); }
    static constexpr Type i32() { return Type(TypeKind::I32); }
    static constexpr Type ptr() { return Type(TypeKind::Ptr); }
    static constexpr Type ctx() { return Type(TypeKind::Ctx); }

    bool operator==(const Type& o) const { return kind == o.kind; }
    bool operator!=(const Type& o) const { return kind != o.kind; }
    bool valid() const { return kind != TypeKind::None; }
    std::string_view name() const;
};

// ============================================================== 属性 ====

struct Attr {
    enum class Kind : uint8_t { None, Int, Double, Str };
    Kind kind = Kind::None;
    int64_t i = 0;
    double d = 0.0;
    std::string s;

    static Attr integer(int64_t v);
    static Attr real(double v);
    static Attr str(std::string v);

    bool isInt() const { return kind == Kind::Int; }
    bool isDouble() const { return kind == Kind::Double; }
    bool isStr() const { return kind == Kind::Str; }
    int64_t asInt(int64_t fallback = 0) const {
        return kind == Kind::Int ? i : fallback;
    }
    double asDouble(double fallback = 0.0) const {
        return kind == Kind::Double ? d : fallback;
    }
    const std::string& asStr() const;
};

using AttrMap = std::map<std::string, Attr>;

// ======================================================== 値と操作 ====

struct Op;
struct Block;
struct Region;
struct Module;

/// SSA 値。op の結果か、region の block argument のどちらか。
struct Value {
    Op* op = nullptr;      // 結果を持つ op（block argument なら null）
    uint32_t index = 0;    // 結果番号
    Block* block = nullptr;  // block argument の持ち主
    uint32_t argIndex = 0;
    Type type;

    bool valid() const { return op != nullptr || block != nullptr; }
    bool isBlockArg() const { return block != nullptr; }
    explicit operator bool() const { return valid(); }
};

struct Block {
    std::vector<Value> args;
    std::vector<Op*> ops;
    Region* parent = nullptr;
    uint32_t index = 0;

    Op* lastOp() const { return ops.empty() ? nullptr : ops.back(); }
    bool empty() const { return ops.empty(); }
};

struct Region {
    std::vector<std::unique_ptr<Block>> blocks;
    Op* owner = nullptr;

    Block* entry() const {
        return blocks.empty() ? nullptr : blocks.front().get();
    }
    Block* addBlock() {
        blocks.push_back(std::make_unique<Block>());
        Block* b = blocks.back().get();
        b->parent = this;
        b->index = static_cast<uint32_t>(blocks.size() - 1);
        return b;
    }
};

struct Op {
    std::string name;
    std::vector<Value> operands;
    std::vector<Type> resultTypes;
    AttrMap attrs;
    std::vector<std::unique_ptr<Region>> regions;
    Block* parent = nullptr;
    int64_t debugId = 0;

    Region* addRegion();
    Region* region(size_t i) const {
        return i < regions.size() ? regions[i].get() : nullptr;
    }
    bool hasAttr(const std::string& key) const {
        return attrs.find(key) != attrs.end();
    }
    const Attr* attr(const std::string& key) const {
        auto it = attrs.find(key);
        return it == attrs.end() ? nullptr : &it->second;
    }
    std::string_view attrStr(const std::string& key) const {
        const Attr* a = attr(key);
        return a && a->isStr() ? std::string_view(a->s) : std::string_view();
    }
    int64_t attrInt(const std::string& key, int64_t fallback = 0) const {
        const Attr* a = attr(key);
        return a && a->isInt() ? a->i : fallback;
    }
    Value result(size_t i = 0) const {
        Value v;
        v.op = const_cast<Op*>(this);
        v.index = static_cast<uint32_t>(i);
        v.type = i < resultTypes.size() ? resultTypes[i] : Type{};
        return v;
    }
};

/// `xyo` dialect の知識。op 名だけから性質を引く（dialect registry の
/// 最小版）。未知の op は保守的に「副作用あり・pure でない」とみなす。
namespace dialect {
bool isTerminator(std::string_view name);
bool isPure(std::string_view name);       // 未使用なら DCE 可能
bool isReadOnly(std::string_view name);   // メモリ読みだが書きはしない
bool isRegionOp(std::string_view name);
size_t regionCount(std::string_view name);  // 期待する region 数（0 = 非対応）
} // namespace dialect

// ============================================================== Module ====

class Module {
public:
    Module();
    ~Module();

    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    /// op を arena へ確保する（ポインタは Module の寿命まで安定）。
    Op* alloc();
    Op* create(std::string name, std::vector<Value> operands = {},
               std::vector<Type> resultTypes = {}, AttrMap attrs = {});

    struct Function {
        Op* op = nullptr;
        std::string name;
        Block* body = nullptr;
        std::vector<Type> argTypes;
        /// true: 手続き定義の mutation が warp="true"。
        bool warp() const { return op && op->attrInt("warp") != 0; }
    };
    Function* createFunction(std::string name, std::vector<Type> argTypes,
                             AttrMap attrs = {});
    const std::deque<Function>& functions() const { return functions_; }
    std::deque<Function>& functions() { return functions_; }
    Function* function(std::string_view name);

    /// 全 op（関数・region 内を問わず）を生成順に列挙する。
    void forEachOp(const std::function<void(Op&)>& fn) const;
    void forEachOp(const std::function<void(Op&)>& fn);
    size_t opCount() const;

    /// 整数プロモーションの適格変数 id 集合（frontend が設定）。
    /// target-local・非 cloud・slider 非監視・全書き込みが整数値である
    /// ことが静的に分かっている変数だけが入る。空なら整数化しない。
    std::unordered_set<std::string> intEligibleVars;
    /// 変数 id → 非リテラル書き込み（定数でない値の代入）の個数
    /// （frontend が全ブロック走査で数える）。promoter がループ内訳と
    /// 突き合わせて、ループ外の非整数書き込みを検出する。
    /// clean fixpoint で残った変数 id 集合も保持する（数え方と解析の
    /// 証明を一致させるために promoter が参照する）。
    std::unordered_map<std::string, int> intNonLiteralWrites;
    std::unordered_set<std::string> intCleanVars;
    /// 要素が全て整数と証明できたリスト id → 値の区間（frontend が設定）。
    /// 要素読み（xyo.list.item）の整数証明に使う。範囲外読みは ""→0
    /// になるので区間は 0 で広げてある。
    std::unordered_map<std::string, std::pair<int64_t, int64_t>> intListRanges;
    /// 要素が全て整数と証明できたリスト id 集合（範囲は問わない）。
    /// NaN チェック省略に使う（整数は NaN にならない）。
    /// intListRanges より緩い：変数 append も許す（clean なら）。
    std::unordered_set<std::string> intValuedLists;
    /// コンパイル時に畳み込んだ文字列（SStr*）。frontend が JitPinned へ
    /// 移して GC から守る（実体は pass が xyo_join で作る）。
    std::vector<SStr*> pinnedStrings;
    /// 常に数値（素の double）を持つと証明できた変数/リスト。
    /// 変数読み・リスト読みの toNumber からタグ検査を省くために使う。
    std::unordered_set<std::string> numericVars;
    std::unordered_set<std::string> numericLists;
    /// true: この Module が単一スポーンのハット（green flag）から来た。
    /// 同一スレッド逐次実行が保証されるため、入口ガード版の解析では
    /// 同一関数内の外書きを入口ガードのカバー範囲とみなせる。
    /// broadcast/clone/key 等（多重起動し得る）は false のまま。
    bool singleSpawn = false;

    /// MLIR generic syntax 風のテキスト。
    std::string print() const;

private:
    std::deque<std::unique_ptr<Op>> ops_;
    std::deque<Function> functions_;
    int64_t nextDebugId_ = 1;
};

// ============================================================= Builder ====

class Builder {
public:
    explicit Builder(Module& m) : m_(m) {}

    void setBlock(Block* b) { b_ = b; }
    Block* block() const { return b_; }

    Op* create(std::string name, std::vector<Value> operands,
               std::vector<Type> resultTypes, AttrMap attrs = {});
    Op* createTerminator(std::string name, std::vector<Value> operands,
                         AttrMap attrs = {});

    /// 現在の block の末尾に、結果 1 個の op を作る。
    Value unary(std::string name, Value a, Type result,
                AttrMap attrs = {});
    Value binary(std::string name, Value a, Value b, Type result,
                 AttrMap attrs = {});

    // --- 定数 ---
    Value cst(uint64_t bits);              // !xyo.value（NaN-boxed）
    Value cstNum(double d);                // 数値の boxed 定数
    Value cstF64(double d);                // !xyo.f64

    // --- 変数 ---
    Value getVar(const std::string& id, const std::string& name);
    void setVar(const std::string& id, const std::string& name, Value v);

    // --- 算術 ---
    Value add(Value a, Value b) { return binary("xyo.add", a, b, Type::value()); }
    Value sub(Value a, Value b) { return binary("xyo.sub", a, b, Type::value()); }
    Value mul(Value a, Value b) { return binary("xyo.mul", a, b, Type::value()); }
    Value div(Value a, Value b) { return binary("xyo.div", a, b, Type::value()); }
    Value mod(Value a, Value b) { return binary("xyo.mod", a, b, Type::value()); }
    Value lt(Value a, Value b) { return binary("xyo.lt", a, b, Type::boolean()); }
    Value gt(Value a, Value b) { return binary("xyo.gt", a, b, Type::boolean()); }
    Value eq(Value a, Value b) { return binary("xyo.eq", a, b, Type::boolean()); }
    Value truthy(Value a) {
        // boolean への truthy は恒等写像（Scratch の真偽値は
        // そのまま真偽）。ランタイム呼び出しを出さない。
        if (a.type == Type::boolean()) return a;
        // boolval で箱詰めした直後なら開けて返す（box/unbox の往復）。
        if (a.op && a.op->name == "xyo.boolval" &&
            !a.op->operands.empty())
            return a.op->operands[0];
        return unary("xyo.truthy", a, Type::boolean());
    }
    Value toNum(Value a) { return unary("xyo.to_num", a, Type::f64()); }

private:
    Module& m_;
    Block* b_ = nullptr;
};

// =========================================================== verifier ====

struct Diagnostic {
    std::string message;
    Op* op = nullptr;
};

/// SSA の定義順・region の入れ子・終端命令・型の整合を検査する。
/// `canonicalize` などの破壊的 pass の前後で使う。
bool verify(Module& m, std::vector<Diagnostic>* errors = nullptr);

// ============================================================== passes ====

struct PassStats {
    uint64_t folded = 0;
    uint64_t cse = 0;
    uint64_t dce = 0;
    uint64_t promotedVars = 0;
    uint64_t promotedLoops = 0;
    uint64_t guardedLoops = 0;  // 入口ガード付き複製ループ数
    uint64_t intLoops = 0;      // 整数昇格したループ数
};

/// 定数ビットが i32 範囲の整数値を表すか。素の double はそのまま、
/// 文字列は数値として整数かつエンジンの印字と一致（正準形）のときだけ
/// 真（"1" は可、"1.0"/" 1"/"007" は不可。文字列読みでの観測を変えない
/// ため）。整数プロモーションの入口・書き込み判定が共有する。
bool intLiteralBits(uint64_t bits, int64_t* out);

/// 定数畳み込み・自明な簡約（`truthy(const)` 等）。
void canonicalize(Module& m, PassStats* stats = nullptr);
/// block 内 CSE（副作用 op を境界に表をクリアする）。
void cse(Module& m, PassStats* stats = nullptr);
/// 未使用の pure op を除去する。
void dce(Module& m, PassStats* stats = nullptr);

/// `xyo.repeat` 本体の直鎖で、数値としてしか触られない変数を f64 の
/// loop-carried SSA に昇格する。yield の前後では実メモリへ spill/reload
/// するため、他のスクリプト・モニタから見える値は逐次実行と一致する。
void promoteNumericLoopVars(Module& m, PassStats* stats = nullptr);

/// canonicalize → cse → dce → numeric promotion → canonicalize の
/// 標準パイプライン。
void runStandardPipeline(Module& m, PassStats* stats = nullptr);

} // namespace xyo::mlir
