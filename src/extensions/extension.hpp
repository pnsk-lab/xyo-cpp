// xyo-cpp: 自作拡張の共通型とレジストリ（Phase 0）。
//
// 設計は docs/extensions.md。ここでは最小限の「ブロック定義 + 実装」を
// 実行時レジストリに載せ、Runtime のプリミティブ表へ結び付ける。
// マニフェスト JSON / 動的ロード / WASM は後続フェーズ。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/value.hpp"
#include "../model/blocks.hpp"

namespace xyo {

struct BlockUtility;
struct Runtime;

/// 1 引数の宣言（マニフェストの args[]）。
struct ExtensionArgDef {
    std::string name;
    std::string type;       // number | string | boolean | angle | dropdown
    std::string dflt;
    std::vector<std::string> options;   // dropdown: label, value, label, value...
};

/// 1 ブロックの宣言。エディタ用のラベルや引数は後続フェーズで拡張する。
struct ExtensionBlockDef {
    std::string opcode;
    std::string label;
    bool hat = false;
    bool edgeActivated = false;
    bool restartExistingThreads = false;
    bool conditional = false;
    bool loop = false;
    /// stack | reporter | boolean | hat | end。空なら hat から推定。
    std::string shape;
    std::vector<ExtensionArgDef> args;
};

/// 1 拡張の宣言。id は opcode の prefix（`myext_*`）と一致させる。
struct ExtensionDef {
    std::string id;
    std::string name;
    uint32_t color = 0;
    /// "manifest" (no-op stub) / "wasm" / ""（未指定 = manifest）。
    std::string provider;
    /// マニフェストのあるディレクトリ（WASM の相対パス解決用）。
    std::string sourceDir;
    std::vector<ExtensionBlockDef> blocks;
};

/// 拡張の実装。プリミティブはこのインスタンスへ `util.primitiveContext`
/// 経由で到達する。`call` はインタプリタ / JIT compat / AOT の共通経路。
class ExtensionInstance {
public:
    virtual ~ExtensionInstance() = default;
    virtual const ExtensionDef& def() const = 0;
    virtual void onLoad(Runtime&) {}
    virtual void onUnload() {}
    virtual Value call(const std::string& opcode, const ArgValues& args,
                       BlockUtility& util) = 0;
};

/// 全拡張ブロックが共有する PrimFn。`util.op` の opcode と
/// `util.primitiveContext` の ExtensionInstance を結び付ける。
Value extensionPrimitive(const ArgValues& args, BlockUtility& util);

/// opcode → ExtensionInstance のレジストリ。Runtime より長生きさせること。
class ExtensionRegistry {
public:
    /// `rt` にプリミティブ・ハット・flowing を登録する。失敗時は何も登録
    /// しない（検証は全ブロックを見てからコミットする）。
    bool add(Runtime& rt, std::unique_ptr<ExtensionInstance> ext,
             std::string* error = nullptr);

    ExtensionInstance* find(const std::string& id) const;
    PrimFn primitiveFor(const std::string& opcode) const;
    void* contextFor(const std::string& opcode) const;

    const std::vector<ExtensionDef>& definitions() const { return definitions_; }
    std::size_t size() const { return instances_.size(); }

private:
    std::vector<std::unique_ptr<ExtensionInstance>> instances_;
    std::vector<ExtensionDef> definitions_;
    std::unordered_map<std::string, ExtensionInstance*> byId_;
    std::unordered_map<std::string, ExtensionInstance*> byOpcode_;
};

} // namespace xyo
