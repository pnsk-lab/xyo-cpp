#include "sjit/jit.hpp"

#include <stdexcept>
#include <string>

namespace sjit {

struct JitEngine::Impl {
    std::string fallback_reason;
};

JitEngine::JitEngine() : impl_(std::make_unique<Impl>()) {
    impl_->fallback_reason = "native LLVM JIT is unavailable in the Web build";
}

JitEngine::~JitEngine() = default;

SScriptEntryFn JitEngine::compileScript(
    const SCompiledScript &script,
    const std::string &name,
    SRuntime *runtime) {
    (void)script;
    (void)name;
    (void)runtime;
    return sjit_script_interpreter_entry;
}

bool JitEngine::certifyScriptOwnership(
    SRuntime *runtime,
    int scriptId,
    const SCompiledScript &script,
    SScriptEntryFn entry) const {
    (void)runtime;
    (void)scriptId;
    (void)script;
    (void)entry;
    return false;
}

const std::string &JitEngine::lastFallbackReason() const {
    return impl_->fallback_reason;
}

bool JitEngine::hasRuntimeBitcode() const {
    return false;
}

SRuntimeTickFn JitEngine::runtimeTick() {
    return nullptr;
}

SRuntimeVoidFn JitEngine::runtimeGreenFlag() {
    return nullptr;
}

SRuntimeVoidFn JitEngine::runtimeStopAll() {
    return nullptr;
}

SRuntimeThreadQueryFn JitEngine::runtimeHasThreads() {
    return nullptr;
}

SRuntimeThreadQueryFn JitEngine::runtimeThreadCount() {
    return nullptr;
}

SRuntimePenPathDataFn JitEngine::runtimePenPathData() {
    return nullptr;
}

SRuntimeThreadQueryFn JitEngine::runtimePenPathCount() {
    return nullptr;
}

SRuntimeThreadQueryFn JitEngine::runtimePenPathRevision() {
    return nullptr;
}

void JitEngine::emitScriptLl(
    const SCompiledScript &script,
    const std::string &name,
    const std::string &path) {
    (void)script;
    (void)name;
    (void)path;
    throw std::runtime_error("LLVM IR emission is unavailable in the Web build");
}

void JitEngine::emitRuntimeLl(const std::string &path) {
    (void)path;
    throw std::runtime_error("LLVM IR emission is unavailable in the Web build");
}

SValue runSmokeJit() {
    return sjit_make_number(7.0);
}

void emitSmokeLl(const std::string &path) {
    (void)path;
    throw std::runtime_error("LLVM IR emission is unavailable in the Web build");
}

void emitRuntimeLl(const std::string &path) {
    (void)path;
    throw std::runtime_error("LLVM IR emission is unavailable in the Web build");
}

} // namespace sjit
