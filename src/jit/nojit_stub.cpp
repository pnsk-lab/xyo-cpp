// xyo-cpp: XYO_JIT=OFF ビルド用スタブ。
// JitEngine の空実装と C ABI ヘルパの未使用スタブを提供し、
// jit 非依存コード側（runtime/thread/sequencer）に ifdef を散らさない。
// compile は常に失敗扱い（nullptr）なので生成コード経路には到達しない。
#include "jit.hpp"

namespace xyo {

// Impl は jit.cpp の実体を必要としない — ここでは空定義で十分
struct JitEngine::Impl {};

JitEngine::JitEngine() : impl_(std::make_unique<Impl>()) {}
JitEngine::~JitEngine() = default;

const CompiledScript* JitEngine::compile(Thread&, Runtime&, Sequencer&,
                                       const Block*) {
    return nullptr;
}
bool JitEngine::decideCompile(Thread&, const Block*) {
    return false;
}
void JitEngine::noteInterpBlocks(const void*, uint64_t) {}
bool JitEngine::scriptStats(const void*, ScriptStats*) const { return false; }
uint8_t JitEngine::scanScriptLoop(Blocks&, const Block*, uint32_t*) {
    return 1;
}
const AccessSet* JitEngine::accessFor(const Thread&, Runtime&) {
    return nullptr;
}
void JitEngine::invalidate() {}
bool JitEngine::ok() const { return false; }

} // namespace xyo

// 生成コードからのみ呼ばれる C ABI — no-JIT では到達しないが
// 参照が残る箇所（compat 層等）のためにシンボルだけ提供する。
extern "C" void xyo_yield(xyo::JitCtx*) {}
extern "C" xyo::Variable* xyo_jitVar(xyo::JitCtx*, const char*,
                                   const char16_t*, int) {
    return nullptr;
}
extern "C" void xyo_varslot_init(xyo::JitCtx*, uint64_t, const char*,
                                 const char16_t*, int32_t) {}
