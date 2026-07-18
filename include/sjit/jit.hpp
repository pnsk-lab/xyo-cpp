#ifndef SJIT_JIT_HPP
#define SJIT_JIT_HPP

#include "sjit/abi.hpp"

#include <memory>
#include <string>

namespace sjit {

using SRuntimeTickFn = SRuntimeStatus (*)(SRuntime *runtime);
using SRuntimeVoidFn = void (*)(SRuntime *runtime);
using SRuntimeThreadQueryFn = int (*)(const SRuntime *runtime);
using SRuntimePenPathDataFn = const SDrawCommand *(*)(const SRuntime *runtime);

class JitEngine {
public:
    JitEngine();
    ~JitEngine();

    JitEngine(const JitEngine &) = delete;
    JitEngine &operator=(const JitEngine &) = delete;

    /* The returned entry borrows script and its complete AST.  They must stay
       at stable addresses and outlive every possible invocation of the entry. */
    SScriptEntryFn compileScript(
        const SCompiledScript &script,
        const std::string &name,
        SRuntime *runtime = nullptr);
    /* Attach parallel ownership metadata only when this engine's private
       registry binds entry and every native procedure to this exact AST. */
    bool certifyScriptOwnership(
        SRuntime *runtime,
        int scriptId,
        const SCompiledScript &script,
        SScriptEntryFn entry) const;
    /* Empty after a native compilation.  When compileScript returns the
       interpreter entry, this describes the opcode which made native lowering
       ineligible. */
    const std::string &lastFallbackReason() const;
    bool hasRuntimeBitcode() const;
    SRuntimeTickFn runtimeTick();
    SRuntimeVoidFn runtimeGreenFlag();
    SRuntimeVoidFn runtimeStopAll();
    SRuntimeThreadQueryFn runtimeHasThreads();
    SRuntimeThreadQueryFn runtimeThreadCount();
    SRuntimePenPathDataFn runtimePenPathData();
    SRuntimeThreadQueryFn runtimePenPathCount();
    SRuntimeThreadQueryFn runtimePenPathRevision();
    void emitScriptLl(const SCompiledScript &script, const std::string &name, const std::string &path);
    void emitRuntimeLl(const std::string &path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

SValue runSmokeJit();
void emitSmokeLl(const std::string &path);
void emitRuntimeLl(const std::string &path);

}

#endif
