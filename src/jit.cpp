#include "sjit/jit.hpp"
#include "sjit_opcode_effects.h"
#include "sjit_operator.h"
#include "sjit_ownership_internal.h"
#include "sjit_string.h"
#include "sjit_thread_pool.h"

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace {

std::string takeError(llvm::Error error) {
    std::string message;
    llvm::handleAllErrors(std::move(error), [&](const llvm::ErrorInfoBase &info) {
        if (!message.empty()) {
            message += "; ";
        }
        message += info.message();
    });
    return message;
}

template <class T>
T unwrapExpected(llvm::Expected<T> expected, const char *context) {
    if (!expected) {
        throw std::runtime_error(std::string(context) + ": " + takeError(expected.takeError()));
    }
    return std::move(*expected);
}

void checkError(llvm::Error error, const char *context) {
    if (error) {
        throw std::runtime_error(std::string(context) + ": " + takeError(std::move(error)));
    }
}

std::unique_ptr<llvm::orc::LLJIT> createOptimizedLlJit(const char *context) {
    auto target = unwrapExpected(llvm::orc::JITTargetMachineBuilder::detectHost(), "detect host target");
    target.setCodeGenOptLevel(llvm::CodeGenOptLevel::Aggressive);
    return unwrapExpected(
        llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(target)).create(),
        context);
}

struct SmokeModule {
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
};

struct ModuleBuild {
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
};

SmokeModule createSmokeModule(const llvm::DataLayout &dataLayout) {
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>("sjit_smoke_module", *context);
    module->setDataLayout(dataLayout);

    auto *i32 = llvm::Type::getInt32Ty(*context);
    auto *f64 = llvm::Type::getDoubleTy(*context);
    auto *ptr = llvm::PointerType::getUnqual(*context);
    auto *svalue = llvm::StructType::create(*context, {i32, f64, ptr}, "SValue");

    auto *makeNumberTy = llvm::FunctionType::get(svalue, {f64}, false);
    auto *makeNumber = llvm::Function::Create(
        makeNumberTy,
        llvm::Function::ExternalLinkage,
        "sjit_make_number",
        module.get());

    auto *fnTy = llvm::FunctionType::get(svalue, {}, false);
    auto *fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, "sjit_jit_smoke", module.get());
    auto *entry = llvm::BasicBlock::Create(*context, "entry", fn);
    llvm::IRBuilder<> builder(entry);
    auto *seven = llvm::ConstantFP::get(f64, 7.0);
    builder.CreateRet(builder.CreateCall(makeNumber, {seven}));

    return {std::move(context), std::move(module)};
}

void verifyModuleOrThrow(const llvm::Module &module, const char *context) {
    std::string message;
    llvm::raw_string_ostream stream(message);
    if (llvm::verifyModule(module, &stream)) {
        throw std::runtime_error(std::string(context) + ": " + stream.str());
    }
}

void eraseUnusedInternalFunctions(llvm::Module &module) {
    bool changed = false;
    do {
        changed = false;
        for (auto function = module.begin(); function != module.end();) {
            llvm::Function *candidate = &*function++;
            if (!candidate->hasInternalLinkage() || !candidate->use_empty()) {
                continue;
            }
            candidate->eraseFromParent();
            changed = true;
        }
    } while (changed);
}

size_t moduleInstructionCount(const llvm::Module &module) {
    size_t count = 0;
    for (const llvm::Function &function : module) {
        for (const llvm::BasicBlock &block : function) {
            count += block.size();
        }
    }
    return count;
}

llvm::OptimizationLevel configuredJitOptimizationLevel(const llvm::Module &module) {
    const char *level = std::getenv("SJIT_JIT_OPT_LEVEL");
    if (level && level[0] != '\0') {
        if (std::strcmp(level, "0") == 0) {
            return llvm::OptimizationLevel::O0;
        }
        if (std::strcmp(level, "1") == 0) {
            return llvm::OptimizationLevel::O1;
        }
        if (std::strcmp(level, "2") == 0) {
            return llvm::OptimizationLevel::O2;
        }
        if (std::strcmp(level, "3") == 0) {
            return llvm::OptimizationLevel::O3;
        }
    }
    /* O3 duplicates enough control flow in very large generated renderers to
       hurt both compilation latency and instruction-cache locality.  O1 still
       performs the SSA promotion and local simplification these modules need,
       while small scripts retain the more profitable O3 pipeline. */
    constexpr size_t largeModuleInstructionThreshold = 10000;
    if (moduleInstructionCount(module) > largeModuleInstructionThreshold) {
        return llvm::OptimizationLevel::O1;
    }
    return llvm::OptimizationLevel::O3;
}

void optimizeModule(llvm::Module &module) {
    llvm::LoopAnalysisManager loopAnalysis;
    llvm::FunctionAnalysisManager functionAnalysis;
    llvm::CGSCCAnalysisManager cgsccAnalysis;
    llvm::ModuleAnalysisManager moduleAnalysis;
    llvm::PassBuilder passBuilder;

    passBuilder.registerModuleAnalyses(moduleAnalysis);
    passBuilder.registerCGSCCAnalyses(cgsccAnalysis);
    passBuilder.registerFunctionAnalyses(functionAnalysis);
    passBuilder.registerLoopAnalyses(loopAnalysis);
    passBuilder.crossRegisterProxies(loopAnalysis, functionAnalysis, cgsccAnalysis, moduleAnalysis);

    llvm::ModulePassManager passManager =
        passBuilder.buildPerModuleDefaultPipeline(configuredJitOptimizationLevel(module));
    passManager.run(module, moduleAnalysis);
}

void eliminateUnreachableInternalGlobals(llvm::Module &module) {
    llvm::LoopAnalysisManager loopAnalysis;
    llvm::FunctionAnalysisManager functionAnalysis;
    llvm::CGSCCAnalysisManager cgsccAnalysis;
    llvm::ModuleAnalysisManager moduleAnalysis;
    llvm::PassBuilder passBuilder;

    passBuilder.registerModuleAnalyses(moduleAnalysis);
    passBuilder.registerCGSCCAnalyses(cgsccAnalysis);
    passBuilder.registerFunctionAnalyses(functionAnalysis);
    passBuilder.registerLoopAnalyses(loopAnalysis);
    passBuilder.crossRegisterProxies(loopAnalysis, functionAnalysis, cgsccAnalysis, moduleAnalysis);

    llvm::ModulePassManager passManager;
    passManager.addPass(llvm::GlobalDCEPass());
    passManager.run(module, moduleAnalysis);
}

std::string runtimeBitcodePath() {
    const char *overridePath = std::getenv("SJIT_RUNTIME_BC");
    if (overridePath && overridePath[0] != '\0') {
        return overridePath;
    }
    static int executableAnchor = 0;
    const std::string executable = llvm::sys::fs::getMainExecutable(
        nullptr,
        &executableAnchor);
    const size_t separator = executable.find_last_of("/\\");
    if (separator != std::string::npos) {
        const std::string directory = executable.substr(0, separator);
        const std::string sibling = directory + "/sjit_runtime.bc";
        if (llvm::sys::fs::exists(sibling)) {
            return sibling;
        }
        const size_t parentSeparator = directory.find_last_of("/\\");
        if (parentSeparator != std::string::npos) {
            const std::string parent =
                directory.substr(0, parentSeparator) + "/sjit_runtime.bc";
            if (llvm::sys::fs::exists(parent)) {
                return parent;
            }
        }
    }
    return "build/sjit_runtime.bc";
}

std::string runtimeObjectPath() {
    const char *overridePath = std::getenv("SJIT_RUNTIME_OBJECT");
    if (overridePath && overridePath[0] != '\0') {
        return overridePath;
    }
    std::string path = runtimeBitcodePath();
    if (path.size() >= 3 && path.compare(path.size() - 3, 3, ".bc") == 0) {
        path.replace(path.size() - 3, 3, ".o");
    } else {
        path += ".o";
    }
    return path;
}

std::unique_ptr<llvm::Module> loadRuntimeBitcodeModule(
    llvm::LLVMContext &context,
    const llvm::DataLayout &dataLayout,
    bool missingIsError) {
    const std::string path = runtimeBitcodePath();
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if (!buffer) {
        if (missingIsError) {
            throw std::runtime_error("open runtime bitcode " + path + ": " + buffer.getError().message());
        }
        return nullptr;
    }

    auto module = unwrapExpected(
        llvm::parseBitcodeFile((*buffer)->getMemBufferRef(), context),
        "parse runtime bitcode");
    module->setDataLayout(dataLayout);
    verifyModuleOrThrow(*module, "verify runtime bitcode");
    return module;
}

bool linkRuntimeHelpersIntoScript(
    llvm::Module &scriptModule,
    const llvm::DataLayout &dataLayout) {
    if (std::getenv("SJIT_DISABLE_SCRIPT_RUNTIME_LINK") != nullptr) {
        return false;
    }
    std::unique_ptr<llvm::Module> runtimeModule = loadRuntimeBitcodeModule(
        scriptModule.getContext(),
        dataLayout,
        false);
    if (!runtimeModule) {
        return false;
    }

    /* Import a deliberately small, reviewed leaf subset.  Boxed value,
       string/list, lookup, and interpreter helpers remain calls into the
       shared runtime: importing their transitive graphs makes per-script O3
       and code size substantially worse.  The selected bodies still expose
       numeric/frame fast paths and their compiler-inferred attributes to the
       script optimizer. */
    static const std::unordered_set<std::string> importableRuntimeLeaves = {
        "sjit_runtime_get_sprite",
        "sjit_jit_thread_script_data",
        "sjit_jit_frame_pc",
        "sjit_jit_frame_set_pc",
        "sjit_jit_frame_mark_finished",
        "sjit_jit_sprite_x",
        "sjit_jit_sprite_y",
        "sjit_jit_pen_set_size_number",
        "sjit_jit_sprite_set_size",
        "sjit_jit_variable_is_number",
        "sjit_jit_variable_number",
        "sjit_jit_variable_set_number",
        "sjit_jit_variable_change_by_number",
        "sjit_jit_list_variable_length_number",
        "sjit_jit_round_number",
        "sjit_jit_random_number",
        "sjit_jit_value_make_number",
        "sjit_jit_value_make_bool",
        "sjit_jit_control_loop_state",
        "sjit_jit_control_loop_reset",
        "sjit_jit_procedure_control_loop_state",
        "sjit_jit_procedure_control_loop_reset",
        "sjit_jit_repeat_remaining_number",
        "sjit_jit_repeat_take_all_number",
        "sjit_jit_round_repeat_count",
        "sjit_jit_finish_control_branch",
        "sjit_jit_finish_batched_loop_branch",
    };
    for (llvm::Function &function : *runtimeModule) {
        if (!function.isDeclaration() &&
            !function.hasLocalLinkage() &&
            importableRuntimeLeaves.count(function.getName().str()) == 0) {
            function.deleteBody();
            function.setLinkage(llvm::GlobalValue::ExternalLinkage);
            function.setComdat(nullptr);
        }
    }

    std::vector<std::pair<std::string, llvm::FunctionType *>> scriptDeclarations;
    std::unordered_set<std::string> scriptDefinitionNames;
    for (llvm::Function &function : scriptModule) {
        if (function.isDeclaration() && !function.isIntrinsic()) {
            scriptDeclarations.emplace_back(
                function.getName().str(),
                function.getFunctionType());
        } else if (!function.isDeclaration()) {
            scriptDefinitionNames.insert(function.getName().str());
        }
    }
    std::unordered_set<std::string> scriptGlobalDefinitionNames;
    for (llvm::GlobalVariable &global : scriptModule.globals()) {
        if (!global.isDeclaration()) {
            scriptGlobalDefinitionNames.insert(global.getName().str());
        }
    }

    /* Link while runtime definitions still have external linkage so they
       resolve the declarations emitted by the script module.  Internalizing
       first makes LLVM rename the definition instead of satisfying the
       declaration, silently defeating cross-module optimization. */
    if (llvm::Linker::linkModules(
            scriptModule,
            std::move(runtimeModule),
            llvm::Linker::Flags::LinkOnlyNeeded)) {
        throw std::runtime_error("link runtime bitcode into script module");
    }

    /* Each script owns private copies of the runtime definitions pulled in by
       LinkOnlyNeeded.  Preserve only definitions that originated in the
       generated script itself; everything else can be inlined and DCE'd
       without exporting duplicate symbols into the ORC dylib. */
    for (llvm::Function &function : scriptModule) {
        if (!function.isDeclaration() && !function.isIntrinsic() &&
            scriptDefinitionNames.count(function.getName().str()) == 0) {
            function.setLinkage(llvm::GlobalValue::InternalLinkage);
            function.setComdat(nullptr);
        }
    }
    for (llvm::GlobalVariable &global : scriptModule.globals()) {
        if (!global.isDeclaration() && !global.hasAppendingLinkage() &&
            scriptGlobalDefinitionNames.count(global.getName().str()) == 0) {
            global.setLinkage(llvm::GlobalValue::InternalLinkage);
            global.setComdat(nullptr);
        }
    }
    for (llvm::GlobalAlias &alias : scriptModule.aliases()) {
        alias.setLinkage(llvm::GlobalValue::InternalLinkage);
    }
    for (llvm::GlobalIFunc &ifunc : scriptModule.ifuncs()) {
        ifunc.setLinkage(llvm::GlobalValue::InternalLinkage);
    }
    eliminateUnreachableInternalGlobals(scriptModule);
    /* Keep the script module's original ABI surface stable for textual IR
       diagnostics.  GlobalDCE removes an unused declaration when the linked
       definition was internalized, so recreate only declarations which were
       already present before linking. */
    for (const auto &[name, type] : scriptDeclarations) {
        if (!scriptModule.getFunction(name)) {
            llvm::Function::Create(
                type,
                llvm::Function::ExternalLinkage,
                name,
                &scriptModule);
        }
    }
    verifyModuleOrThrow(scriptModule, "verify script/runtime linked module");
    return true;
}

bool addRuntimeObjectIfAvailable(llvm::orc::LLJIT &jit) {
    if (std::getenv("SJIT_DISABLE_RUNTIME_OBJECT") != nullptr) {
        return false;
    }
    auto buffer = llvm::MemoryBuffer::getFile(runtimeObjectPath());
    if (!buffer) {
        return false;
    }
    checkError(jit.addObjectFile(std::move(*buffer)), "add precompiled runtime object");
    return true;
}

void defineHostThreadPoolSymbols(llvm::orc::LLJIT &jit) {
    llvm::orc::MangleAndInterner mangle(
        jit.getExecutionSession(), jit.getDataLayout());
    checkError(
        jit.getMainJITDylib().define(llvm::orc::absoluteSymbols({
            {mangle("sjit_thread_pool_create"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_thread_pool_create),
              llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_thread_pool_destroy"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_thread_pool_destroy),
              llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_thread_pool_parallelism"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_thread_pool_parallelism),
              llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_thread_pool_run"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_thread_pool_run),
              llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_thread_pool_environment_parallelism"),
             {llvm::orc::ExecutorAddr::fromPtr(
                  &sjit_thread_pool_environment_parallelism),
              llvm::JITSymbolFlags::Exported}},
        })),
        "define host thread-pool symbols");
}

bool addRuntimeCodeIfAvailable(llvm::orc::LLJIT &jit) {
    if (addRuntimeObjectIfAvailable(jit)) {
        return true;
    }
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = loadRuntimeBitcodeModule(*context, jit.getDataLayout(), false);
    if (!module) {
        return false;
    }
    if (std::getenv("SJIT_ENABLE_EAGER_RUNTIME_OPT") != nullptr) {
        optimizeModule(*module);
    }
    verifyModuleOrThrow(*module, "verify optimized runtime bitcode");

    checkError(
        jit.addIRModule(llvm::orc::ThreadSafeModule(std::move(module), std::move(context))),
        "add runtime bitcode");
    return true;
}

llvm::Value *isNull(llvm::IRBuilder<> &builder, llvm::Value *pointer) {
    return builder.CreateICmpEQ(pointer, llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(pointer->getType())));
}

bool parseFiniteNumberLiteral(const char *text, double &out) {
    if (!text) {
        out = 0.0;
        return true;
    }
    errno = 0;
    char *end = nullptr;
    const double parsed = std::strtod(text, &end);
    if (end == text) {
        return false;
    }
    while (end && *end) {
        const unsigned char ch = static_cast<unsigned char>(*end);
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\f' && ch != '\v') {
            return false;
        }
        ++end;
    }
    if (errno == ERANGE || !std::isfinite(parsed)) {
        return false;
    }
    out = parsed;
    return true;
}

bool literalNumber(const SExpr *expr, double &out) {
    if (!expr || expr->opcode != SJIT_EXPR_LITERAL) {
        return false;
    }
    switch (expr->literal.tag) {
    case SJIT_VALUE_NUMBER:
        out = std::isnan(expr->literal.number) ? 0.0 : expr->literal.number;
        return true;
    case SJIT_VALUE_BOOL:
        out = expr->literal.number != 0.0 ? 1.0 : 0.0;
        return true;
    case SJIT_VALUE_NULL:
        out = 0.0;
        return true;
    case SJIT_VALUE_STRING:
        return parseFiniteNumberLiteral(sjit_string_cstr(static_cast<const SString *>(expr->literal.ptr)), out);
    default:
        return false;
    }
}

bool literalBool(const SExpr *expr, bool &out) {
    if (!expr || expr->opcode != SJIT_EXPR_LITERAL) {
        return false;
    }
    switch (expr->literal.tag) {
    case SJIT_VALUE_NUMBER:
    case SJIT_VALUE_BOOL:
        out = expr->literal.number != 0.0;
        return true;
    case SJIT_VALUE_NULL:
        out = false;
        return true;
    case SJIT_VALUE_STRING: {
        const char *text = sjit_string_cstr(static_cast<const SString *>(expr->literal.ptr));
        out = text && text[0] != '\0' &&
            !sjit_cstr_equals_ignore_case(text, "0") &&
            !sjit_cstr_equals_ignore_case(text, "false");
        return true;
    }
    case SJIT_VALUE_LIST:
        out = true;
        return true;
    default:
        return false;
    }
}

bool literalStringLength(const SExpr *expr, int &out) {
    if (!expr || expr->opcode != SJIT_EXPR_LITERAL || expr->literal.tag != SJIT_VALUE_STRING) {
        return false;
    }
    const SString *string = static_cast<const SString *>(expr->literal.ptr);
    out = string ? string->length : 0;
    return true;
}

int literalKeyIndex(const SExpr *expr) {
    if (!expr || expr->opcode != SJIT_EXPR_LITERAL || expr->literal.tag != SJIT_VALUE_STRING) {
        return -1;
    }
    const char *name = sjit_string_cstr(static_cast<const SString *>(expr->literal.ptr));
    if (!name || name[0] == '\0') {
        return -1;
    }
    if (sjit_cstr_equals_ignore_case(name, "up arrow")) {
        return SJIT_KEY_UP_ARROW;
    }
    if (sjit_cstr_equals_ignore_case(name, "down arrow")) {
        return SJIT_KEY_DOWN_ARROW;
    }
    if (sjit_cstr_equals_ignore_case(name, "right arrow")) {
        return SJIT_KEY_RIGHT_ARROW;
    }
    if (sjit_cstr_equals_ignore_case(name, "left arrow")) {
        return SJIT_KEY_LEFT_ARROW;
    }
    if (sjit_cstr_equals_ignore_case(name, "space")) {
        return ' ';
    }
    if (name[1] == '\0') {
        return static_cast<unsigned char>(name[0]);
    }
    return -1;
}

const SExpr *statementExprForSlot(const SStatement &statement, int slot) {
    switch (slot) {
    case SJIT_STMT_EXPR_VALUE:
        return statement.value;
    case SJIT_STMT_EXPR_INDEX:
        return statement.index;
    case SJIT_STMT_EXPR_CONDITION:
        return statement.condition;
    case SJIT_STMT_EXPR_TIMES:
        return statement.times;
    default:
        return nullptr;
    }
}

enum class JitNodeKind {
    Statement,
    RepeatEntry,
    RepeatContinue,
    RepeatAfter,
    RepeatUntilEntry,
    RepeatUntilContinue,
    RepeatUntilAfter,
    WhileEntry,
    WhileContinue,
    WhileAfter,
    ForeverEntry,
    ForeverContinue,
    ForeverAfter,
    ForEachEntry,
    ForEachContinue,
    ForEachAfter,
    IfEntry,
    IfAfter,
    IfElseEntry,
    IfElseAfter,
};

struct JitStatementNode {
    JitNodeKind kind = JitNodeKind::Statement;
    const SStatement *statement = nullptr;
    int pc = -1;
    int nextPc = -1;
    int bodyPc = -1;
    int elsePc = -1;
    int continuePc = -1;
    int controlPc = -1;
    int branchCounter = -1;
};

int findProcedureIndex(const SCompiledScript &script, const SString *name) {
    const char *procedureName = sjit_string_cstr(name);
    if (!procedureName) {
        return -1;
    }
    for (int i = 0; i < script.procedure_count; ++i) {
        if (std::strcmp(sjit_string_cstr(script.procedures[i].name), procedureName) == 0) {
            return i;
        }
    }
    return -1;
}

int procedureArgumentIndex(const SCompiledProcedure &procedure, const SExpr *expr) {
    if (!expr || expr->opcode != SJIT_EXPR_ARGUMENT || expr->literal.tag != SJIT_VALUE_STRING) {
        return -1;
    }
    const char *name = sjit_string_cstr(static_cast<const SString *>(expr->literal.ptr));
    if (!name) {
        return -1;
    }
    for (int i = 0; i < procedure.argument_count; ++i) {
        if (std::strcmp(sjit_string_cstr(procedure.argument_names[i]), name) == 0) {
            return i;
        }
    }
    return -1;
}

enum class ProcedureArgumentUseMode {
    Number,
    Value,
    Boolean,
};

void markProcedureArgumentUsesInExpr(
    const SCompiledProcedure &procedure,
    const SExpr *expr,
    ProcedureArgumentUseMode mode,
    std::vector<bool> &needsValue) {
    if (!expr) {
        return;
    }
    if (expr->opcode == SJIT_EXPR_ARGUMENT) {
        const int index = procedureArgumentIndex(procedure, expr);
        if (index >= 0 && mode != ProcedureArgumentUseMode::Number) {
            needsValue[static_cast<size_t>(index)] = true;
        }
        return;
    }
    switch (expr->opcode) {
    case SJIT_EXPR_ADD:
    case SJIT_EXPR_SUB:
    case SJIT_EXPR_MUL:
    case SJIT_EXPR_DIV:
    case SJIT_EXPR_MOD:
    case SJIT_EXPR_RANDOM:
        markProcedureArgumentUsesInExpr(procedure, expr->left, ProcedureArgumentUseMode::Number, needsValue);
        markProcedureArgumentUsesInExpr(procedure, expr->right, ProcedureArgumentUseMode::Number, needsValue);
        return;
    case SJIT_EXPR_MATHOP:
    case SJIT_EXPR_ROUND:
        markProcedureArgumentUsesInExpr(procedure, expr->left, ProcedureArgumentUseMode::Number, needsValue);
        return;
    case SJIT_EXPR_LT:
    case SJIT_EXPR_EQ:
    case SJIT_EXPR_GT:
    case SJIT_EXPR_JOIN:
    case SJIT_EXPR_LETTER_OF:
    case SJIT_EXPR_CONTAINS:
        markProcedureArgumentUsesInExpr(procedure, expr->left, ProcedureArgumentUseMode::Value, needsValue);
        markProcedureArgumentUsesInExpr(procedure, expr->right, ProcedureArgumentUseMode::Value, needsValue);
        return;
    case SJIT_EXPR_AND:
    case SJIT_EXPR_OR:
        markProcedureArgumentUsesInExpr(procedure, expr->left, ProcedureArgumentUseMode::Boolean, needsValue);
        markProcedureArgumentUsesInExpr(procedure, expr->right, ProcedureArgumentUseMode::Boolean, needsValue);
        return;
    case SJIT_EXPR_NOT:
        markProcedureArgumentUsesInExpr(procedure, expr->left, ProcedureArgumentUseMode::Boolean, needsValue);
        return;
    case SJIT_EXPR_LIST_ITEM:
    case SJIT_EXPR_LIST_ITEM_NUMBER:
    case SJIT_EXPR_LIST_CONTAINS:
    case SJIT_EXPR_LENGTH:
    case SJIT_EXPR_KEY_PRESSED:
        markProcedureArgumentUsesInExpr(procedure, expr->left, ProcedureArgumentUseMode::Value, needsValue);
        return;
    case SJIT_EXPR_LITERAL:
    case SJIT_EXPR_TIMER:
    case SJIT_EXPR_VARIABLE:
    case SJIT_EXPR_LIST_LENGTH:
    case SJIT_EXPR_MOUSE_X:
    case SJIT_EXPR_MOUSE_Y:
    case SJIT_EXPR_MOUSE_DOWN:
    case SJIT_EXPR_DAYS_SINCE_2000:
    case SJIT_EXPR_X_POSITION:
    case SJIT_EXPR_Y_POSITION:
    case SJIT_EXPR_LIST_VARIABLE:
        return;
    default:
        // Unknown reporters are conservatively treated as value consumers.
        markProcedureArgumentUsesInExpr(procedure, expr->left, ProcedureArgumentUseMode::Value, needsValue);
        markProcedureArgumentUsesInExpr(procedure, expr->right, ProcedureArgumentUseMode::Value, needsValue);
        return;
    }
}

void markProcedureArgumentUsesInStatements(
    const SCompiledProcedure &procedure,
    const SStatement *statements,
    int count,
    std::vector<bool> &needsValue) {
    if (!statements || count <= 0) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        const SStatement &statement = statements[i];
        markProcedureArgumentUsesInStatements(
            procedure,
            statement.substack,
            statement.substack_count,
            needsValue);
        markProcedureArgumentUsesInStatements(
            procedure,
            statement.substack2,
            statement.substack2_count,
            needsValue);
        switch (statement.opcode) {
        case SJIT_STMT_CHANGE_VARIABLE:
        case SJIT_STMT_PEN_SET_SIZE:
        case SJIT_STMT_MOTION_SET_X:
        case SJIT_STMT_MOTION_SET_Y:
        case SJIT_STMT_MOTION_CHANGE_X:
        case SJIT_STMT_MOTION_CHANGE_Y:
        case SJIT_STMT_LOOKS_SET_SIZE:
        case SJIT_STMT_LOOKS_SET_EFFECT:
        case SJIT_STMT_LOOKS_CHANGE_EFFECT:
        case SJIT_STMT_WAIT:
            markProcedureArgumentUsesInExpr(
                procedure,
                statement.value,
                ProcedureArgumentUseMode::Number,
                needsValue);
            break;
        case SJIT_STMT_MOTION_GOTO_XY:
            markProcedureArgumentUsesInExpr(
                procedure,
                statement.value,
                ProcedureArgumentUseMode::Number,
                needsValue);
            markProcedureArgumentUsesInExpr(
                procedure,
                statement.index,
                ProcedureArgumentUseMode::Number,
                needsValue);
            break;
        case SJIT_STMT_REPEAT:
        case SJIT_STMT_FOR_EACH:
            markProcedureArgumentUsesInExpr(
                procedure,
                statement.times,
                ProcedureArgumentUseMode::Number,
                needsValue);
            break;
        case SJIT_STMT_IF:
        case SJIT_STMT_IF_ELSE:
        case SJIT_STMT_REPEAT_UNTIL:
        case SJIT_STMT_WHILE:
        case SJIT_STMT_WAIT_UNTIL:
            markProcedureArgumentUsesInExpr(
                procedure,
                statement.condition,
                ProcedureArgumentUseMode::Boolean,
                needsValue);
            break;
        case SJIT_STMT_SET_VARIABLE:
        case SJIT_STMT_LIST_ADD:
        case SJIT_STMT_SAY:
        case SJIT_STMT_PEN_SET_COLOR:
        case SJIT_STMT_BROADCAST:
        case SJIT_STMT_LOOKS_SWITCH_BACKDROP:
            markProcedureArgumentUsesInExpr(
                procedure,
                statement.value,
                ProcedureArgumentUseMode::Value,
                needsValue);
            break;
        case SJIT_STMT_PEN_CHANGE_COLOR_PARAM:
            markProcedureArgumentUsesInExpr(
                procedure,
                statement.index,
                ProcedureArgumentUseMode::Value,
                needsValue);
            markProcedureArgumentUsesInExpr(
                procedure,
                statement.value,
                ProcedureArgumentUseMode::Number,
                needsValue);
            break;
        case SJIT_STMT_LIST_DELETE:
            markProcedureArgumentUsesInExpr(
                procedure,
                statement.index,
                ProcedureArgumentUseMode::Value,
                needsValue);
            break;
        case SJIT_STMT_LIST_INSERT:
        case SJIT_STMT_LIST_REPLACE:
            markProcedureArgumentUsesInExpr(
                procedure,
                statement.index,
                ProcedureArgumentUseMode::Value,
                needsValue);
            markProcedureArgumentUsesInExpr(
                procedure,
                statement.value,
                ProcedureArgumentUseMode::Value,
                needsValue);
            break;
        case SJIT_STMT_LOOKS_SAY_FOR_SECS:
            markProcedureArgumentUsesInExpr(
                procedure,
                statement.value,
                ProcedureArgumentUseMode::Value,
                needsValue);
            markProcedureArgumentUsesInExpr(
                procedure,
                statement.times,
                ProcedureArgumentUseMode::Number,
                needsValue);
            break;
        case SJIT_STMT_PROCEDURE_CALL:
            // Forwarded values are kept generic. A later fixed-point pass can
            // safely relax this using the callee's argument requirements.
            for (int argument = 0; argument < statement.argument_count; ++argument) {
                markProcedureArgumentUsesInExpr(
                    procedure,
                    statement.arguments ? statement.arguments[argument].value : nullptr,
                    ProcedureArgumentUseMode::Value,
                    needsValue);
            }
            break;
        case SJIT_STMT_NOOP:
        case SJIT_STMT_RESET_TIMER:
        case SJIT_STMT_LIST_DELETE_ALL:
        case SJIT_STMT_FOREVER:
        case SJIT_STMT_STOP_THIS_SCRIPT:
        case SJIT_STMT_PEN_CLEAR:
        case SJIT_STMT_PEN_DOWN:
        case SJIT_STMT_PEN_UP:
        case SJIT_STMT_LOOKS_SHOW:
        case SJIT_STMT_LOOKS_HIDE:
        case SJIT_STMT_STOP_OTHER_SCRIPTS:
        case SJIT_STMT_STOP_ALL:
        case SJIT_STMT_MONITOR_SHOW:
        case SJIT_STMT_MONITOR_HIDE:
            break;
        default:
            markProcedureArgumentUsesInExpr(
                procedure,
                statement.value,
                ProcedureArgumentUseMode::Value,
                needsValue);
            markProcedureArgumentUsesInExpr(
                procedure,
                statement.index,
                ProcedureArgumentUseMode::Value,
                needsValue);
            markProcedureArgumentUsesInExpr(
                procedure,
                statement.condition,
                ProcedureArgumentUseMode::Value,
                needsValue);
            markProcedureArgumentUsesInExpr(
                procedure,
                statement.times,
                ProcedureArgumentUseMode::Value,
                needsValue);
            break;
        }
    }
}

std::vector<std::vector<bool>> findProcedureArgumentValueRequirements(const SCompiledScript &script) {
    std::vector<std::vector<bool>> requirements;
    requirements.reserve(static_cast<size_t>(script.procedure_count));
    for (int i = 0; i < script.procedure_count; ++i) {
        const SCompiledProcedure &procedure = script.procedures[i];
        std::vector<bool> needsValue(static_cast<size_t>(std::max(0, procedure.argument_count)), false);
        markProcedureArgumentUsesInStatements(
            procedure,
            procedure.statements,
            procedure.statement_count,
            needsValue);
        requirements.push_back(std::move(needsValue));
    }
    return requirements;
}

struct ProcedureLoopVariableUses {
    std::unordered_set<const SExpr *> scalarExpressions;
    std::unordered_set<const SStatement *> scalarStatements;
};

void collectProcedureLoopExprUses(const SExpr *expr, ProcedureLoopVariableUses &uses) {
    if (!expr) {
        return;
    }
    if (expr->opcode == SJIT_EXPR_VARIABLE) {
        uses.scalarExpressions.insert(expr);
    }
    collectProcedureLoopExprUses(expr->left, uses);
    collectProcedureLoopExprUses(expr->right, uses);
}

bool isProcedureLoopStatement(int opcode) {
    switch (opcode) {
    case SJIT_STMT_REPEAT:
    case SJIT_STMT_REPEAT_UNTIL:
    case SJIT_STMT_WHILE:
    case SJIT_STMT_FOREVER:
    case SJIT_STMT_FOR_EACH:
        return true;
    default:
        return false;
    }
}

void collectProcedureLoopVariableUses(
    const SStatement *statements,
    int count,
    bool insideLoop,
    ProcedureLoopVariableUses &uses) {
    if (!statements || count <= 0) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        const SStatement &statement = statements[i];
        const bool loopStatement = isProcedureLoopStatement(statement.opcode);
        if (insideLoop) {
            collectProcedureLoopExprUses(statement.value, uses);
            collectProcedureLoopExprUses(statement.index, uses);
            collectProcedureLoopExprUses(statement.condition, uses);
            collectProcedureLoopExprUses(statement.times, uses);
            for (int argument = 0; argument < statement.argument_count; ++argument) {
                collectProcedureLoopExprUses(
                    statement.arguments ? statement.arguments[argument].value : nullptr,
                    uses);
            }
            switch (statement.opcode) {
            case SJIT_STMT_SET_VARIABLE:
            case SJIT_STMT_CHANGE_VARIABLE:
            case SJIT_STMT_FOR_EACH:
                uses.scalarStatements.insert(&statement);
                break;
            default:
                break;
            }
        }
        if (loopStatement) {
            // Loop conditions and for-each destinations are revisited even
            // when the surrounding statement itself is not already nested.
            collectProcedureLoopExprUses(statement.condition, uses);
            if (statement.opcode == SJIT_STMT_FOR_EACH) {
                uses.scalarStatements.insert(&statement);
            }
        }
        collectProcedureLoopVariableUses(
            statement.substack,
            statement.substack_count,
            insideLoop || loopStatement,
            uses);
        collectProcedureLoopVariableUses(
            statement.substack2,
            statement.substack2_count,
            insideLoop,
            uses);
    }
}

struct PreboundVariableLocation {
    SSprite *owner = nullptr;
    int index = -1;
};

using PreboundVariableMap = std::unordered_map<std::string, PreboundVariableLocation>;

bool unsafeRawRuntimeObjectConstantsEnabled() {
    /* SVariable arrays are reallocatable and therefore cannot be embedded in
       code safely.  Keep the old experiment available only for explicit
       profiling; normal code generation uses (owner, index, type) handles. */
    return std::getenv("SJIT_UNSAFE_EMBED_RUNTIME_POINTERS") != nullptr;
}

std::string preboundVariableKey(int type, const char *id, const char *name) {
    return std::to_string(type) + ":" +
        ((id && id[0] != '\0') ? "id:" : "name:") +
        ((id && id[0] != '\0') ? id : (name ? name : ""));
}

std::string compiledVariableKey(
    int type,
    const SString *id,
    const SString *name) {
    return preboundVariableKey(
        type,
        sjit_string_cstr(id),
        sjit_string_cstr(name));
}

PreboundVariableLocation findPreboundVariable(
    SRuntime *runtime,
    int targetId,
    int type,
    const char *id,
    const char *name,
    PreboundVariableMap &variables) {
    const std::string key = preboundVariableKey(type, id, name);
    auto found = variables.find(key);
    if (found != variables.end()) {
        return found->second;
    }
    PreboundVariableLocation location;
    SVariable *variable = sjit_runtime_lookup_or_create_variable_by_scratch_id(
        runtime, targetId, id ? id : "", name ? name : "", type);
    for (int targetIndex = 0; runtime && variable && targetIndex < runtime->target_count; ++targetIndex) {
        SSprite *owner = runtime->targets[targetIndex];
        for (int variableIndex = 0; owner && variableIndex < owner->base.variable_count; ++variableIndex) {
            if (&owner->base.variables[variableIndex] == variable) {
                location.owner = owner;
                location.index = variableIndex;
                break;
            }
        }
        if (location.owner) {
            break;
        }
    }
    variables.emplace(key, location);
    return location;
}

void prebindExprVariables(
    SRuntime *runtime,
    int targetId,
    SExpr *expr,
    PreboundVariableMap &variables) {
    if (!expr) {
        return;
    }
    int variableType = -1;
    if (expr->opcode == SJIT_EXPR_VARIABLE) {
        variableType = SJIT_VAR_SCALAR;
    } else {
        switch (expr->opcode) {
        case SJIT_EXPR_LIST_ITEM:
        case SJIT_EXPR_LIST_ITEM_NUMBER:
        case SJIT_EXPR_LIST_LENGTH:
        case SJIT_EXPR_LIST_CONTAINS:
        case SJIT_EXPR_LIST_VARIABLE:
            variableType = SJIT_VAR_LIST;
            break;
        default:
            break;
        }
    }
    if (variableType >= 0 && expr->literal.tag == SJIT_VALUE_STRING) {
        /* A script may be recompiled for another runtime.  Clear the previous
           specialization before lookup so a missing target or allocation
           failure cannot leave a stale owner behind the new runtime guard. */
        expr->variable_cache_owner = nullptr;
        expr->variable_cache_target_id = targetId;
        expr->variable_cache_owner_target_id = 0;
        expr->variable_cache_index = -1;
        expr->variable_cache_type = variableType;
        expr->variable_cache_owner_is_original = 0;
        expr->variable_cache_runtime = nullptr;
        expr->variable_cache_runtime_instance_id = 0;
        expr->variable_cache_identity = nullptr;
        const char *id = sjit_string_cstr(expr->variable_id);
        const char *name = sjit_string_cstr(static_cast<const SString *>(expr->literal.ptr));
        const PreboundVariableLocation location = findPreboundVariable(
            runtime,
            targetId,
            variableType,
            id,
            name,
            variables);
        if (location.owner && location.index >= 0) {
            /* The original target object itself is runtime-owned and stable.
               Safe IR may embed this owner only behind the runtime instance-id
               guard; it still reloads the reallocatable variable array. */
            expr->variable_cache_owner = location.owner;
            expr->variable_cache_target_id = targetId;
            expr->variable_cache_owner_target_id = location.owner->base.id;
            expr->variable_cache_index = location.index;
            expr->variable_cache_type = variableType;
            expr->variable_cache_owner_is_original = location.owner->base.is_original;
            expr->variable_cache_runtime = runtime;
            expr->variable_cache_runtime_instance_id = runtime->instance_id;
            const SVariable *variable = &location.owner->base.variables[location.index];
            expr->variable_cache_identity =
                id && id[0] != '\0' ? variable->scratch_id : variable->name;
        }
    }
    prebindExprVariables(runtime, targetId, expr->left, variables);
    prebindExprVariables(runtime, targetId, expr->right, variables);
}

void prebindStatementVariables(
    SRuntime *runtime,
    int targetId,
    SStatement *statements,
    int count,
    PreboundVariableMap &variables) {
    if (!statements || count <= 0) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        SStatement &statement = statements[i];
        int variableType = -1;
        switch (statement.opcode) {
        case SJIT_STMT_SET_VARIABLE:
        case SJIT_STMT_CHANGE_VARIABLE:
        case SJIT_STMT_FOR_EACH:
            variableType = SJIT_VAR_SCALAR;
            break;
        case SJIT_STMT_LIST_ADD:
        case SJIT_STMT_LIST_DELETE:
        case SJIT_STMT_LIST_DELETE_ALL:
        case SJIT_STMT_LIST_INSERT:
        case SJIT_STMT_LIST_REPLACE:
            variableType = SJIT_VAR_LIST;
            break;
        default:
            break;
        }
        if (variableType >= 0) {
            statement.variable_cache_owner = nullptr;
            statement.variable_cache_target_id = targetId;
            statement.variable_cache_owner_target_id = 0;
            statement.variable_cache_index = -1;
            statement.variable_cache_type = variableType;
            statement.variable_cache_owner_is_original = 0;
            statement.variable_cache_runtime = nullptr;
            statement.variable_cache_runtime_instance_id = 0;
            statement.variable_cache_identity = nullptr;
            const char *id = sjit_string_cstr(statement.variable_id);
            const char *name = sjit_string_cstr(statement.variable_name);
            const PreboundVariableLocation location = findPreboundVariable(
                runtime,
                targetId,
                variableType,
                id,
                name,
                variables);
            if (location.owner && location.index >= 0) {
                statement.variable_cache_owner = location.owner;
                statement.variable_cache_target_id = targetId;
                statement.variable_cache_owner_target_id = location.owner->base.id;
                statement.variable_cache_index = location.index;
                statement.variable_cache_type = variableType;
                statement.variable_cache_owner_is_original = location.owner->base.is_original;
                statement.variable_cache_runtime = runtime;
                statement.variable_cache_runtime_instance_id = runtime->instance_id;
                const SVariable *variable = &location.owner->base.variables[location.index];
                statement.variable_cache_identity =
                    id && id[0] != '\0' ? variable->scratch_id : variable->name;
            }
        }
        prebindExprVariables(runtime, targetId, statement.value, variables);
        prebindExprVariables(runtime, targetId, statement.index, variables);
        prebindExprVariables(runtime, targetId, statement.condition, variables);
        prebindExprVariables(runtime, targetId, statement.times, variables);
        for (int argument = 0; argument < statement.argument_count; ++argument) {
            prebindExprVariables(
                runtime,
                targetId,
                statement.arguments ? statement.arguments[argument].value : nullptr,
                variables);
        }
        prebindStatementVariables(
            runtime,
            targetId,
            statement.substack,
            statement.substack_count,
            variables);
        prebindStatementVariables(
            runtime,
            targetId,
            statement.substack2,
            statement.substack2_count,
            variables);
    }
}

void prebindScriptVariables(SRuntime *runtime, SCompiledScript &script) {
    if (!runtime) {
        return;
    }
    script.bound_target = unsafeRawRuntimeObjectConstantsEnabled() ?
        sjit_runtime_get_sprite(runtime, script.target_id) : nullptr;
    PreboundVariableMap variables;
    prebindStatementVariables(
        runtime,
        script.target_id,
        script.statements,
        script.statement_count,
        variables);
    for (int i = 0; i < script.procedure_count; ++i) {
        prebindStatementVariables(
            runtime,
            script.target_id,
            script.procedures[i].statements,
            script.procedures[i].statement_count,
            variables);
    }
}

SVariable *preboundExprVariable(const SExpr *expr, int type) {
    if (!unsafeRawRuntimeObjectConstantsEnabled() ||
        !expr || expr->variable_cache_type != type ||
        !expr->variable_cache_owner || !expr->variable_cache_owner_is_original ||
        expr->variable_cache_index < 0 ||
        expr->variable_cache_index >= expr->variable_cache_owner->base.variable_count) {
        return nullptr;
    }
    SVariable *variable = &expr->variable_cache_owner->base.variables[expr->variable_cache_index];
    return variable->type == type ? variable : nullptr;
}

SList *preboundExprList(const SExpr *expr) {
    SVariable *variable = preboundExprVariable(expr, SJIT_VAR_LIST);
    if (!variable || variable->value.tag != SJIT_VALUE_LIST || !variable->value.ptr) {
        return nullptr;
    }
    return static_cast<SList *>(variable->value.ptr);
}

SVariable *preboundStatementVariable(const SStatement *statement, int type) {
    if (!unsafeRawRuntimeObjectConstantsEnabled() ||
        !statement || statement->variable_cache_type != type ||
        !statement->variable_cache_owner || !statement->variable_cache_owner_is_original ||
        statement->variable_cache_index < 0 ||
        statement->variable_cache_index >= statement->variable_cache_owner->base.variable_count) {
        return nullptr;
    }
    SVariable *variable = &statement->variable_cache_owner->base.variables[statement->variable_cache_index];
    return variable->type == type ? variable : nullptr;
}

bool procedureExprNeedsSprite(const SExpr *expr) {
    if (!expr) {
        return false;
    }
    if (expr->opcode == SJIT_EXPR_X_POSITION || expr->opcode == SJIT_EXPR_Y_POSITION) {
        return true;
    }
    return procedureExprNeedsSprite(expr->left) || procedureExprNeedsSprite(expr->right);
}

bool procedureStatementsNeedSprite(const SStatement *statements, int count) {
    if (!statements || count <= 0) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        const SStatement &statement = statements[i];
        switch (statement.opcode) {
        case SJIT_STMT_PEN_DOWN:
        case SJIT_STMT_PEN_UP:
        case SJIT_STMT_PEN_SET_SIZE:
        case SJIT_STMT_MOTION_SET_X:
        case SJIT_STMT_MOTION_SET_Y:
        case SJIT_STMT_MOTION_CHANGE_X:
        case SJIT_STMT_MOTION_CHANGE_Y:
        case SJIT_STMT_MOTION_GOTO_XY:
        case SJIT_STMT_LOOKS_SET_SIZE:
        case SJIT_STMT_LOOKS_SHOW:
        case SJIT_STMT_LOOKS_HIDE:
        case SJIT_STMT_LOOKS_SET_EFFECT:
        case SJIT_STMT_LOOKS_CHANGE_EFFECT:
        case SJIT_STMT_LOOKS_CLEAR_EFFECTS:
        case SJIT_STMT_SENSING_SET_DRAG_MODE:
            return true;
        default:
            break;
        }
        if (procedureExprNeedsSprite(statement.value) ||
            procedureExprNeedsSprite(statement.index) ||
            procedureExprNeedsSprite(statement.condition) ||
            procedureExprNeedsSprite(statement.times)) {
            return true;
        }
        for (int argument = 0; argument < statement.argument_count; ++argument) {
            if (procedureExprNeedsSprite(
                    statement.arguments ? statement.arguments[argument].value : nullptr)) {
                return true;
            }
        }
        if (procedureStatementsNeedSprite(statement.substack, statement.substack_count) ||
            procedureStatementsNeedSprite(statement.substack2, statement.substack2_count)) {
            return true;
        }
    }
    return false;
}

bool stringEquals(const SString *left, const SString *right) {
    const char *leftText = sjit_string_cstr(left);
    const char *rightText = sjit_string_cstr(right);
    return leftText && rightText && std::strcmp(leftText, rightText) == 0;
}

bool stringEqualsIgnoreCase(const SString *left, const SString *right) {
    return left && right && sjit_string_equals_ignore_case(
        left,
        sjit_string_cstr(right));
}

bool scalarVariableExprNamed(const SExpr *expr, const SString *name) {
    return expr && name && expr->opcode == SJIT_EXPR_VARIABLE &&
        expr->literal.tag == SJIT_VALUE_STRING &&
        stringEquals(static_cast<const SString *>(expr->literal.ptr), name);
}

bool equivalentNativeNumberExpr(const SExpr *left, const SExpr *right) {
    if (left == right) {
        return true;
    }
    if (!left || !right || left->opcode != right->opcode) {
        return false;
    }
    if (left->opcode == SJIT_EXPR_LITERAL) {
        if (left->literal.tag != right->literal.tag) {
            return false;
        }
        if (left->literal.tag == SJIT_VALUE_STRING) {
            return stringEquals(
                static_cast<const SString *>(left->literal.ptr),
                static_cast<const SString *>(right->literal.ptr));
        }
        if (left->literal.tag == SJIT_VALUE_NULL) {
            return true;
        }
        return std::memcmp(
            &left->literal.number,
            &right->literal.number,
            sizeof(double)) == 0;
    }
    if (left->opcode == SJIT_EXPR_VARIABLE) {
        return left->literal.tag == SJIT_VALUE_STRING &&
            right->literal.tag == SJIT_VALUE_STRING &&
            stringEquals(
                static_cast<const SString *>(left->literal.ptr),
                static_cast<const SString *>(right->literal.ptr));
    }
    switch (left->opcode) {
    case SJIT_EXPR_ADD:
    case SJIT_EXPR_SUB:
    case SJIT_EXPR_MUL:
    case SJIT_EXPR_DIV:
    case SJIT_EXPR_MOD:
        return equivalentNativeNumberExpr(left->left, right->left) &&
            equivalentNativeNumberExpr(left->right, right->right);
    default:
        return false;
    }
}

bool exactNumericLiteral(const SExpr *expr, double expected) {
    double value = 0.0;
    return literalNumber(expr, value) && value == expected;
}

bool nativePenRowInvariantNumberExpr(
    const SExpr *expr,
    const SString *indexName,
    const SString *columnName) {
    if (!expr) {
        return false;
    }
    switch (expr->opcode) {
    case SJIT_EXPR_LITERAL:
        return expr->literal.tag != SJIT_VALUE_LIST;
    case SJIT_EXPR_VARIABLE: {
        if (expr->literal.tag != SJIT_VALUE_STRING || !expr->literal.ptr) {
            return false;
        }
        const auto *name = static_cast<const SString *>(expr->literal.ptr);
        return !stringEqualsIgnoreCase(name, indexName) &&
            !stringEqualsIgnoreCase(name, columnName);
    }
    case SJIT_EXPR_ADD:
    case SJIT_EXPR_SUB:
    case SJIT_EXPR_MUL:
    case SJIT_EXPR_DIV:
    case SJIT_EXPR_MOD:
        return nativePenRowInvariantNumberExpr(
                   expr->left,
                   indexName,
                   columnName) &&
            nativePenRowInvariantNumberExpr(
                   expr->right,
                   indexName,
                   columnName);
    default:
        return false;
    }
}

struct NativePenRowPattern {
    const SExpr *colorExpr = nullptr;
    const SExpr *brightnessExpr = nullptr;
    const SExpr *rowExpr = nullptr;
    const SExpr *columnExpr = nullptr;
    const SExpr *indexExpr = nullptr;
    const SExpr *replacementLiteral = nullptr;
    const SExpr *xStepExpr = nullptr;
    int paramId = 0;
};

bool matchNativePenRowPattern(
    const SStatement *repeat,
    NativePenRowPattern &out) {
    if (!repeat || repeat->opcode != SJIT_STMT_REPEAT ||
        !repeat->times || !repeat->substack || repeat->substack_count != 4) {
        return false;
    }
    const SStatement &setIndex = repeat->substack[0];
    const SStatement &condition = repeat->substack[1];
    const SStatement &changeX = repeat->substack[2];
    const SStatement &changeColumn = repeat->substack[3];
    if (setIndex.opcode != SJIT_STMT_SET_VARIABLE || !setIndex.variable_name ||
        condition.opcode != SJIT_STMT_IF || !condition.condition ||
        changeX.opcode != SJIT_STMT_MOTION_CHANGE_X || !changeX.value ||
        changeColumn.opcode != SJIT_STMT_CHANGE_VARIABLE ||
        !changeColumn.variable_name ||
        !exactNumericLiteral(changeColumn.value, 1.0) ||
        !nativePenRowInvariantNumberExpr(
            repeat->times,
            setIndex.variable_name,
            changeColumn.variable_name) ||
        !nativePenRowInvariantNumberExpr(
            changeX.value,
            setIndex.variable_name,
            changeColumn.variable_name) ||
        !condition.substack || condition.substack_count != 5) {
        return false;
    }

    const SExpr *sum = setIndex.value;
    const SExpr *rowAndColumn = sum && sum->opcode == SJIT_EXPR_ADD ?
        sum->left : nullptr;
    const SExpr *one = sum && sum->opcode == SJIT_EXPR_ADD ?
        sum->right : nullptr;
    const SExpr *rowProduct = rowAndColumn &&
        rowAndColumn->opcode == SJIT_EXPR_ADD ? rowAndColumn->left : nullptr;
    const SExpr *columnExpr = rowAndColumn &&
        rowAndColumn->opcode == SJIT_EXPR_ADD ? rowAndColumn->right : nullptr;
    const SExpr *rowExpr = rowProduct && rowProduct->opcode == SJIT_EXPR_MUL ?
        rowProduct->left : nullptr;
    const SExpr *strideExpr = rowProduct && rowProduct->opcode == SJIT_EXPR_MUL ?
        rowProduct->right : nullptr;
    if (!exactNumericLiteral(one, 1.0) ||
        !scalarVariableExprNamed(columnExpr, changeColumn.variable_name) ||
        !rowExpr || rowExpr->opcode != SJIT_EXPR_VARIABLE ||
        !equivalentNativeNumberExpr(strideExpr, repeat->times)) {
        return false;
    }

    const SExpr *notExpr = condition.condition;
    const SExpr *equalExpr = notExpr->opcode == SJIT_EXPR_NOT ?
        notExpr->left : nullptr;
    const SExpr *conditionColor = equalExpr && equalExpr->opcode == SJIT_EXPR_EQ ?
        equalExpr->left : nullptr;
    const SExpr *conditionLiteral = equalExpr && equalExpr->opcode == SJIT_EXPR_EQ ?
        equalExpr->right : nullptr;
    if (!conditionColor || conditionColor->opcode != SJIT_EXPR_LIST_ITEM ||
        !scalarVariableExprNamed(conditionColor->left, setIndex.variable_name) ||
        !conditionLiteral || conditionLiteral->opcode != SJIT_EXPR_LITERAL ||
        conditionLiteral->literal.tag != SJIT_VALUE_STRING) {
        return false;
    }

    const SStatement &setColor = condition.substack[0];
    const SStatement &changeParam = condition.substack[1];
    const SStatement &replaceColor = condition.substack[2];
    if (setColor.opcode != SJIT_STMT_PEN_SET_COLOR ||
        changeParam.opcode != SJIT_STMT_PEN_CHANGE_COLOR_PARAM ||
        replaceColor.opcode != SJIT_STMT_LIST_REPLACE ||
        condition.substack[3].opcode != SJIT_STMT_PEN_DOWN ||
        condition.substack[4].opcode != SJIT_STMT_PEN_UP ||
        !setColor.value || setColor.value->opcode != SJIT_EXPR_LIST_ITEM ||
        !changeParam.value || changeParam.value->opcode != SJIT_EXPR_LIST_ITEM ||
        !scalarVariableExprNamed(setColor.value->left, setIndex.variable_name) ||
        !scalarVariableExprNamed(changeParam.value->left, setIndex.variable_name) ||
        !scalarVariableExprNamed(replaceColor.index, setIndex.variable_name) ||
        !replaceColor.variable_name ||
        setColor.value->literal.tag != SJIT_VALUE_STRING ||
        changeParam.value->literal.tag != SJIT_VALUE_STRING ||
        conditionColor->literal.tag != SJIT_VALUE_STRING ||
        !stringEquals(
            static_cast<const SString *>(conditionColor->literal.ptr),
            static_cast<const SString *>(setColor.value->literal.ptr)) ||
        !stringEquals(
            static_cast<const SString *>(setColor.value->literal.ptr),
            replaceColor.variable_name) ||
        !replaceColor.value || replaceColor.value->opcode != SJIT_EXPR_LITERAL ||
        replaceColor.value->literal.tag != SJIT_VALUE_STRING ||
        !stringEquals(
            static_cast<const SString *>(conditionLiteral->literal.ptr),
            static_cast<const SString *>(replaceColor.value->literal.ptr)) ||
        !changeParam.index || changeParam.index->opcode != SJIT_EXPR_LITERAL ||
        changeParam.index->literal.tag != SJIT_VALUE_STRING) {
        return false;
    }
    const int paramId = sjit_pen_color_param_id(sjit_string_cstr(
        static_cast<const SString *>(changeParam.index->literal.ptr)));
    if (paramId == 0) {
        return false;
    }

    out.colorExpr = setColor.value;
    out.brightnessExpr = changeParam.value;
    out.rowExpr = rowExpr;
    out.columnExpr = columnExpr;
    out.indexExpr = setColor.value->left;
    out.replacementLiteral = replaceColor.value;
    out.xStepExpr = changeX.value;
    out.paramId = paramId;
    return true;
}

bool exprReferencesScalarVariable(const SExpr *expr, const SString *variableName) {
    if (!expr || !variableName) {
        return false;
    }
    if (expr->opcode == SJIT_EXPR_VARIABLE &&
        expr->literal.tag == SJIT_VALUE_STRING &&
        stringEquals(static_cast<const SString *>(expr->literal.ptr), variableName)) {
        return true;
    }
    return exprReferencesScalarVariable(expr->left, variableName) ||
        exprReferencesScalarVariable(expr->right, variableName);
}

bool entrySupportsNumberExpr(const SExpr *expr) {
    if (!expr) {
        return false;
    }
    double literal = 0.0;
    if (literalNumber(expr, literal)) {
        return true;
    }
    switch (expr->opcode) {
    case SJIT_EXPR_TIMER:
    case SJIT_EXPR_MOUSE_X:
    case SJIT_EXPR_MOUSE_Y:
    case SJIT_EXPR_X_POSITION:
    case SJIT_EXPR_Y_POSITION:
    case SJIT_EXPR_VARIABLE:
    case SJIT_EXPR_LIST_LENGTH:
        return true;
    case SJIT_EXPR_LIST_ITEM:
        return entrySupportsNumberExpr(expr->left);
    case SJIT_EXPR_ADD:
    case SJIT_EXPR_SUB:
    case SJIT_EXPR_MUL:
    case SJIT_EXPR_DIV:
    case SJIT_EXPR_MOD:
        return entrySupportsNumberExpr(expr->left) && entrySupportsNumberExpr(expr->right);
    default:
        return false;
    }
}

const SStatement *bulkRepeatChangeCandidate(const SStatement *statement) {
    if (!statement ||
        statement->opcode != SJIT_STMT_REPEAT ||
        statement->substack_count != 1 ||
        !statement->substack) {
        return nullptr;
    }
    const SStatement *change = &statement->substack[0];
    if (change->opcode != SJIT_STMT_CHANGE_VARIABLE || !change->variable_name) {
        return nullptr;
    }
    if (!entrySupportsNumberExpr(change->value)) {
        return nullptr;
    }
    if (exprReferencesScalarVariable(change->value, change->variable_name)) {
        return nullptr;
    }
    return change;
}

bool smallWarpProcedureStatements(const SStatement *statements, int count, int &remainingBudget) {
    if (count <= 0) {
        return true;
    }
    if (!statements || count > remainingBudget) {
        return false;
    }
    remainingBudget -= count;
    for (int i = 0; i < count; ++i) {
        const SStatement &statement = statements[i];
        switch (statement.opcode) {
        case SJIT_STMT_REPEAT:
        case SJIT_STMT_REPEAT_UNTIL:
        case SJIT_STMT_WHILE:
        case SJIT_STMT_FOREVER:
        case SJIT_STMT_FOR_EACH:
            return false;
        case SJIT_STMT_IF:
            if (!smallWarpProcedureStatements(
                    statement.substack,
                    statement.substack_count,
                    remainingBudget)) {
                return false;
            }
            break;
        case SJIT_STMT_IF_ELSE:
            if (!smallWarpProcedureStatements(
                    statement.substack,
                    statement.substack_count,
                    remainingBudget) ||
                !smallWarpProcedureStatements(
                    statement.substack2,
                    statement.substack2_count,
                    remainingBudget)) {
                return false;
            }
            break;
        default:
            break;
        }
    }
    return true;
}

bool shouldAlwaysInlineWarpProcedure(const SCompiledProcedure &procedure) {
    int remainingBudget = 12;
    return procedure.warp_mode && smallWarpProcedureStatements(
        procedure.statements,
        procedure.statement_count,
        remainingBudget);
}

bool procedureSupportsNumberExpr(const SCompiledProcedure &procedure, const SExpr *expr);
bool procedureCanEvaluateNumberExpr(const SCompiledProcedure &procedure, const SExpr *expr);
bool procedureExprHasNumberResult(const SExpr *expr);
bool procedureSupportsValueExpr(const SCompiledProcedure &procedure, const SExpr *expr);

bool procedureSupportsBoolExpr(const SCompiledProcedure &procedure, const SExpr *expr) {
    if (!expr) {
        return false;
    }
    if (sjit_expr_opcode_effects(expr->opcode).requiresInterpreter) {
        return false;
    }
    bool literal = false;
    if (literalBool(expr, literal)) {
        return true;
    }
    switch (expr->opcode) {
    case SJIT_EXPR_MOUSE_DOWN:
        return true;
    case SJIT_EXPR_KEY_PRESSED:
        return literalKeyIndex(expr->left) >= 0 ||
            procedureSupportsValueExpr(procedure, expr->left);
    case SJIT_EXPR_LIST_CONTAINS:
        return procedureSupportsValueExpr(procedure, expr->left);
    case SJIT_EXPR_CONTAINS:
        return procedureSupportsValueExpr(procedure, expr->left) &&
            procedureSupportsValueExpr(procedure, expr->right);
    case SJIT_EXPR_VARIABLE:
        return true;
    case SJIT_EXPR_ARGUMENT:
    case SJIT_EXPR_LIST_ITEM:
    case SJIT_EXPR_JOIN:
    case SJIT_EXPR_LETTER_OF:
        return procedureSupportsValueExpr(procedure, expr);
    case SJIT_EXPR_LT:
    case SJIT_EXPR_EQ:
    case SJIT_EXPR_GT:
        return (procedureSupportsNumberExpr(procedure, expr->left) &&
                procedureSupportsNumberExpr(procedure, expr->right)) ||
            (procedureSupportsValueExpr(procedure, expr->left) &&
             procedureSupportsValueExpr(procedure, expr->right));
    case SJIT_EXPR_AND:
    case SJIT_EXPR_OR:
        return procedureSupportsBoolExpr(procedure, expr->left) &&
            procedureSupportsBoolExpr(procedure, expr->right);
    case SJIT_EXPR_NOT:
        return procedureSupportsBoolExpr(procedure, expr->left);
    default:
        return procedureExprHasNumberResult(expr) ?
            procedureCanEvaluateNumberExpr(procedure, expr) :
            procedureSupportsValueExpr(procedure, expr);
    }
}

bool procedureSupportsNumberExpr(const SCompiledProcedure &procedure, const SExpr *expr) {
    if (!expr) {
        return false;
    }
    if (sjit_expr_opcode_effects(expr->opcode).requiresInterpreter) {
        return false;
    }
    double literal = 0.0;
    if (literalNumber(expr, literal)) {
        return true;
    }
    switch (expr->opcode) {
    case SJIT_EXPR_TIMER:
    case SJIT_EXPR_DAYS_SINCE_2000:
    case SJIT_EXPR_MOUSE_X:
    case SJIT_EXPR_MOUSE_Y:
    case SJIT_EXPR_X_POSITION:
    case SJIT_EXPR_Y_POSITION:
    case SJIT_EXPR_VARIABLE:
    case SJIT_EXPR_LIST_LENGTH:
        return true;
    case SJIT_EXPR_ARGUMENT:
        return procedureArgumentIndex(procedure, expr) >= 0;
    case SJIT_EXPR_LIST_ITEM:
        return procedureSupportsValueExpr(procedure, expr->left);
    case SJIT_EXPR_LIST_ITEM_NUMBER:
        return procedureSupportsValueExpr(procedure, expr->left);
    case SJIT_EXPR_LENGTH: {
        return procedureSupportsValueExpr(procedure, expr->left);
    }
    case SJIT_EXPR_JOIN:
    case SJIT_EXPR_LETTER_OF:
        return procedureSupportsValueExpr(procedure, expr);
    case SJIT_EXPR_MOUSE_DOWN:
    case SJIT_EXPR_KEY_PRESSED:
    case SJIT_EXPR_LIST_CONTAINS:
    case SJIT_EXPR_CONTAINS:
    case SJIT_EXPR_LT:
    case SJIT_EXPR_EQ:
    case SJIT_EXPR_GT:
    case SJIT_EXPR_AND:
    case SJIT_EXPR_OR:
    case SJIT_EXPR_NOT:
        return procedureSupportsBoolExpr(procedure, expr);
    case SJIT_EXPR_MATHOP:
    case SJIT_EXPR_ROUND:
        return procedureSupportsNumberExpr(procedure, expr->left);
    case SJIT_EXPR_ADD:
    case SJIT_EXPR_SUB:
    case SJIT_EXPR_MUL:
    case SJIT_EXPR_DIV:
    case SJIT_EXPR_MOD:
    case SJIT_EXPR_RANDOM:
        return procedureSupportsNumberExpr(procedure, expr->left) &&
            procedureSupportsNumberExpr(procedure, expr->right);
    default:
        return false;
    }
}

bool procedureSupportsValueExpr(const SCompiledProcedure &procedure, const SExpr *expr) {
    if (!expr) {
        return false;
    }
    if (sjit_expr_opcode_effects(expr->opcode).requiresInterpreter) {
        return false;
    }
    if (expr->opcode == SJIT_EXPR_LITERAL) {
        return true;
    }
    switch (expr->opcode) {
    case SJIT_EXPR_ARGUMENT:
        return procedureArgumentIndex(procedure, expr) >= 0;
    case SJIT_EXPR_VARIABLE:
        return true;
    case SJIT_EXPR_LIST_ITEM:
        return procedureSupportsValueExpr(procedure, expr->left);
    case SJIT_EXPR_JOIN:
        return procedureSupportsValueExpr(procedure, expr->left) &&
            procedureSupportsValueExpr(procedure, expr->right);
    case SJIT_EXPR_LETTER_OF:
        return procedureSupportsValueExpr(procedure, expr->left) &&
            procedureSupportsValueExpr(procedure, expr->right);
    case SJIT_EXPR_LT:
    case SJIT_EXPR_EQ:
    case SJIT_EXPR_GT:
    case SJIT_EXPR_AND:
    case SJIT_EXPR_OR:
    case SJIT_EXPR_NOT:
    case SJIT_EXPR_MOUSE_DOWN:
    case SJIT_EXPR_KEY_PRESSED:
    case SJIT_EXPR_LIST_CONTAINS:
    case SJIT_EXPR_CONTAINS:
        return procedureSupportsBoolExpr(procedure, expr);
    case SJIT_EXPR_TIMER:
    case SJIT_EXPR_DAYS_SINCE_2000:
    case SJIT_EXPR_MOUSE_X:
    case SJIT_EXPR_MOUSE_Y:
    case SJIT_EXPR_X_POSITION:
    case SJIT_EXPR_Y_POSITION:
    case SJIT_EXPR_LIST_LENGTH:
    case SJIT_EXPR_LIST_ITEM_NUMBER:
    case SJIT_EXPR_LENGTH:
    case SJIT_EXPR_MATHOP:
    case SJIT_EXPR_ROUND:
    case SJIT_EXPR_ADD:
    case SJIT_EXPR_SUB:
    case SJIT_EXPR_MUL:
    case SJIT_EXPR_DIV:
    case SJIT_EXPR_MOD:
    case SJIT_EXPR_RANDOM:
        return procedureCanEvaluateNumberExpr(procedure, expr);
    default:
        return false;
    }
}

bool procedureCanEvaluateNumberExpr(const SCompiledProcedure &procedure, const SExpr *expr) {
    if (!expr) {
        return false;
    }
    if (procedureSupportsNumberExpr(procedure, expr)) {
        return true;
    }
    switch (expr->opcode) {
    case SJIT_EXPR_LITERAL:
        // Scratch arithmetic coerces every literal to a number. In
        // particular, projects commonly encode unary negation as "" - x.
        return true;
    case SJIT_EXPR_DAYS_SINCE_2000:
        return true;
    case SJIT_EXPR_LENGTH:
    case SJIT_EXPR_LIST_ITEM_NUMBER:
    case SJIT_EXPR_MATHOP:
    case SJIT_EXPR_ROUND:
        return procedureSupportsValueExpr(procedure, expr->left) ||
            procedureCanEvaluateNumberExpr(procedure, expr->left);
    case SJIT_EXPR_ADD:
    case SJIT_EXPR_SUB:
    case SJIT_EXPR_MUL:
    case SJIT_EXPR_DIV:
    case SJIT_EXPR_MOD:
    case SJIT_EXPR_RANDOM:
        return procedureCanEvaluateNumberExpr(procedure, expr->left) &&
            procedureCanEvaluateNumberExpr(procedure, expr->right);
    default:
        return false;
    }
}

bool procedureExprHasNumberResult(const SExpr *expr) {
    if (!expr) {
        return false;
    }
    if (expr->opcode == SJIT_EXPR_LITERAL) {
        return expr->literal.tag == SJIT_VALUE_NUMBER ||
            expr->literal.tag == SJIT_VALUE_BOOL ||
            expr->literal.tag == SJIT_VALUE_NULL;
    }
    switch (expr->opcode) {
    case SJIT_EXPR_TIMER:
    case SJIT_EXPR_MOUSE_X:
    case SJIT_EXPR_MOUSE_Y:
    case SJIT_EXPR_X_POSITION:
    case SJIT_EXPR_Y_POSITION:
    case SJIT_EXPR_LIST_LENGTH:
    case SJIT_EXPR_LIST_ITEM_NUMBER:
    case SJIT_EXPR_DAYS_SINCE_2000:
    case SJIT_EXPR_LENGTH:
    case SJIT_EXPR_MATHOP:
    case SJIT_EXPR_RANDOM:
    case SJIT_EXPR_ROUND:
    case SJIT_EXPR_ADD:
    case SJIT_EXPR_SUB:
    case SJIT_EXPR_MUL:
    case SJIT_EXPR_DIV:
    case SJIT_EXPR_MOD:
        return true;
    default:
        return false;
    }
}

bool procedureSupportsValueSlotExpr(const SCompiledProcedure &procedure, const SExpr *expr) {
    if (procedureExprHasNumberResult(expr)) {
        return procedureCanEvaluateNumberExpr(procedure, expr);
    }
    return procedureSupportsValueExpr(procedure, expr);
}

bool procedureSupportsProcedureCallArguments(const SCompiledProcedure &procedure, const SStatement &statement) {
    if (statement.argument_count < 0 || (statement.argument_count > 0 && !statement.arguments)) {
        return false;
    }
    for (int argument = 0; argument < statement.argument_count; ++argument) {
        if (!statement.arguments[argument].value ||
            !procedureSupportsValueExpr(procedure, statement.arguments[argument].value)) {
            return false;
        }
    }
    return true;
}

bool scriptSupportsNumberExpr(const SExpr *expr);
bool scriptSupportsBoolExpr(const SExpr *expr);
bool scriptSupportsValueExpr(const SExpr *expr);

bool scriptSupportsBoolExpr(const SExpr *expr) {
    if (!expr) {
        return false;
    }
    if (sjit_expr_opcode_effects(expr->opcode).requiresInterpreter) {
        return false;
    }
    bool literal = false;
    if (literalBool(expr, literal)) {
        return true;
    }
    switch (expr->opcode) {
    case SJIT_EXPR_MOUSE_DOWN:
        return true;
    case SJIT_EXPR_KEY_PRESSED:
        return literalKeyIndex(expr->left) >= 0 || scriptSupportsValueExpr(expr->left);
    case SJIT_EXPR_LIST_CONTAINS:
        return scriptSupportsValueExpr(expr->left);
    case SJIT_EXPR_CONTAINS:
        return scriptSupportsValueExpr(expr->left) &&
            scriptSupportsValueExpr(expr->right);
    case SJIT_EXPR_VARIABLE:
        return true;
    case SJIT_EXPR_LIST_ITEM:
    case SJIT_EXPR_JOIN:
    case SJIT_EXPR_LETTER_OF:
        return scriptSupportsValueExpr(expr);
    case SJIT_EXPR_LT:
    case SJIT_EXPR_EQ:
    case SJIT_EXPR_GT:
        return (scriptSupportsNumberExpr(expr->left) && scriptSupportsNumberExpr(expr->right)) ||
            (scriptSupportsValueExpr(expr->left) && scriptSupportsValueExpr(expr->right));
    case SJIT_EXPR_AND:
    case SJIT_EXPR_OR:
        return scriptSupportsBoolExpr(expr->left) && scriptSupportsBoolExpr(expr->right);
    case SJIT_EXPR_NOT:
        return scriptSupportsBoolExpr(expr->left);
    default:
        return procedureExprHasNumberResult(expr) ?
            scriptSupportsNumberExpr(expr) :
            scriptSupportsValueExpr(expr);
    }
}

bool scriptSupportsNumberExpr(const SExpr *expr) {
    if (!expr) {
        return false;
    }
    if (sjit_expr_opcode_effects(expr->opcode).requiresInterpreter) {
        return false;
    }
    double literal = 0.0;
    if (literalNumber(expr, literal)) {
        return true;
    }
    switch (expr->opcode) {
    case SJIT_EXPR_TIMER:
    case SJIT_EXPR_DAYS_SINCE_2000:
    case SJIT_EXPR_MOUSE_X:
    case SJIT_EXPR_MOUSE_Y:
    case SJIT_EXPR_X_POSITION:
    case SJIT_EXPR_Y_POSITION:
    case SJIT_EXPR_VARIABLE:
    case SJIT_EXPR_LIST_LENGTH:
        return true;
    case SJIT_EXPR_LIST_ITEM:
    case SJIT_EXPR_LIST_ITEM_NUMBER:
        return scriptSupportsValueExpr(expr->left);
    case SJIT_EXPR_LENGTH:
        return scriptSupportsValueExpr(expr->left);
    case SJIT_EXPR_JOIN:
    case SJIT_EXPR_LETTER_OF:
        return scriptSupportsValueExpr(expr);
    case SJIT_EXPR_MATHOP:
    case SJIT_EXPR_ROUND:
        return scriptSupportsNumberExpr(expr->left);
    case SJIT_EXPR_ADD:
    case SJIT_EXPR_SUB:
    case SJIT_EXPR_MUL:
    case SJIT_EXPR_DIV:
    case SJIT_EXPR_MOD:
    case SJIT_EXPR_RANDOM:
        return scriptSupportsNumberExpr(expr->left) && scriptSupportsNumberExpr(expr->right);
    default:
        return false;
    }
}

bool scriptSupportsValueExpr(const SExpr *expr) {
    if (!expr) {
        return false;
    }
    if (sjit_expr_opcode_effects(expr->opcode).requiresInterpreter) {
        return false;
    }
    if (expr->opcode == SJIT_EXPR_LITERAL) {
        return true;
    }
    switch (expr->opcode) {
    case SJIT_EXPR_VARIABLE:
        return true;
    case SJIT_EXPR_LIST_ITEM:
        return scriptSupportsValueExpr(expr->left);
    case SJIT_EXPR_JOIN:
        return scriptSupportsValueExpr(expr->left) && scriptSupportsValueExpr(expr->right);
    case SJIT_EXPR_LETTER_OF:
        return scriptSupportsValueExpr(expr->left) && scriptSupportsValueExpr(expr->right);
    case SJIT_EXPR_LT:
    case SJIT_EXPR_EQ:
    case SJIT_EXPR_GT:
    case SJIT_EXPR_AND:
    case SJIT_EXPR_OR:
    case SJIT_EXPR_NOT:
    case SJIT_EXPR_MOUSE_DOWN:
    case SJIT_EXPR_KEY_PRESSED:
    case SJIT_EXPR_LIST_CONTAINS:
    case SJIT_EXPR_CONTAINS:
        return scriptSupportsBoolExpr(expr);
    case SJIT_EXPR_TIMER:
    case SJIT_EXPR_DAYS_SINCE_2000:
    case SJIT_EXPR_MOUSE_X:
    case SJIT_EXPR_MOUSE_Y:
    case SJIT_EXPR_X_POSITION:
    case SJIT_EXPR_Y_POSITION:
    case SJIT_EXPR_LIST_LENGTH:
    case SJIT_EXPR_LIST_ITEM_NUMBER:
    case SJIT_EXPR_LENGTH:
    case SJIT_EXPR_MATHOP:
    case SJIT_EXPR_ROUND:
    case SJIT_EXPR_ADD:
    case SJIT_EXPR_SUB:
    case SJIT_EXPR_MUL:
    case SJIT_EXPR_DIV:
    case SJIT_EXPR_MOD:
    case SJIT_EXPR_RANDOM:
        return scriptSupportsNumberExpr(expr);
    default:
        return false;
    }
}

bool procedureSupportsStatements(
    const SCompiledScript &script,
    const SCompiledProcedure &procedure,
    const SStatement *statements,
    int count,
    const std::vector<bool> *eligibleProcedures) {
    if (count <= 0) {
        return true;
    }
    if (!statements) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        const SStatement &statement = statements[i];
        if (sjit_statement_opcode_effects(statement.opcode).requiresInterpreter) {
            return false;
        }
        switch (statement.opcode) {
        case SJIT_STMT_NOOP:
        case SJIT_STMT_RESET_TIMER:
        case SJIT_STMT_STOP_THIS_SCRIPT:
        case SJIT_STMT_LIST_DELETE_ALL:
        case SJIT_STMT_PEN_CLEAR:
        case SJIT_STMT_PEN_DOWN:
        case SJIT_STMT_PEN_UP:
        case SJIT_STMT_LOOKS_SHOW:
        case SJIT_STMT_LOOKS_HIDE:
        case SJIT_STMT_LOOKS_CLEAR_EFFECTS:
        case SJIT_STMT_SENSING_SET_DRAG_MODE:
        case SJIT_STMT_MONITOR_SHOW:
        case SJIT_STMT_MONITOR_HIDE:
            break;
        case SJIT_STMT_SET_VARIABLE:
        case SJIT_STMT_LIST_ADD:
            if (!procedureSupportsValueSlotExpr(procedure, statement.value)) {
                return false;
            }
            break;
        case SJIT_STMT_CHANGE_VARIABLE:
            if (!procedureCanEvaluateNumberExpr(procedure, statement.value)) {
                return false;
            }
            break;
        case SJIT_STMT_LIST_REPLACE:
            if (!procedureSupportsValueSlotExpr(procedure, statement.index) ||
                !procedureSupportsValueSlotExpr(procedure, statement.value)) {
                return false;
            }
            break;
        case SJIT_STMT_LIST_DELETE:
            if (!procedureSupportsValueSlotExpr(procedure, statement.index)) {
                return false;
            }
            break;
        case SJIT_STMT_LIST_INSERT:
            if (!procedureSupportsValueSlotExpr(procedure, statement.index) ||
                !procedureSupportsValueSlotExpr(procedure, statement.value)) {
                return false;
            }
            break;
        case SJIT_STMT_SAY:
        case SJIT_STMT_PEN_SET_COLOR:
        case SJIT_STMT_LOOKS_SWITCH_BACKDROP:
            if (!procedureSupportsValueSlotExpr(procedure, statement.value)) {
                return false;
            }
            break;
        case SJIT_STMT_PEN_SET_SIZE:
        case SJIT_STMT_MOTION_SET_X:
        case SJIT_STMT_MOTION_SET_Y:
        case SJIT_STMT_MOTION_CHANGE_X:
        case SJIT_STMT_MOTION_CHANGE_Y:
        case SJIT_STMT_LOOKS_SET_SIZE:
        case SJIT_STMT_LOOKS_SET_EFFECT:
        case SJIT_STMT_LOOKS_CHANGE_EFFECT:
            if (!procedureCanEvaluateNumberExpr(procedure, statement.value)) {
                return false;
            }
            break;
        case SJIT_STMT_PEN_CHANGE_COLOR_PARAM:
            if (!procedureSupportsValueExpr(procedure, statement.index) ||
                !procedureSupportsValueExpr(procedure, statement.value)) {
                return false;
            }
            break;
        case SJIT_STMT_MOTION_GOTO_XY:
            if (!procedureCanEvaluateNumberExpr(procedure, statement.value) ||
                !procedureCanEvaluateNumberExpr(procedure, statement.index)) {
                return false;
            }
            break;
        case SJIT_STMT_IF:
            if (!procedureSupportsBoolExpr(procedure, statement.condition) ||
                !procedureSupportsStatements(
                    script,
                    procedure,
                    statement.substack,
                    statement.substack_count,
                    eligibleProcedures)) {
                return false;
            }
            break;
        case SJIT_STMT_IF_ELSE:
            if (!procedureSupportsBoolExpr(procedure, statement.condition) ||
                !procedureSupportsStatements(
                    script,
                    procedure,
                    statement.substack,
                    statement.substack_count,
                    eligibleProcedures) ||
                !procedureSupportsStatements(
                    script,
                    procedure,
                    statement.substack2,
                    statement.substack2_count,
                    eligibleProcedures)) {
                return false;
            }
            break;
        case SJIT_STMT_REPEAT:
            if (!procedureCanEvaluateNumberExpr(procedure, statement.times) ||
                !procedureSupportsStatements(
                    script,
                    procedure,
                    statement.substack,
                    statement.substack_count,
                    eligibleProcedures)) {
                return false;
            }
            break;
        case SJIT_STMT_REPEAT_UNTIL:
        case SJIT_STMT_WHILE:
            if (!procedureSupportsBoolExpr(procedure, statement.condition) ||
                !procedureSupportsStatements(
                    script,
                    procedure,
                    statement.substack,
                    statement.substack_count,
                    eligibleProcedures)) {
                return false;
            }
            break;
        case SJIT_STMT_FOREVER:
            if (!procedureSupportsStatements(
                    script,
                    procedure,
                    statement.substack,
                    statement.substack_count,
                    eligibleProcedures)) {
                return false;
            }
            break;
        case SJIT_STMT_FOR_EACH:
            if (!procedureCanEvaluateNumberExpr(procedure, statement.times) ||
                !procedureSupportsStatements(
                    script,
                    procedure,
                    statement.substack,
                    statement.substack_count,
                    eligibleProcedures)) {
                return false;
            }
            break;
        case SJIT_STMT_PROCEDURE_CALL: {
            const int target = findProcedureIndex(script, statement.procedure_name);
            if (target >= 0 &&
                statement.argument_count != script.procedures[target].argument_count) {
                return false;
            }
            if (target >= 0 &&
                sjit_string_equals_ignore_case(
                    procedure.name,
                    sjit_string_cstr(statement.procedure_name)) &&
                !procedure.warp_mode) {
                return false;
            }
            if (target >= 0 &&
                eligibleProcedures &&
                (target >= (int)eligibleProcedures->size() || !(*eligibleProcedures)[static_cast<size_t>(target)])) {
                return false;
            }
            if (!procedureSupportsProcedureCallArguments(procedure, statement)) {
                return false;
            }
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

std::vector<bool> findDirectProcedureEligibility(const SCompiledScript &script) {
    std::vector<bool> eligible(static_cast<size_t>(script.procedure_count), false);
    for (int i = 0; i < script.procedure_count; ++i) {
        const SCompiledProcedure &procedure = script.procedures[i];
        eligible[static_cast<size_t>(i)] = procedureSupportsStatements(
            script,
            procedure,
            procedure.statements,
            procedure.statement_count,
            nullptr);
    }
    auto pruneIneligibleDependencies = [&](std::vector<bool> &candidates) {
        bool changed = true;
        while (changed) {
            changed = false;
            for (int i = 0; i < script.procedure_count; ++i) {
                if (!candidates[static_cast<size_t>(i)]) {
                    continue;
                }
                const SCompiledProcedure &procedure = script.procedures[i];
                if (!procedureSupportsStatements(
                        script,
                        procedure,
                        procedure.statements,
                        procedure.statement_count,
                        &candidates)) {
                    candidates[static_cast<size_t>(i)] = false;
                    changed = true;
                }
            }
        }
    };

    pruneIneligibleDependencies(eligible);
    return eligible;
}

void collectReachableProcedures(
    const SCompiledScript &script,
    const SStatement *statements,
    int count,
    std::vector<bool> &reachable) {
    if (!statements || count <= 0) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        const SStatement &statement = statements[i];
        if (statement.opcode == SJIT_STMT_PROCEDURE_CALL) {
            const int target = findProcedureIndex(script, statement.procedure_name);
            if (target >= 0 && !reachable[static_cast<size_t>(target)]) {
                reachable[static_cast<size_t>(target)] = true;
                const SCompiledProcedure &procedure = script.procedures[target];
                collectReachableProcedures(
                    script,
                    procedure.statements,
                    procedure.statement_count,
                    reachable);
            }
        }
        collectReachableProcedures(
            script,
            statement.substack,
            statement.substack_count,
            reachable);
        collectReachableProcedures(
            script,
            statement.substack2,
            statement.substack2_count,
            reachable);
    }
}

std::vector<bool> findJitProcedureEligibility(const SCompiledScript &script) {
    std::vector<bool> eligible = findDirectProcedureEligibility(script);
    if (std::getenv("SJIT_DISABLE_PROCEDURE_REACHABILITY") != nullptr) {
        return eligible;
    }

    std::vector<bool> reachable(static_cast<size_t>(script.procedure_count), false);
    collectReachableProcedures(
        script,
        script.statements,
        script.statement_count,
        reachable);
    for (int i = 0; i < script.procedure_count; ++i) {
        eligible[static_cast<size_t>(i)] =
            eligible[static_cast<size_t>(i)] && reachable[static_cast<size_t>(i)];
    }
    return eligible;
}

bool findInterpreterOnlyExpr(
    const SExpr *expr,
    const std::string &path,
    std::string &reason) {
    if (!expr) {
        return false;
    }
    const OpcodeEffects opcodeEffects = sjit_expr_opcode_effects(expr->opcode);
    if (opcodeEffects.requiresInterpreter) {
        reason = "reporter " + std::string(sjit_expr_opcode_name(expr->opcode)) +
            " (opcode " + std::to_string(expr->opcode) + ") at " + path +
            " requires the interpreter";
        return true;
    }
    return findInterpreterOnlyExpr(expr->left, path + ".left", reason) ||
        findInterpreterOnlyExpr(expr->right, path + ".right", reason);
}

bool findInterpreterOnlyStatements(
    const SStatement *statements,
    int count,
    const std::string &path,
    std::string &reason) {
    if (count <= 0) {
        return false;
    }
    if (!statements) {
        reason = "invalid null statement array at " + path;
        return true;
    }
    for (int i = 0; i < count; ++i) {
        const SStatement &statement = statements[i];
        const std::string statementPath = path + "[" + std::to_string(i) + "]";
        const OpcodeEffects opcodeEffects = sjit_statement_opcode_effects(statement.opcode);
        if (opcodeEffects.requiresInterpreter) {
            reason = "statement " + std::string(sjit_statement_opcode_name(statement.opcode)) +
                " (opcode " + std::to_string(statement.opcode) + ") at " + statementPath +
                " requires the interpreter";
            return true;
        }
        if (findInterpreterOnlyExpr(statement.value, statementPath + ".value", reason) ||
            findInterpreterOnlyExpr(statement.index, statementPath + ".index", reason) ||
            findInterpreterOnlyExpr(statement.condition, statementPath + ".condition", reason) ||
            findInterpreterOnlyExpr(statement.times, statementPath + ".times", reason)) {
            return true;
        }
        for (int argument = 0; argument < statement.argument_count; ++argument) {
            if (findInterpreterOnlyExpr(
                    statement.arguments ? statement.arguments[argument].value : nullptr,
                    statementPath + ".arguments[" + std::to_string(argument) + "]",
                    reason)) {
                return true;
            }
        }
        if (findInterpreterOnlyStatements(
                statement.substack,
                statement.substack_count,
                statementPath + ".substack",
                reason) ||
            findInterpreterOnlyStatements(
                statement.substack2,
                statement.substack2_count,
                statementPath + ".substack2",
                reason)) {
            return true;
        }
    }
    return false;
}

bool statementsMaySuspendOrCallUnknown(const SStatement *statements, int count) {
    if (count <= 0) {
        return false;
    }
    if (!statements) {
        return true;
    }
    for (int i = 0; i < count; ++i) {
        const SStatement &statement = statements[i];
        const OpcodeEffects opcodeEffects = sjit_statement_opcode_effects(statement.opcode);
        if (opcodeEffects.canYield ||
            opcodeEffects.canCallUnknown ||
            opcodeEffects.requiresInterpreter ||
            statementsMaySuspendOrCallUnknown(statement.substack, statement.substack_count) ||
            statementsMaySuspendOrCallUnknown(statement.substack2, statement.substack2_count)) {
            return true;
        }
    }
    return false;
}

bool exprIsGuardReplaySafe(const SExpr *expr) {
    if (!expr) {
        return true;
    }
    /* A failed numeric type guard executes the generic call path.  Do not
       speculatively consume RNG state or sample the wall clock before that
       fallback re-evaluates the argument. */
    if (expr->opcode == SJIT_EXPR_RANDOM ||
        expr->opcode == SJIT_EXPR_DAYS_SINCE_2000) {
        return false;
    }
    return exprIsGuardReplaySafe(expr->left) &&
        exprIsGuardReplaySafe(expr->right);
}

std::string scriptInterpreterFallbackReason(const SCompiledScript &script) {
    std::string reason;
    if (findInterpreterOnlyStatements(
            script.statements,
            script.statement_count,
            "script.statements",
            reason)) {
        return reason;
    }
    return {};
}

std::string procedureInterpreterFallbackReason(
    const SCompiledScript &script,
    int procedureIndex) {
    if (procedureIndex < 0 || procedureIndex >= script.procedure_count) {
        return "invalid procedure index";
    }
    std::string reason;
    const SCompiledProcedure &procedure = script.procedures[procedureIndex];
    if (findInterpreterOnlyStatements(
            procedure.statements,
            procedure.statement_count,
            "script.procedures[" + std::to_string(procedureIndex) + "].statements",
            reason)) {
        return reason;
    }
    return {};
}

bool procedureSupportsStatelessWarpStatements(
    const SCompiledScript &script,
    const SStatement *statements,
    int count,
    const std::vector<bool> *eligibleProcedures) {
    if (count <= 0) {
        return true;
    }
    if (!statements) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        const SStatement &statement = statements[i];
        switch (statement.opcode) {
        case SJIT_STMT_FOREVER:
        case SJIT_STMT_FOR_EACH:
        case SJIT_STMT_REPEAT_UNTIL:
        case SJIT_STMT_WHILE:
            // These direct-JIT loops can yield at their safety guard and
            // therefore need the activation PC to resume instead of restart.
            return false;
        case SJIT_STMT_IF:
        case SJIT_STMT_REPEAT:
            if (!procedureSupportsStatelessWarpStatements(
                    script,
                    statement.substack,
                    statement.substack_count,
                    eligibleProcedures)) {
                return false;
            }
            break;
        case SJIT_STMT_IF_ELSE:
            if (!procedureSupportsStatelessWarpStatements(
                    script,
                    statement.substack,
                    statement.substack_count,
                    eligibleProcedures) ||
                !procedureSupportsStatelessWarpStatements(
                    script,
                    statement.substack2,
                    statement.substack2_count,
                    eligibleProcedures)) {
                return false;
            }
            break;
        case SJIT_STMT_PROCEDURE_CALL: {
            const int target = findProcedureIndex(script, statement.procedure_name);
            if (target >= 0 && eligibleProcedures &&
                (target >= (int)eligibleProcedures->size() ||
                 !(*eligibleProcedures)[static_cast<size_t>(target)])) {
                return false;
            }
            break;
        }
        default:
            break;
        }
    }
    return true;
}

std::vector<bool> findStatelessWarpProcedureEligibility(
    const SCompiledScript &script,
    const std::vector<bool> &directProcedures) {
    std::vector<bool> eligible(static_cast<size_t>(script.procedure_count), false);
    for (int i = 0; i < script.procedure_count; ++i) {
        const SCompiledProcedure &procedure = script.procedures[i];
        eligible[static_cast<size_t>(i)] = directProcedures[static_cast<size_t>(i)] &&
            procedure.warp_mode && procedureSupportsStatelessWarpStatements(
                script,
                procedure.statements,
                procedure.statement_count,
                nullptr);
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < script.procedure_count; ++i) {
            if (!eligible[static_cast<size_t>(i)]) {
                continue;
            }
            const SCompiledProcedure &procedure = script.procedures[i];
            if (!procedureSupportsStatelessWarpStatements(
                    script,
                    procedure.statements,
                    procedure.statement_count,
                    &eligible)) {
                eligible[static_cast<size_t>(i)] = false;
                changed = true;
            }
        }
    }
    return eligible;
}

ModuleBuild createScriptModule(const llvm::DataLayout &dataLayout, const SCompiledScript &script, const std::string &name) {
    if (const std::string reason = scriptInterpreterFallbackReason(script); !reason.empty()) {
        throw std::runtime_error("refusing native lowering: " + reason);
    }
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>(name + "_module", *context);
    module->setDataLayout(dataLayout);

    auto *i32 = llvm::Type::getInt32Ty(*context);
    auto *i1 = llvm::Type::getInt1Ty(*context);
    auto *i64 = llvm::Type::getInt64Ty(*context);
    auto *f64 = llvm::Type::getDoubleTy(*context);
    auto *voidTy = llvm::Type::getVoidTy(*context);
    auto *ptr = llvm::PointerType::getUnqual(*context);
    auto *svalueTy = llvm::StructType::create(*context, {i32, f64, ptr}, "SValue.jit");
    auto *stringTy = llvm::StructType::create(
        *context,
        {ptr, i32, i32, i32, i32, i32, f64},
        "SString.jit");
    auto *listStorageTy = llvm::StructType::create(
        *context,
        {ptr, i32, i32, i32, ptr, i32},
        "SListStorage.jit");
    auto *listTy = llvm::StructType::create(*context, {ptr}, "SList.jit");
    auto *loopStateTy = llvm::StructType::create(*context, {ptr, i32, f64, i32, i32}, "SLoopState.jit");
    auto *frameTy = llvm::StructType::create(*context, "SFrame.jit");
    frameTy->setBody({
        i32,
        i32,
        i32,
        i32,
        i32,
        f64,
        i32,
        llvm::ArrayType::get(svalueTy, SJIT_MAX_LOCALS),
        llvm::ArrayType::get(svalueTy, SJIT_MAX_STACK),
        i32,
        llvm::ArrayType::get(svalueTy, SJIT_MAX_PARAMS),
        i32,
        llvm::ArrayType::get(loopStateTy, SJIT_MAX_LOOP_STATES),
        i32,
        i32,
        i32,
        i32,
        i32,
    });
    auto *threadTy = llvm::StructType::create(
        *context,
        {i32, i32, i32, i32, i32, ptr, ptr, frameTy},
        "SThread.jit");
    auto *keyStateTy = llvm::ArrayType::get(i32, 256);
    auto *inputTy = llvm::StructType::create(
        *context,
        {
            f64,
            f64,
            f64,
            f64,
            i32,
            keyStateTy,
            keyStateTy,
            i32,
            i32,
            i32,
        },
        "SHostInputSnapshot.jit");
    auto *drawBufferTy = llvm::StructType::create(*context, {ptr, i32, i32}, "SDrawCommandBuffer.jit");
    auto *targetTy = llvm::StructType::create(
        *context,
        {i32, i32, i32, ptr, ptr, i32, i32},
        "STarget.jit");
    auto *spriteTy = llvm::StructType::create(
        *context,
        {
            targetTy,
            i32,
            i32,
            f64,
            f64,
            f64,
            f64,
            i32,
            i32,
            i32,
            i32,
            f64,
            i32,
            i32,
            f64,
            i32,
            i32,
            i32,
            i32,
            f64,
            f64,
            f64,
            f64,
        },
        "SSprite.jit");
    auto *runtimeTy = llvm::StructType::create(
        *context,
        {
            inputTy,
            f64,
            f64,
            ptr,
            i32,
            i32,
            i32,
            i32,
            ptr,
            i32,
            i32,
            i32,
            ptr,
            i32,
            i32,
            drawBufferTy,
            drawBufferTy,
            i32,
            f64,
            f64,
            i32,
            i32,
            i32,
            i64,
        },
        "SRuntime.jit");
    auto *variableTy = llvm::StructType::create(
        *context,
        {i32, ptr, i32, i32, i32, svalueTy, ptr},
        "SVariable.jit");
    auto *exprTy = llvm::StructType::create(
        *context,
        {i32, svalueTy, ptr, ptr, i32, f64, ptr, i32, i32, i32, i32},
        "SExpr.jit");
    auto *statementTy = llvm::StructType::create(
        *context,
        {
            i32,
            ptr,
            ptr,
            ptr,
            ptr,
            ptr,
            ptr,
            ptr,
            i32,
            ptr,
            i32,
            ptr,
            i32,
            ptr,
            i32,
            i32,
            i32,
            i32,
            i32,
            i32,
            i32,
            i32,
            i32,
            i32,
            i32,
            i32,
            i32,
            ptr,
            ptr,
        },
        "SStatement.jit");

    auto *entryTy = llvm::FunctionType::get(i32, {ptr, ptr, ptr}, false);
    auto *procedureStatementTy = llvm::FunctionType::get(i32, {ptr, ptr, ptr, ptr, ptr}, false);
    auto *genericStatementTy = procedureStatementTy;
    auto *statementNumberTy = llvm::FunctionType::get(f64, {ptr, ptr, ptr, i32}, false);
    auto *statementBoolTy = llvm::FunctionType::get(i32, {ptr, ptr, ptr, i32}, false);
    auto *valuePtrToNumberTy = llvm::FunctionType::get(f64, {ptr, ptr}, false);
    auto *valueDestroyPtrTy = llvm::FunctionType::get(voidTy, {ptr}, false);
    auto *runtimeGetSpriteTy = llvm::FunctionType::get(ptr, {ptr, i32}, false);
    auto *runtimeOnlyStatementTy = llvm::FunctionType::get(voidTy, {ptr}, false);
    auto *statementActionTy = llvm::FunctionType::get(voidTy, {ptr, ptr, ptr}, false);
    auto *genericStatement = llvm::Function::Create(
        genericStatementTy,
        llvm::Function::ExternalLinkage,
        "sjit_script_execute_statement_ptr_with_thread",
        module.get());
    auto *statementVariablePtrTy = llvm::FunctionType::get(ptr, {ptr, ptr, ptr}, false);
    auto *exprVariableTy = llvm::FunctionType::get(ptr, {ptr, i32, ptr}, false);
    auto *variableIsNumberTy = llvm::FunctionType::get(i32, {ptr}, false);
    auto *variableNumberTy = llvm::FunctionType::get(f64, {ptr, ptr}, false);
    auto *variableValueTy = llvm::FunctionType::get(voidTy, {ptr, ptr}, false);
    auto *variableTruthyTy = llvm::FunctionType::get(i32, {ptr, ptr}, false);
    auto *variableSetNumberTy = llvm::FunctionType::get(voidTy, {ptr, f64}, false);
    auto *variableSetStringBorrowedTy = llvm::FunctionType::get(voidTy, {ptr, ptr, ptr}, false);
    auto *exprVariableNumberTy = llvm::FunctionType::get(f64, {ptr, i32, ptr}, false);
    auto *listVariableLengthNumberTy = llvm::FunctionType::get(f64, {ptr}, false);
    auto *listVariableItemNumberAtTy = llvm::FunctionType::get(f64, {ptr, ptr, f64}, false);
    auto *listVariableItemNumberAtVariableTy = llvm::FunctionType::get(f64, {ptr, ptr, ptr}, false);
    auto *listVariableItemNumberAtArgumentTy = llvm::FunctionType::get(
        f64,
        {ptr, ptr, ptr, ptr, i32},
        false);
    auto *listVariableItemValueTy = llvm::FunctionType::get(voidTy, {ptr, ptr, ptr, ptr}, false);
    auto *listVariableItemValueAtNumberTy = llvm::FunctionType::get(
        voidTy,
        {ptr, ptr, ptr, f64, ptr},
        false);
    auto *variableSetFromListItemAtNumberTy = llvm::FunctionType::get(
        voidTy,
        {ptr, i32, ptr, ptr, f64},
        false);
    auto *listVariableItemNumberValueTy = llvm::FunctionType::get(f64, {ptr, ptr, ptr}, false);
    auto *listVariableContainsValueTy = llvm::FunctionType::get(i32, {ptr, ptr, ptr}, false);
    auto *numberUnaryTy = llvm::FunctionType::get(f64, {f64}, false);
    auto *numberBinaryTy = llvm::FunctionType::get(f64, {f64, f64}, false);
    auto *mathopNumberTy = llvm::FunctionType::get(f64, {ptr, ptr, f64}, false);
    auto *numberNullaryTy = llvm::FunctionType::get(f64, {}, false);
    auto *exprLiteralValueTy = llvm::FunctionType::get(voidTy, {ptr, ptr}, false);
    auto *variableArgumentValueTy = llvm::FunctionType::get(f64, {ptr, ptr, ptr}, false);
    auto *procedureArgumentCopyTy = llvm::FunctionType::get(f64, {ptr, ptr, i32, ptr}, false);
    auto *listItemArgumentLiteralTy = llvm::FunctionType::get(f64, {ptr, i32, ptr, ptr}, false);
    auto *listItemArgumentAtVariableTy = llvm::FunctionType::get(f64, {ptr, i32, ptr, ptr, ptr}, false);
    auto *procedureArgumentValueAtTy = llvm::FunctionType::get(voidTy, {ptr, ptr, i32, ptr}, false);
    auto *valueJoinPtrTy = llvm::FunctionType::get(voidTy, {ptr, ptr, ptr, ptr}, false);
    auto *exprVariableValueTy = llvm::FunctionType::get(voidTy, {ptr, i32, ptr, ptr}, false);
    auto *exprListItemValueTy = llvm::FunctionType::get(voidTy, {ptr, i32, ptr, ptr, ptr}, false);
    auto *exprListItemNumberValueTy = llvm::FunctionType::get(f64, {ptr, i32, ptr, ptr}, false);
    auto *exprListContainsValueTy = llvm::FunctionType::get(i32, {ptr, i32, ptr, ptr}, false);
    auto *valueLengthNumberTy = llvm::FunctionType::get(f64, {ptr, ptr}, false);
    auto *valueLetterOfTy = llvm::FunctionType::get(voidTy, {ptr, ptr, ptr, ptr}, false);
    auto *keyPressedValueTy = llvm::FunctionType::get(i32, {ptr, ptr}, false);
    auto *valueTruthyTy = llvm::FunctionType::get(i32, {ptr, ptr}, false);
    auto *valueCompareTy = llvm::FunctionType::get(i32, {ptr, ptr, ptr, i32}, false);
    auto *valueContainsTy = llvm::FunctionType::get(i32, {ptr, ptr, ptr}, false);
    auto *listItemCompareLiteralAtVariableTy = llvm::FunctionType::get(i32, {ptr, i32, ptr, ptr, ptr, i32}, false);
    auto *statementNumberValueTy = llvm::FunctionType::get(voidTy, {ptr, ptr, ptr, f64}, false);
    auto *variableSetFromVariableTy = llvm::FunctionType::get(voidTy, {ptr, ptr, ptr}, false);
    auto *variableSetFromArgumentTy = llvm::FunctionType::get(voidTy, {ptr, ptr, ptr, ptr, i32}, false);
    auto *variableSetFromListItemLiteralTy = llvm::FunctionType::get(voidTy, {ptr, i32, ptr, ptr}, false);
    auto *variableSetFromListItemAtVariableTy = llvm::FunctionType::get(
        voidTy,
        {ptr, i32, ptr, ptr, ptr},
        false);
    auto *variableSetFromListItemAtArgumentTy = llvm::FunctionType::get(
        voidTy,
        {ptr, i32, ptr, ptr, ptr, ptr, i32},
        false);
    auto *statementListAddFromVariableTy = llvm::FunctionType::get(voidTy, {ptr, ptr, ptr, ptr}, false);
    auto *statementListAddFromArgumentTy = llvm::FunctionType::get(
        voidTy,
        {ptr, ptr, ptr, ptr, ptr, i32},
        false);
    auto *statementListAddListItemAtVariableTy = llvm::FunctionType::get(
        voidTy,
        {ptr, ptr, ptr, ptr, ptr},
        false);
    auto *statementListAddLiteralRepeatedTy = llvm::FunctionType::get(i32, {ptr, ptr, ptr, f64}, false);
    auto *statementListDeleteAtTy = llvm::FunctionType::get(voidTy, {ptr, ptr, ptr, f64}, false);
    auto *statementListReplaceNumberAtTy = llvm::FunctionType::get(voidTy, {ptr, ptr, ptr, f64, f64}, false);
    auto *listVariableAddNumberTy = llvm::FunctionType::get(voidTy, {ptr, f64}, false);
    auto *listVariableClearTy = llvm::FunctionType::get(voidTy, {ptr}, false);
    auto *listVariableReplaceNumberAtTy = llvm::FunctionType::get(voidTy, {ptr, ptr, f64, f64}, false);
    auto *listVariableReplaceNumberAtVariableTy = llvm::FunctionType::get(voidTy, {ptr, ptr, ptr, f64}, false);
    auto *listVariableReplaceFromVariablesTy = llvm::FunctionType::get(voidTy, {ptr, ptr, ptr, ptr}, false);
    auto *listVariableReplaceListItemAtVariablesTy = llvm::FunctionType::get(
        voidTy,
        {ptr, ptr, ptr, ptr, ptr},
        false);
    auto *statementValuePtrTy = llvm::FunctionType::get(voidTy, {ptr, ptr, ptr, ptr}, false);
    auto *statementTwoValuePtrTy = llvm::FunctionType::get(voidTy, {ptr, ptr, ptr, ptr, ptr}, false);
    auto *runtimeValuePtrTy = llvm::FunctionType::get(voidTy, {ptr, ptr}, false);
    auto *scriptValuePtrTy = llvm::FunctionType::get(voidTy, {ptr, ptr, ptr}, false);
    auto *scriptTwoValuePtrTy = llvm::FunctionType::get(voidTy, {ptr, ptr, ptr, ptr}, false);
    auto *spriteStatementTy = llvm::FunctionType::get(voidTy, {ptr, ptr}, false);
    auto *penColorChangePairTy = llvm::FunctionType::get(voidTy, {ptr, ptr, ptr, i32, f64}, false);
    auto *penColorListChangeAtVariableTy = llvm::FunctionType::get(voidTy, {ptr, ptr, ptr, ptr, i32, f64}, false);
    auto *listReplaceLiteralAtVariableTy = llvm::FunctionType::get(voidTy, {ptr, ptr, ptr, ptr}, false);
    auto *listReplaceListItemAtVariableNumberTy = llvm::FunctionType::get(
        voidTy,
        {ptr, ptr, ptr, ptr, ptr, f64},
        false);
    auto *penRenderListPixelFromVariablesTy = llvm::FunctionType::get(
        voidTy,
        {ptr, ptr, ptr, ptr, ptr, ptr, i32},
        false);
    auto *penRenderRowFromVariablesTy = llvm::FunctionType::get(
        i32,
        {ptr, ptr, ptr, ptr, ptr, ptr, ptr, f64, f64, ptr, i32},
        false);
    auto *spriteSetXyTy = llvm::FunctionType::get(voidTy, {ptr, ptr, f64, f64, i32}, false);
    auto *spriteNumberGetterTy = llvm::FunctionType::get(f64, {ptr}, false);
    auto *spriteSetSizeTy = llvm::FunctionType::get(voidTy, {ptr, ptr, f64}, false);
    auto *penSetSizeNumberTy = llvm::FunctionType::get(voidTy, {ptr, ptr, f64}, false);
    auto *spriteEffectNumberTy = llvm::FunctionType::get(
        voidTy,
        {ptr, ptr, i32, f64},
        false);
    auto *spriteIntSetterTy = llvm::FunctionType::get(voidTy, {ptr, i32}, false);
    auto *controlLoopStateTy = llvm::FunctionType::get(ptr, {ptr, ptr, i32}, false);
    auto *controlLoopResetTy = llvm::FunctionType::get(voidTy, {ptr, ptr}, false);
    auto *procedureControlLoopStateTy = llvm::FunctionType::get(ptr, {ptr, ptr, i32, i32}, false);
    auto *procedureControlLoopResetTy = llvm::FunctionType::get(voidTy, {ptr, ptr, i32}, false);
    auto *procedureActivationStateTy = llvm::FunctionType::get(ptr, {ptr, ptr, i32, i32}, false);
    auto *procedureActivationResetTy = llvm::FunctionType::get(voidTy, {ptr, ptr, i32}, false);
    auto *procedureScopeResetTy = llvm::FunctionType::get(voidTy, {ptr, i32}, false);
    auto *repeatShouldEnterTy = llvm::FunctionType::get(i32, {ptr, ptr, f64}, false);
    auto *procedureRepeatShouldEnterTy = llvm::FunctionType::get(i32, {ptr, ptr, i32, f64}, false);
    auto *repeatTakeAllTy = llvm::FunctionType::get(f64, {ptr, ptr, f64}, false);
    auto *finishControlBranchTy = llvm::FunctionType::get(i32, {ptr, ptr}, false);
    auto *finishBatchedLoopBranchTy = llvm::FunctionType::get(i32, {ptr, ptr, ptr, i32}, false);
    auto *repeatRoundTy = llvm::FunctionType::get(f64, {f64}, false);
    auto *procedureTy = llvm::FunctionType::get(i32, {ptr, ptr, ptr, ptr, i32, ptr, ptr}, false);

    auto *scriptInterpreter = llvm::Function::Create(
        entryTy,
        llvm::Function::ExternalLinkage,
        "sjit_script_interpreter_entry",
        module.get());

    auto *statementNumber = llvm::Function::Create(
        statementNumberTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_number",
        module.get());
    auto *statementBool = llvm::Function::Create(
        statementBoolTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_bool",
        module.get());
    auto *valuePtrToNumber = llvm::Function::Create(
        valuePtrToNumberTy,
        llvm::Function::ExternalLinkage,
        "sjit_script_value_ptr_to_number",
        module.get());
    auto *valueDestroyPtr = llvm::Function::Create(
        valueDestroyPtrTy,
        llvm::Function::ExternalLinkage,
        "sjit_script_destroy_value_ptr",
        module.get());
    auto *runtimeGetSprite = llvm::Function::Create(
        runtimeGetSpriteTy,
        llvm::Function::ExternalLinkage,
        "sjit_runtime_get_sprite",
        module.get());
    auto *resetTimer = llvm::Function::Create(
        runtimeOnlyStatementTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_reset_timer",
        module.get());
    auto *penClear = llvm::Function::Create(
        runtimeOnlyStatementTy,
        llvm::Function::ExternalLinkage,
        "sjit_pen_clear",
        module.get());
    auto *penDown = llvm::Function::Create(
        spriteStatementTy,
        llvm::Function::ExternalLinkage,
        "sjit_pen_down",
        module.get());
    auto *penUp = llvm::Function::Create(
        spriteStatementTy,
        llvm::Function::ExternalLinkage,
        "sjit_pen_up",
        module.get());
    auto *penStamp = llvm::Function::Create(
        spriteStatementTy,
        llvm::Function::ExternalLinkage,
        "sjit_pen_stamp",
        module.get());
    auto *looksShow = llvm::Function::Create(
        spriteStatementTy,
        llvm::Function::ExternalLinkage,
        "sjit_looks_show",
        module.get());
    auto *looksHide = llvm::Function::Create(
        spriteStatementTy,
        llvm::Function::ExternalLinkage,
        "sjit_looks_hide",
        module.get());
    auto *looksSwitchBackdropValuePtr = llvm::Function::Create(
        runtimeValuePtrTy,
        llvm::Function::ExternalLinkage,
        "sjit_looks_switch_backdrop_value_ptr",
        module.get());
    auto *looksSetEffectNumber = llvm::Function::Create(
        spriteEffectNumberTy,
        llvm::Function::ExternalLinkage,
        "sjit_looks_set_effect_number",
        module.get());
    auto *looksChangeEffectNumber = llvm::Function::Create(
        spriteEffectNumberTy,
        llvm::Function::ExternalLinkage,
        "sjit_looks_change_effect_number",
        module.get());
    auto *looksClearEffects = llvm::Function::Create(
        spriteStatementTy,
        llvm::Function::ExternalLinkage,
        "sjit_looks_clear_effects",
        module.get());
    auto *spriteSetDraggable = llvm::Function::Create(
        spriteIntSetterTy,
        llvm::Function::ExternalLinkage,
        "sjit_sprite_set_draggable",
        module.get());
    auto *spriteSetXy = llvm::Function::Create(
        spriteSetXyTy,
        llvm::Function::ExternalLinkage,
        "sjit_sprite_set_xy",
        module.get());
    auto *spriteX = llvm::Function::Create(
        spriteNumberGetterTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_sprite_x",
        module.get());
    auto *spriteY = llvm::Function::Create(
        spriteNumberGetterTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_sprite_y",
        module.get());
    auto *spriteSetSize = llvm::Function::Create(
        spriteSetSizeTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_sprite_set_size",
        module.get());
    auto *penSetSizeNumber = llvm::Function::Create(
        penSetSizeNumberTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_pen_set_size_number",
        module.get());
    auto *statementSetVariableAction = llvm::Function::Create(
        statementActionTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_set_variable",
        module.get());
    auto *statementChangeVariableAction = llvm::Function::Create(
        statementActionTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_change_variable",
        module.get());
    auto *statementMonitorVisibility = llvm::Function::Create(
        statementActionTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_set_monitor_visibility",
        module.get());
    auto *statementListAdd = llvm::Function::Create(
        statementActionTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_list_add",
        module.get());
    auto *statementListDelete = llvm::Function::Create(
        statementActionTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_list_delete",
        module.get());
    auto *statementListDeleteAll = llvm::Function::Create(
        statementActionTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_list_delete_all",
        module.get());
    auto *statementListInsert = llvm::Function::Create(
        statementActionTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_list_insert",
        module.get());
    auto *statementListReplace = llvm::Function::Create(
        statementActionTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_list_replace",
        module.get());
    auto *statementSay = llvm::Function::Create(
        statementActionTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_say",
        module.get());
    auto *statementPenSetColor = llvm::Function::Create(
        statementActionTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_pen_set_color",
        module.get());
    auto *statementPenChangeColorParam = llvm::Function::Create(
        statementActionTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_pen_change_color_param",
        module.get());
    auto *statementScalarVariablePtr = llvm::Function::Create(
        statementVariablePtrTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_scalar_variable_ptr",
        module.get());
    auto *statementListVariablePtr = llvm::Function::Create(
        statementVariablePtrTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_list_variable_ptr",
        module.get());
    auto *exprScalarVariable = llvm::Function::Create(
        exprVariableTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_expr_scalar_variable",
        module.get());
    auto *exprListVariable = llvm::Function::Create(
        exprVariableTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_expr_list_variable",
        module.get());
    auto *variableIsNumber = llvm::Function::Create(
        variableIsNumberTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_variable_is_number",
        module.get());
    auto *variableNumber = llvm::Function::Create(
        variableNumberTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_variable_number",
        module.get());
    auto *variableValue = llvm::Function::Create(
        variableValueTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_variable_value",
        module.get());
    auto *variableTruthy = llvm::Function::Create(
        variableTruthyTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_variable_truthy",
        module.get());
    auto *variableSetNumber = llvm::Function::Create(
        variableSetNumberTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_variable_set_number",
        module.get());
    auto *variableSetStringBorrowed = llvm::Function::Create(
        variableSetStringBorrowedTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_variable_set_string_borrowed",
        module.get());
    auto *exprVariableNumber = llvm::Function::Create(
        exprVariableNumberTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_expr_variable_number",
        module.get());
    auto *listVariableLengthNumber = llvm::Function::Create(
        listVariableLengthNumberTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_list_variable_length_number",
        module.get());
    auto *listVariableItemNumberAt = llvm::Function::Create(
        listVariableItemNumberAtTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_list_variable_item_number_at",
        module.get());
    auto *listVariableItemNumberAtVariable = llvm::Function::Create(
        listVariableItemNumberAtVariableTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_list_variable_item_number_at_variable",
        module.get());
    auto *listVariableItemNumberAtArgument = llvm::Function::Create(
        listVariableItemNumberAtArgumentTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_list_variable_item_number_at_argument",
        module.get());
    auto *listVariableItemValue = llvm::Function::Create(
        listVariableItemValueTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_list_variable_item_value",
        module.get());
    auto *listVariableItemValueAtNumber = llvm::Function::Create(
        llvm::FunctionType::get(voidTy, {ptr, ptr, f64, ptr}, false),
        llvm::Function::ExternalLinkage,
        "sjit_jit_list_variable_item_value_at_number",
        module.get());
    auto *listVariableItemNumberValue = llvm::Function::Create(
        listVariableItemNumberValueTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_list_variable_item_number_value",
        module.get());
    auto *listVariableContainsValue = llvm::Function::Create(
        listVariableContainsValueTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_list_variable_contains_value",
        module.get());
    auto *daysSince2000 = llvm::Function::Create(
        numberNullaryTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_days_since_2000",
        module.get());
    auto *roundNumber = llvm::Function::Create(
        numberUnaryTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_round_number",
        module.get());
    auto *randomNumber = llvm::Function::Create(
        numberBinaryTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_random_number",
        module.get());
    auto *mathopNumber = llvm::Function::Create(
        mathopNumberTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_mathop_number",
        module.get());
    auto *exprLiteralValue = llvm::Function::Create(
        exprLiteralValueTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_expr_literal_value",
        module.get());
    auto *variableArgumentValue = llvm::Function::Create(
        variableArgumentValueTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_variable_argument_value",
        module.get());
    auto *procedureArgumentCopy = llvm::Function::Create(
        procedureArgumentCopyTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_procedure_argument_copy",
        module.get());
    auto *listItemArgumentLiteral = llvm::Function::Create(
        listItemArgumentLiteralTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_expr_list_item_argument_literal",
        module.get());
    auto *listItemArgumentAtVariable = llvm::Function::Create(
        listItemArgumentAtVariableTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_expr_list_item_argument_at_variable",
        module.get());
    auto *procedureArgumentValueAt = llvm::Function::Create(
        procedureArgumentValueAtTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_procedure_argument_value_at",
        module.get());
    auto *valueJoinPtr = llvm::Function::Create(
        valueJoinPtrTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_value_join_ptr",
        module.get());
    auto *exprVariableValue = llvm::Function::Create(
        exprVariableValueTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_expr_variable_value",
        module.get());
    auto *exprListItemValue = llvm::Function::Create(
        exprListItemValueTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_expr_list_item_value",
        module.get());
    auto *exprListItemNumberValue = llvm::Function::Create(
        exprListItemNumberValueTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_expr_list_item_number_value",
        module.get());
    auto *exprListContainsValue = llvm::Function::Create(
        exprListContainsValueTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_expr_list_contains_value",
        module.get());
    auto *valueLengthNumber = llvm::Function::Create(
        valueLengthNumberTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_value_length_number",
        module.get());
    auto *valueLetterOf = llvm::Function::Create(
        valueLetterOfTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_value_letter_of",
        module.get());
    auto *keyPressedValue = llvm::Function::Create(
        keyPressedValueTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_key_pressed_value",
        module.get());
    auto *valueTruthy = llvm::Function::Create(
        valueTruthyTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_value_truthy",
        module.get());
    auto *valueCompare = llvm::Function::Create(
        valueCompareTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_value_compare",
        module.get());
    auto *valueContains = llvm::Function::Create(
        valueContainsTy,
        llvm::Function::ExternalLinkage,
        "sjit_op_contains_value_ptr",
        module.get());
    auto *listItemCompareLiteralAtVariable = llvm::Function::Create(
        listItemCompareLiteralAtVariableTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_expr_list_item_compare_literal_at_variable",
        module.get());
    auto *penColorChangePair = llvm::Function::Create(
        penColorChangePairTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_pen_set_color_value_and_change_param_number_ptr",
        module.get());
    auto *penColorListChangeAtVariable = llvm::Function::Create(
        penColorListChangeAtVariableTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_pen_set_color_list_item_and_change_param_number_at_variable",
        module.get());
    auto *listReplaceLiteralAtVariable = llvm::Function::Create(
        listReplaceLiteralAtVariableTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_list_replace_literal_at_variable",
        module.get());
    auto *listReplaceListItemAtVariableNumber = llvm::Function::Create(
        listReplaceListItemAtVariableNumberTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_list_replace_list_item_at_variable_number",
        module.get());
    auto *penRenderListPixelFromVariables = llvm::Function::Create(
        penRenderListPixelFromVariablesTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_pen_render_list_pixel_from_variables",
        module.get());
    auto *penRenderRowFromVariables = llvm::Function::Create(
        penRenderRowFromVariablesTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_pen_render_row_from_variables",
        module.get());
    auto *statementSetVariableNumber = llvm::Function::Create(
        statementNumberValueTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_set_variable_number",
        module.get());
    auto *variableSetFromVariable = llvm::Function::Create(
        variableSetFromVariableTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_variable_set_from_variable",
        module.get());
    auto *variableSetFromArgument = llvm::Function::Create(
        variableSetFromArgumentTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_variable_set_from_argument",
        module.get());
    auto *variableSetFromLiteral = llvm::Function::Create(
        variableSetFromVariableTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_variable_set_from_literal",
        module.get());
    auto *variableSetFromListItemLiteral = llvm::Function::Create(
        variableSetFromListItemLiteralTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_variable_set_from_list_item_literal",
        module.get());
    auto *variableSetFromListItemAtVariable = llvm::Function::Create(
        variableSetFromListItemAtVariableTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_variable_set_from_list_item_at_variable",
        module.get());
    auto *variableSetFromListItemAtNumber = llvm::Function::Create(
        variableSetFromListItemAtNumberTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_variable_set_from_list_item_at_number",
        module.get());
    auto *variableSetFromListItemAtArgument = llvm::Function::Create(
        variableSetFromListItemAtArgumentTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_variable_set_from_list_item_at_argument",
        module.get());
    auto *statementChangeVariableNumber = llvm::Function::Create(
        statementNumberValueTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_change_variable_number",
        module.get());
    auto *statementListAddFromVariable = llvm::Function::Create(
        statementListAddFromVariableTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_list_add_from_variable",
        module.get());
    auto *statementListAddFromArgument = llvm::Function::Create(
        statementListAddFromArgumentTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_list_add_from_argument",
        module.get());
    auto *statementListAddFromLiteral = llvm::Function::Create(
        statementActionTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_list_add_from_literal",
        module.get());
    auto *statementListAddListItemAtVariable = llvm::Function::Create(
        statementListAddListItemAtVariableTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_list_add_list_item_at_variable",
        module.get());
    auto *statementListAddLiteralRepeated = llvm::Function::Create(
        statementListAddLiteralRepeatedTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_list_add_literal_repeated",
        module.get());
    auto *statementListDeleteAtNumber = llvm::Function::Create(
        statementListDeleteAtTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_list_delete_at_number",
        module.get());
    auto *statementListInsertNumberAt = llvm::Function::Create(
        statementListReplaceNumberAtTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_list_insert_number_at",
        module.get());
    auto *listVariableAddNumber = llvm::Function::Create(
        listVariableAddNumberTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_list_variable_add_number",
        module.get());
    auto *listVariableClear = llvm::Function::Create(
        listVariableClearTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_list_variable_clear",
        module.get());
    auto *listVariableReplaceNumberAt = llvm::Function::Create(
        listVariableReplaceNumberAtTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_list_variable_replace_number_at",
        module.get());
    auto *listVariableReplaceNumberAtVariable = llvm::Function::Create(
        listVariableReplaceNumberAtVariableTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_list_variable_replace_number_at_variable",
        module.get());
    auto *listVariableReplaceFromVariables = llvm::Function::Create(
        listVariableReplaceFromVariablesTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_list_variable_replace_from_variables",
        module.get());
    auto *listVariableReplaceListItemAtVariables = llvm::Function::Create(
        listVariableReplaceListItemAtVariablesTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_list_variable_replace_list_item_at_variables",
        module.get());
    auto *statementSetVariableValuePtr = llvm::Function::Create(
        statementValuePtrTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_set_variable_value_ptr",
        module.get());
    auto *statementListAddValuePtr = llvm::Function::Create(
        statementValuePtrTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_list_add_value_ptr",
        module.get());
    auto *statementListDeleteIndexValuePtr = llvm::Function::Create(
        statementValuePtrTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_list_delete_index_value_ptr",
        module.get());
    auto *statementListInsertValuePtr = llvm::Function::Create(
        statementTwoValuePtrTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_list_insert_value_ptr",
        module.get());
    auto *statementListReplaceValuePtr = llvm::Function::Create(
        statementTwoValuePtrTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_list_replace_value_ptr",
        module.get());
    auto *statementSayValuePtr = llvm::Function::Create(
        runtimeValuePtrTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_say_value_ptr",
        module.get());
    auto *statementPenSetColorValuePtr = llvm::Function::Create(
        scriptValuePtrTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_pen_set_color_value_ptr",
        module.get());
    auto *statementPenChangeColorParamValuePtr = llvm::Function::Create(
        scriptTwoValuePtrTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_statement_pen_change_color_param_value_ptr",
        module.get());
    auto *scriptExecuteProcedureStatement = llvm::Function::Create(
        procedureStatementTy,
        llvm::Function::ExternalLinkage,
        "sjit_script_execute_procedure_statement",
        module.get());
    auto *controlLoopState = llvm::Function::Create(
        controlLoopStateTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_control_loop_state",
        module.get());
    auto *controlLoopReset = llvm::Function::Create(
        controlLoopResetTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_control_loop_reset",
        module.get());
    auto *procedureControlLoopState = llvm::Function::Create(
        procedureControlLoopStateTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_procedure_control_loop_state",
        module.get());
    auto *procedureControlLoopReset = llvm::Function::Create(
        procedureControlLoopResetTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_procedure_control_loop_reset",
        module.get());
    auto *procedureActivationState = llvm::Function::Create(
        procedureActivationStateTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_procedure_activation_state",
        module.get());
    auto *procedureActivationReset = llvm::Function::Create(
        procedureActivationResetTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_procedure_activation_reset",
        module.get());
    auto *procedureScopeReset = llvm::Function::Create(
        procedureScopeResetTy,
        llvm::Function::ExternalLinkage,
        "sjit_control_loop_reset_from_depth",
        module.get());
    auto *repeatShouldEnterNumber = llvm::Function::Create(
        repeatShouldEnterTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_repeat_should_enter_number",
        module.get());
    auto *procedureRepeatShouldEnterNumber = llvm::Function::Create(
        procedureRepeatShouldEnterTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_procedure_repeat_should_enter_number",
        module.get());
    auto *repeatRemainingNumber = llvm::Function::Create(
        repeatTakeAllTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_repeat_remaining_number",
        module.get());
    auto *repeatTakeAllNumber = llvm::Function::Create(
        repeatTakeAllTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_repeat_take_all_number",
        module.get());
    auto *finishControlBranch = llvm::Function::Create(
        finishControlBranchTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_finish_control_branch",
        module.get());
    auto *finishBatchedLoopBranch = llvm::Function::Create(
        finishBatchedLoopBranchTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_finish_batched_loop_branch",
        module.get());
    auto *roundRepeatCount = llvm::Function::Create(
        repeatRoundTy,
        llvm::Function::ExternalLinkage,
        "sjit_jit_round_repeat_count",
        module.get());

    auto *fastNumericTextCompareTy = llvm::FunctionType::get(
        i32,
        {ptr, ptr, ptr, f64, i32, i1},
        false);
    auto *fastNumericTextCompare = llvm::Function::Create(
        fastNumericTextCompareTy,
        llvm::Function::InternalLinkage,
        name + "_fast_numeric_text_compare",
        module.get());
    {
        auto arg = fastNumericTextCompare->arg_begin();
        llvm::Value *fastRuntime = &*arg++;
        llvm::Value *dynamicValue = &*arg++;
        llvm::Value *literalValue = &*arg++;
        llvm::Value *literalNumber = &*arg++;
        llvm::Value *opcode = &*arg++;
        llvm::Value *dynamicIsLeft = &*arg++;
        auto *entry = llvm::BasicBlock::Create(*context, "entry", fastNumericTextCompare);
        auto *checkNumber = llvm::BasicBlock::Create(*context, "check_number", fastNumericTextCompare);
        auto *checkString = llvm::BasicBlock::Create(*context, "check_string", fastNumericTextCompare);
        auto *cachedString = llvm::BasicBlock::Create(*context, "cached_string", fastNumericTextCompare);
        auto *direct = llvm::BasicBlock::Create(*context, "direct_number", fastNumericTextCompare);
        auto *fallback = llvm::BasicBlock::Create(*context, "fallback", fastNumericTextCompare);
        llvm::IRBuilder<> fastBuilder(entry);
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                fastBuilder.CreateNot(isNull(fastBuilder, dynamicValue)),
                fastBuilder.CreateNot(isNull(fastBuilder, literalValue))),
            checkNumber,
            fallback);

        fastBuilder.SetInsertPoint(checkNumber);
        llvm::Value *dynamicTag = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(svalueTy, dynamicValue, 0));
        llvm::Value *dynamicNumber = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(svalueTy, dynamicValue, 1));
        llvm::Value *isUsableNumber = fastBuilder.CreateAnd(
            fastBuilder.CreateICmpEQ(
                dynamicTag,
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)),
            fastBuilder.CreateFCmpORD(dynamicNumber, dynamicNumber));
        fastBuilder.CreateCondBr(isUsableNumber, direct, checkString);

        fastBuilder.SetInsertPoint(checkString);
        llvm::Value *dynamicStringPtr = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(svalueTy, dynamicValue, 2));
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                fastBuilder.CreateICmpEQ(
                    dynamicTag,
                    llvm::ConstantInt::get(i32, SJIT_VALUE_STRING)),
                fastBuilder.CreateNot(isNull(fastBuilder, dynamicStringPtr))),
            cachedString,
            fallback);

        fastBuilder.SetInsertPoint(cachedString);
        llvm::Value *dynamicCachedNumber = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(stringTy, dynamicStringPtr, 6));
        llvm::Value *dynamicCacheValid = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(stringTy, dynamicStringPtr, 3)),
            llvm::ConstantInt::get(i32, 0));
        llvm::Value *dynamicCacheOk = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(stringTy, dynamicStringPtr, 4)),
            llvm::ConstantInt::get(i32, 0));
        llvm::Value *dynamicCacheWhitespace = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(stringTy, dynamicStringPtr, 5)),
            llvm::ConstantInt::get(i32, 0));
        llvm::Value *dynamicWhitespaceOnly = fastBuilder.CreateAnd(
            fastBuilder.CreateFCmpOEQ(
                dynamicCachedNumber,
                llvm::ConstantFP::get(f64, 0.0)),
            dynamicCacheWhitespace);
        llvm::Value *dynamicCachedUsable = fastBuilder.CreateAnd(
            fastBuilder.CreateAnd(dynamicCacheValid, dynamicCacheOk),
            fastBuilder.CreateAnd(
                fastBuilder.CreateNot(dynamicWhitespaceOnly),
                fastBuilder.CreateFCmpORD(dynamicCachedNumber, dynamicCachedNumber)));
        fastBuilder.CreateCondBr(dynamicCachedUsable, direct, fallback);

        fastBuilder.SetInsertPoint(direct);
        auto *usableDynamicNumber = fastBuilder.CreatePHI(f64, 2, "dynamic_numeric_value");
        usableDynamicNumber->addIncoming(dynamicNumber, checkNumber);
        usableDynamicNumber->addIncoming(dynamicCachedNumber, cachedString);
        llvm::Value *leftNumber = fastBuilder.CreateSelect(
            dynamicIsLeft,
            usableDynamicNumber,
            literalNumber);
        llvm::Value *rightNumber = fastBuilder.CreateSelect(
            dynamicIsLeft,
            literalNumber,
            usableDynamicNumber);
        llvm::Value *isLess = fastBuilder.CreateFCmpOLT(leftNumber, rightNumber);
        llvm::Value *isEqual = fastBuilder.CreateFCmpOEQ(leftNumber, rightNumber);
        llvm::Value *isGreater = fastBuilder.CreateFCmpOGT(leftNumber, rightNumber);
        llvm::Value *result = fastBuilder.CreateSelect(
            fastBuilder.CreateICmpEQ(
                opcode,
                llvm::ConstantInt::get(i32, SJIT_EXPR_LT)),
            isLess,
            fastBuilder.CreateSelect(
                fastBuilder.CreateICmpEQ(
                    opcode,
                    llvm::ConstantInt::get(i32, SJIT_EXPR_EQ)),
                isEqual,
                fastBuilder.CreateSelect(
                    fastBuilder.CreateICmpEQ(
                        opcode,
                        llvm::ConstantInt::get(i32, SJIT_EXPR_GT)),
                    isGreater,
                    llvm::ConstantInt::getFalse(*context))));
        fastBuilder.CreateRet(fastBuilder.CreateZExt(result, i32));

        fastBuilder.SetInsertPoint(fallback);
        llvm::Value *leftValue = fastBuilder.CreateSelect(
            dynamicIsLeft,
            dynamicValue,
            literalValue);
        llvm::Value *rightValue = fastBuilder.CreateSelect(
            dynamicIsLeft,
            literalValue,
            dynamicValue);
        fastBuilder.CreateRet(fastBuilder.CreateCall(
            valueCompare,
            {fastRuntime, leftValue, rightValue, opcode}));
    }

    auto *fastArgumentValuePointerTy = llvm::FunctionType::get(
        ptr,
        {ptr, ptr, i32, ptr},
        false);
    auto *fastArgumentValuePointer = llvm::Function::Create(
        fastArgumentValuePointerTy,
        llvm::Function::InternalLinkage,
        name + "_fast_argument_value_pointer",
        module.get());
    fastArgumentValuePointer->addFnAttr(llvm::Attribute::AlwaysInline);
    {
        auto arg = fastArgumentValuePointer->arg_begin();
        llvm::Value *numericArguments = &*arg++;
        llvm::Value *valueArguments = &*arg++;
        llvm::Value *argumentIndex = &*arg++;
        llvm::Value *scratch = &*arg++;
        auto *entry = llvm::BasicBlock::Create(*context, "entry", fastArgumentValuePointer);
        auto *borrowed = llvm::BasicBlock::Create(*context, "borrowed", fastArgumentValuePointer);
        auto *checkNumeric = llvm::BasicBlock::Create(*context, "check_numeric", fastArgumentValuePointer);
        auto *numeric = llvm::BasicBlock::Create(*context, "numeric", fastArgumentValuePointer);
        auto *zero = llvm::BasicBlock::Create(*context, "zero", fastArgumentValuePointer);
        llvm::IRBuilder<> fastBuilder(entry);
        llvm::Value *validIndex = fastBuilder.CreateICmpSGE(
            argumentIndex,
            llvm::ConstantInt::get(i32, 0));
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                validIndex,
                fastBuilder.CreateNot(isNull(fastBuilder, valueArguments))),
            borrowed,
            checkNumeric);

        fastBuilder.SetInsertPoint(borrowed);
        fastBuilder.CreateRet(fastBuilder.CreateInBoundsGEP(
            svalueTy,
            valueArguments,
            argumentIndex));

        fastBuilder.SetInsertPoint(checkNumeric);
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                validIndex,
                fastBuilder.CreateAnd(
                    fastBuilder.CreateNot(isNull(fastBuilder, numericArguments)),
                    fastBuilder.CreateNot(isNull(fastBuilder, scratch)))),
            numeric,
            zero);

        fastBuilder.SetInsertPoint(numeric);
        fastBuilder.CreateStore(
            llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER),
            fastBuilder.CreateStructGEP(svalueTy, scratch, 0));
        fastBuilder.CreateStore(
            fastBuilder.CreateLoad(
                f64,
                fastBuilder.CreateInBoundsGEP(f64, numericArguments, argumentIndex)),
            fastBuilder.CreateStructGEP(svalueTy, scratch, 1));
        fastBuilder.CreateStore(
            llvm::ConstantPointerNull::get(ptr),
            fastBuilder.CreateStructGEP(svalueTy, scratch, 2));
        fastBuilder.CreateRet(scratch);

        fastBuilder.SetInsertPoint(zero);
        fastBuilder.CreateStore(
            llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER),
            fastBuilder.CreateStructGEP(svalueTy, scratch, 0));
        fastBuilder.CreateStore(
            llvm::ConstantFP::get(f64, 0.0),
            fastBuilder.CreateStructGEP(svalueTy, scratch, 1));
        fastBuilder.CreateStore(
            llvm::ConstantPointerNull::get(ptr),
            fastBuilder.CreateStructGEP(svalueTy, scratch, 2));
        fastBuilder.CreateRet(scratch);
    }

    auto *fastVariableArgumentValue = llvm::Function::Create(
        variableArgumentValueTy,
        llvm::Function::InternalLinkage,
        name + "_fast_variable_argument_value",
        module.get());
    fastVariableArgumentValue->addFnAttr(llvm::Attribute::AlwaysInline);
    {
        auto arg = fastVariableArgumentValue->arg_begin();
        llvm::Value *fastRuntime = &*arg++;
        llvm::Value *variable = &*arg++;
        llvm::Value *out = &*arg++;
        auto *entry = llvm::BasicBlock::Create(*context, "entry", fastVariableArgumentValue);
        auto *checkValue = llvm::BasicBlock::Create(*context, "check_value", fastVariableArgumentValue);
        auto *direct = llvm::BasicBlock::Create(*context, "direct", fastVariableArgumentValue);
        auto *fallback = llvm::BasicBlock::Create(*context, "fallback", fastVariableArgumentValue);
        llvm::IRBuilder<> fastBuilder(entry);
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                fastBuilder.CreateNot(isNull(fastBuilder, variable)),
                fastBuilder.CreateNot(isNull(fastBuilder, out))),
            checkValue,
            fallback);

        fastBuilder.SetInsertPoint(checkValue);
        llvm::Value *value = fastBuilder.CreateStructGEP(variableTy, variable, 5, "value");
        fastBuilder.CreateCondBr(
            fastBuilder.CreateICmpEQ(
                fastBuilder.CreateLoad(i32, fastBuilder.CreateStructGEP(svalueTy, value, 0)),
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)),
            direct,
            fallback);

        fastBuilder.SetInsertPoint(direct);
        llvm::Value *raw = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(svalueTy, value, 1),
            "raw");
        fastBuilder.CreateStore(
            llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER),
            fastBuilder.CreateStructGEP(svalueTy, out, 0));
        fastBuilder.CreateStore(raw, fastBuilder.CreateStructGEP(svalueTy, out, 1));
        fastBuilder.CreateStore(
            llvm::ConstantPointerNull::get(ptr),
            fastBuilder.CreateStructGEP(svalueTy, out, 2));
        fastBuilder.CreateRet(fastBuilder.CreateSelect(
            fastBuilder.CreateFCmpUNO(raw, raw),
            llvm::ConstantFP::get(f64, 0.0),
            raw));

        fastBuilder.SetInsertPoint(fallback);
        fastBuilder.CreateRet(fastBuilder.CreateCall(
            variableArgumentValue,
            {fastRuntime, variable, out}));
    }

    auto *fastProcedureArgumentCopy = llvm::Function::Create(
        procedureArgumentCopyTy,
        llvm::Function::InternalLinkage,
        name + "_fast_procedure_argument_copy",
        module.get());
    fastProcedureArgumentCopy->addFnAttr(llvm::Attribute::AlwaysInline);
    {
        auto arg = fastProcedureArgumentCopy->arg_begin();
        llvm::Value *numericArguments = &*arg++;
        llvm::Value *valueArguments = &*arg++;
        llvm::Value *argumentIndex = &*arg++;
        llvm::Value *out = &*arg++;
        auto *entry = llvm::BasicBlock::Create(*context, "entry", fastProcedureArgumentCopy);
        auto *chooseSource = llvm::BasicBlock::Create(*context, "choose_source", fastProcedureArgumentCopy);
        auto *numeric = llvm::BasicBlock::Create(*context, "numeric", fastProcedureArgumentCopy);
        auto *checkBoxed = llvm::BasicBlock::Create(*context, "check_boxed", fastProcedureArgumentCopy);
        auto *boxed = llvm::BasicBlock::Create(*context, "boxed", fastProcedureArgumentCopy);
        auto *fallback = llvm::BasicBlock::Create(*context, "fallback", fastProcedureArgumentCopy);
        llvm::IRBuilder<> fastBuilder(entry);
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                fastBuilder.CreateICmpSGE(argumentIndex, llvm::ConstantInt::get(i32, 0)),
                fastBuilder.CreateAnd(
                    fastBuilder.CreateNot(isNull(fastBuilder, numericArguments)),
                    fastBuilder.CreateNot(isNull(fastBuilder, out)))),
            chooseSource,
            fallback);

        fastBuilder.SetInsertPoint(chooseSource);
        fastBuilder.CreateCondBr(isNull(fastBuilder, valueArguments), numeric, checkBoxed);

        fastBuilder.SetInsertPoint(numeric);
        llvm::Value *numericValue = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateInBoundsGEP(f64, numericArguments, argumentIndex),
            "numeric_value");
        fastBuilder.CreateStore(
            llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER),
            fastBuilder.CreateStructGEP(svalueTy, out, 0));
        fastBuilder.CreateStore(numericValue, fastBuilder.CreateStructGEP(svalueTy, out, 1));
        fastBuilder.CreateStore(
            llvm::ConstantPointerNull::get(ptr),
            fastBuilder.CreateStructGEP(svalueTy, out, 2));
        fastBuilder.CreateRet(numericValue);

        fastBuilder.SetInsertPoint(checkBoxed);
        llvm::Value *source = fastBuilder.CreateInBoundsGEP(
            svalueTy,
            valueArguments,
            argumentIndex,
            "source");
        fastBuilder.CreateCondBr(
            fastBuilder.CreateICmpEQ(
                fastBuilder.CreateLoad(i32, fastBuilder.CreateStructGEP(svalueTy, source, 0)),
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)),
            boxed,
            fallback);

        fastBuilder.SetInsertPoint(boxed);
        llvm::Value *boxedRaw = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(svalueTy, source, 1),
            "boxed_raw");
        llvm::Value *boxedNumber = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateInBoundsGEP(f64, numericArguments, argumentIndex),
            "boxed_number");
        fastBuilder.CreateStore(
            llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER),
            fastBuilder.CreateStructGEP(svalueTy, out, 0));
        fastBuilder.CreateStore(boxedRaw, fastBuilder.CreateStructGEP(svalueTy, out, 1));
        fastBuilder.CreateStore(
            llvm::ConstantPointerNull::get(ptr),
            fastBuilder.CreateStructGEP(svalueTy, out, 2));
        fastBuilder.CreateRet(boxedNumber);

        fastBuilder.SetInsertPoint(fallback);
        fastBuilder.CreateRet(fastBuilder.CreateCall(
            procedureArgumentCopy,
            {numericArguments, valueArguments, argumentIndex, out}));
    }

    auto *fastCachedValueCompare = llvm::Function::Create(
        valueCompareTy,
        llvm::Function::InternalLinkage,
        name + "_fast_cached_value_compare",
        module.get());
    {
        auto arg = fastCachedValueCompare->arg_begin();
        llvm::Value *fastRuntime = &*arg++;
        llvm::Value *leftValue = &*arg++;
        llvm::Value *rightValue = &*arg++;
        llvm::Value *opcode = &*arg++;
        auto *entry = llvm::BasicBlock::Create(*context, "entry", fastCachedValueCompare);
        auto *checkTags = llvm::BasicBlock::Create(*context, "check_tags", fastCachedValueCompare);
        auto *leftNumber = llvm::BasicBlock::Create(*context, "left_number", fastCachedValueCompare);
        auto *leftNumberCheckString = llvm::BasicBlock::Create(
            *context,
            "left_number_check_string",
            fastCachedValueCompare);
        auto *numberNumber = llvm::BasicBlock::Create(*context, "number_number", fastCachedValueCompare);
        auto *numberString = llvm::BasicBlock::Create(*context, "number_string", fastCachedValueCompare);
        auto *leftString = llvm::BasicBlock::Create(*context, "left_string", fastCachedValueCompare);
        auto *stringNumber = llvm::BasicBlock::Create(*context, "string_number", fastCachedValueCompare);
        auto *leftStringRight = llvm::BasicBlock::Create(
            *context,
            "left_string_right",
            fastCachedValueCompare);
        auto *checkStringString = llvm::BasicBlock::Create(
            *context,
            "check_string_string",
            fastCachedValueCompare);
        auto *stringString = llvm::BasicBlock::Create(*context, "string_string", fastCachedValueCompare);
        auto *compare = llvm::BasicBlock::Create(*context, "compare_numbers", fastCachedValueCompare);
        auto *fallback = llvm::BasicBlock::Create(*context, "fallback", fastCachedValueCompare);
        llvm::IRBuilder<> fastBuilder(entry);
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                fastBuilder.CreateNot(isNull(fastBuilder, leftValue)),
                fastBuilder.CreateNot(isNull(fastBuilder, rightValue))),
            checkTags,
            fallback);

        fastBuilder.SetInsertPoint(checkTags);
        llvm::Value *leftTag = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(svalueTy, leftValue, 0));
        llvm::Value *rightTag = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(svalueTy, rightValue, 0));
        fastBuilder.CreateCondBr(
            fastBuilder.CreateICmpEQ(
                leftTag,
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)),
            leftNumber,
            leftString);

        fastBuilder.SetInsertPoint(leftNumber);
        llvm::Value *leftRaw = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(svalueTy, leftValue, 1));
        llvm::Value *leftNumberUsable = fastBuilder.CreateFCmpORD(leftRaw, leftRaw);
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                leftNumberUsable,
                fastBuilder.CreateICmpEQ(
                    rightTag,
                    llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER))),
            numberNumber,
            leftNumberCheckString);

        fastBuilder.SetInsertPoint(numberNumber);
        llvm::Value *rightRawFromNumber = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(svalueTy, rightValue, 1));
        fastBuilder.CreateCondBr(
            fastBuilder.CreateFCmpORD(rightRawFromNumber, rightRawFromNumber),
            compare,
            fallback);

        fastBuilder.SetInsertPoint(leftNumberCheckString);
        llvm::Value *rightStringPtr = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(svalueTy, rightValue, 2));
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                leftNumberUsable,
                fastBuilder.CreateAnd(
                    fastBuilder.CreateICmpEQ(
                        rightTag,
                        llvm::ConstantInt::get(i32, SJIT_VALUE_STRING)),
                    fastBuilder.CreateNot(isNull(fastBuilder, rightStringPtr)))),
            numberString,
            fallback);

        fastBuilder.SetInsertPoint(numberString);
        llvm::Value *rightCachedNumber = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(stringTy, rightStringPtr, 6));
        llvm::Value *rightCacheValid = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(stringTy, rightStringPtr, 3)),
            llvm::ConstantInt::get(i32, 0));
        llvm::Value *rightCacheOk = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(stringTy, rightStringPtr, 4)),
            llvm::ConstantInt::get(i32, 0));
        llvm::Value *rightCacheWhitespace = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(stringTy, rightStringPtr, 5)),
            llvm::ConstantInt::get(i32, 0));
        llvm::Value *rightWhitespaceOnly = fastBuilder.CreateAnd(
            fastBuilder.CreateFCmpOEQ(
                rightCachedNumber,
                llvm::ConstantFP::get(f64, 0.0)),
            rightCacheWhitespace);
        llvm::Value *rightCachedUsable = fastBuilder.CreateAnd(
            fastBuilder.CreateAnd(rightCacheValid, rightCacheOk),
            fastBuilder.CreateAnd(
                fastBuilder.CreateNot(rightWhitespaceOnly),
                fastBuilder.CreateFCmpORD(rightCachedNumber, rightCachedNumber)));
        fastBuilder.CreateCondBr(rightCachedUsable, compare, fallback);

        fastBuilder.SetInsertPoint(leftString);
        llvm::Value *leftStringPtr = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(svalueTy, leftValue, 2));
        llvm::Value *canCheckLeftString = fastBuilder.CreateAnd(
            fastBuilder.CreateICmpEQ(
                leftTag,
                llvm::ConstantInt::get(i32, SJIT_VALUE_STRING)),
            fastBuilder.CreateNot(isNull(fastBuilder, leftStringPtr)));
        fastBuilder.CreateCondBr(canCheckLeftString, stringNumber, fallback);

        fastBuilder.SetInsertPoint(stringNumber);
        llvm::Value *leftCachedNumber = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(stringTy, leftStringPtr, 6));
        llvm::Value *leftCacheValid = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(stringTy, leftStringPtr, 3)),
            llvm::ConstantInt::get(i32, 0));
        llvm::Value *leftCacheOk = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(stringTy, leftStringPtr, 4)),
            llvm::ConstantInt::get(i32, 0));
        llvm::Value *leftCacheWhitespace = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(stringTy, leftStringPtr, 5)),
            llvm::ConstantInt::get(i32, 0));
        llvm::Value *leftWhitespaceOnly = fastBuilder.CreateAnd(
            fastBuilder.CreateFCmpOEQ(
                leftCachedNumber,
                llvm::ConstantFP::get(f64, 0.0)),
            leftCacheWhitespace);
        llvm::Value *leftCachedUsable = fastBuilder.CreateAnd(
            fastBuilder.CreateAnd(leftCacheValid, leftCacheOk),
            fastBuilder.CreateAnd(
                fastBuilder.CreateNot(leftWhitespaceOnly),
                fastBuilder.CreateFCmpORD(leftCachedNumber, leftCachedNumber)));
        fastBuilder.CreateCondBr(leftCachedUsable, leftStringRight, fallback);

        fastBuilder.SetInsertPoint(leftStringRight);
        llvm::Value *rightRaw = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(svalueTy, rightValue, 1));
        llvm::Value *rightIsUsableNumber = fastBuilder.CreateAnd(
            fastBuilder.CreateICmpEQ(
                rightTag,
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)),
            fastBuilder.CreateFCmpORD(rightRaw, rightRaw));
        fastBuilder.CreateCondBr(rightIsUsableNumber, compare, checkStringString);

        fastBuilder.SetInsertPoint(checkStringString);
        llvm::Value *rightStringPtrForPair = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(svalueTy, rightValue, 2));
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                fastBuilder.CreateICmpEQ(
                    rightTag,
                    llvm::ConstantInt::get(i32, SJIT_VALUE_STRING)),
                fastBuilder.CreateNot(isNull(fastBuilder, rightStringPtrForPair))),
            stringString,
            fallback);

        fastBuilder.SetInsertPoint(stringString);
        llvm::Value *rightStringCachedNumber = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(stringTy, rightStringPtrForPair, 6));
        llvm::Value *rightStringCacheValid = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(stringTy, rightStringPtrForPair, 3)),
            llvm::ConstantInt::get(i32, 0));
        llvm::Value *rightStringCacheOk = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(stringTy, rightStringPtrForPair, 4)),
            llvm::ConstantInt::get(i32, 0));
        llvm::Value *rightStringCacheWhitespace = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(stringTy, rightStringPtrForPair, 5)),
            llvm::ConstantInt::get(i32, 0));
        llvm::Value *rightStringWhitespaceOnly = fastBuilder.CreateAnd(
            fastBuilder.CreateFCmpOEQ(
                rightStringCachedNumber,
                llvm::ConstantFP::get(f64, 0.0)),
            rightStringCacheWhitespace);
        llvm::Value *rightStringCachedUsable = fastBuilder.CreateAnd(
            fastBuilder.CreateAnd(rightStringCacheValid, rightStringCacheOk),
            fastBuilder.CreateAnd(
                fastBuilder.CreateNot(rightStringWhitespaceOnly),
                fastBuilder.CreateFCmpORD(
                    rightStringCachedNumber,
                    rightStringCachedNumber)));
        fastBuilder.CreateCondBr(rightStringCachedUsable, compare, fallback);

        fastBuilder.SetInsertPoint(compare);
        auto *numericLeft = fastBuilder.CreatePHI(f64, 4, "numeric_left");
        auto *numericRight = fastBuilder.CreatePHI(f64, 4, "numeric_right");
        numericLeft->addIncoming(leftRaw, numberNumber);
        numericRight->addIncoming(rightRawFromNumber, numberNumber);
        numericLeft->addIncoming(leftRaw, numberString);
        numericRight->addIncoming(rightCachedNumber, numberString);
        numericLeft->addIncoming(leftCachedNumber, leftStringRight);
        numericRight->addIncoming(rightRaw, leftStringRight);
        numericLeft->addIncoming(leftCachedNumber, stringString);
        numericRight->addIncoming(rightStringCachedNumber, stringString);
        llvm::Value *isLess = fastBuilder.CreateFCmpOLT(numericLeft, numericRight);
        llvm::Value *isEqual = fastBuilder.CreateFCmpOEQ(numericLeft, numericRight);
        llvm::Value *isGreater = fastBuilder.CreateFCmpOGT(numericLeft, numericRight);
        llvm::Value *result = fastBuilder.CreateSelect(
            fastBuilder.CreateICmpEQ(
                opcode,
                llvm::ConstantInt::get(i32, SJIT_EXPR_LT)),
            isLess,
            fastBuilder.CreateSelect(
                fastBuilder.CreateICmpEQ(
                    opcode,
                    llvm::ConstantInt::get(i32, SJIT_EXPR_EQ)),
                isEqual,
                fastBuilder.CreateSelect(
                    fastBuilder.CreateICmpEQ(
                        opcode,
                        llvm::ConstantInt::get(i32, SJIT_EXPR_GT)),
                    isGreater,
                    llvm::ConstantInt::getFalse(*context))));
        fastBuilder.CreateRet(fastBuilder.CreateZExt(result, i32));

        fastBuilder.SetInsertPoint(fallback);
        fastBuilder.CreateRet(fastBuilder.CreateCall(
            valueCompare,
            {fastRuntime, leftValue, rightValue, opcode}));
    }

    auto *fastVariableSetFromVariable = llvm::Function::Create(
        variableSetFromVariableTy,
        llvm::Function::InternalLinkage,
        name + "_fast_set_from_variable",
        module.get());
    fastVariableSetFromVariable->addFnAttr(llvm::Attribute::AlwaysInline);
    {
        auto arg = fastVariableSetFromVariable->arg_begin();
        llvm::Value *fastRuntime = &*arg++;
        llvm::Value *destination = &*arg++;
        llvm::Value *source = &*arg++;
        auto *entry = llvm::BasicBlock::Create(*context, "entry", fastVariableSetFromVariable);
        auto *checkValues = llvm::BasicBlock::Create(*context, "check_values", fastVariableSetFromVariable);
        auto *direct = llvm::BasicBlock::Create(*context, "direct_number", fastVariableSetFromVariable);
        auto *checkString = llvm::BasicBlock::Create(*context, "check_string", fastVariableSetFromVariable);
        auto *checkSameString = llvm::BasicBlock::Create(*context, "check_same_string", fastVariableSetFromVariable);
        auto *sameString = llvm::BasicBlock::Create(*context, "same_string", fastVariableSetFromVariable);
        auto *directString = llvm::BasicBlock::Create(*context, "direct_string", fastVariableSetFromVariable);
        auto *fallback = llvm::BasicBlock::Create(*context, "fallback", fastVariableSetFromVariable);
        llvm::IRBuilder<> fastBuilder(entry);
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                fastBuilder.CreateNot(isNull(fastBuilder, destination)),
                fastBuilder.CreateNot(isNull(fastBuilder, source))),
            checkValues,
            fallback);

        fastBuilder.SetInsertPoint(checkValues);
        llvm::Value *destinationValue = fastBuilder.CreateStructGEP(variableTy, destination, 5);
        llvm::Value *sourceValue = fastBuilder.CreateStructGEP(variableTy, source, 5);
        llvm::Value *destinationKind = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(variableTy, destination, 4));
        llvm::Value *destinationNotBool = fastBuilder.CreateICmpNE(
            destinationKind,
            llvm::ConstantInt::get(i32, SJIT_SCALAR_BOOL));
        llvm::Value *sourceTag = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(svalueTy, sourceValue, 0));
        llvm::Value *sourceString = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(svalueTy, sourceValue, 2));
        llvm::Value *canStore = fastBuilder.CreateAnd(
            destinationNotBool,
            fastBuilder.CreateAnd(
                fastBuilder.CreateICmpEQ(
                    fastBuilder.CreateLoad(i32, fastBuilder.CreateStructGEP(svalueTy, destinationValue, 0)),
                    llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)),
                fastBuilder.CreateICmpEQ(sourceTag, llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER))));
        fastBuilder.CreateCondBr(canStore, direct, checkString);

        fastBuilder.SetInsertPoint(checkString);
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                fastBuilder.CreateICmpEQ(sourceTag, llvm::ConstantInt::get(i32, SJIT_VALUE_STRING)),
                fastBuilder.CreateNot(isNull(fastBuilder, sourceString))),
            checkSameString,
            fallback);

        fastBuilder.SetInsertPoint(checkSameString);
        llvm::Value *destinationTag = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(svalueTy, destinationValue, 0),
            "destination_string_tag");
        llvm::Value *destinationString = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(svalueTy, destinationValue, 2),
            "destination_string");
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                fastBuilder.CreateICmpEQ(
                    destinationTag,
                    llvm::ConstantInt::get(i32, SJIT_VALUE_STRING)),
                fastBuilder.CreateICmpEQ(destinationString, sourceString)),
            sameString,
            directString);

        fastBuilder.SetInsertPoint(sameString);
        fastBuilder.CreateRetVoid();

        fastBuilder.SetInsertPoint(direct);
        fastBuilder.CreateStore(
            fastBuilder.CreateLoad(f64, fastBuilder.CreateStructGEP(svalueTy, sourceValue, 1)),
            fastBuilder.CreateStructGEP(svalueTy, destinationValue, 1));
        fastBuilder.CreateStore(
            llvm::ConstantPointerNull::get(ptr),
            fastBuilder.CreateStructGEP(svalueTy, destinationValue, 2));
        fastBuilder.CreateRetVoid();

        fastBuilder.SetInsertPoint(directString);
        fastBuilder.CreateCall(
            variableSetStringBorrowed,
            {fastRuntime, destination, sourceString});
        fastBuilder.CreateRetVoid();

        fastBuilder.SetInsertPoint(fallback);
        fastBuilder.CreateCall(variableSetFromVariable, {fastRuntime, destination, source});
        fastBuilder.CreateRetVoid();
    }

    auto *fastVariableSetFromArgument = llvm::Function::Create(
        variableSetFromArgumentTy,
        llvm::Function::InternalLinkage,
        name + "_fast_set_from_argument",
        module.get());
    fastVariableSetFromArgument->addFnAttr(llvm::Attribute::AlwaysInline);
    {
        auto arg = fastVariableSetFromArgument->arg_begin();
        llvm::Value *fastRuntime = &*arg++;
        llvm::Value *destination = &*arg++;
        llvm::Value *numericArguments = &*arg++;
        llvm::Value *valueArguments = &*arg++;
        llvm::Value *argumentIndex = &*arg++;
        auto *entry = llvm::BasicBlock::Create(*context, "entry", fastVariableSetFromArgument);
        auto *checkDestination = llvm::BasicBlock::Create(*context, "check_destination", fastVariableSetFromArgument);
        auto *chooseSource = llvm::BasicBlock::Create(*context, "choose_source", fastVariableSetFromArgument);
        auto *numericSource = llvm::BasicBlock::Create(*context, "numeric_source", fastVariableSetFromArgument);
        auto *checkNumericDestination = llvm::BasicBlock::Create(*context, "check_numeric_destination", fastVariableSetFromArgument);
        auto *boxedSource = llvm::BasicBlock::Create(*context, "boxed_source", fastVariableSetFromArgument);
        auto *checkBoxedNumberDestination = llvm::BasicBlock::Create(*context, "check_boxed_number_destination", fastVariableSetFromArgument);
        auto *checkBoxedString = llvm::BasicBlock::Create(*context, "check_boxed_string", fastVariableSetFromArgument);
        auto *directNumeric = llvm::BasicBlock::Create(*context, "direct_numeric", fastVariableSetFromArgument);
        auto *directBoxed = llvm::BasicBlock::Create(*context, "direct_boxed", fastVariableSetFromArgument);
        auto *directString = llvm::BasicBlock::Create(*context, "direct_string", fastVariableSetFromArgument);
        auto *checkSameString = llvm::BasicBlock::Create(*context, "check_same_string", fastVariableSetFromArgument);
        auto *sameString = llvm::BasicBlock::Create(*context, "same_string", fastVariableSetFromArgument);
        auto *fallback = llvm::BasicBlock::Create(*context, "fallback", fastVariableSetFromArgument);
        llvm::IRBuilder<> fastBuilder(entry);
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                fastBuilder.CreateNot(isNull(fastBuilder, destination)),
                fastBuilder.CreateICmpSGE(argumentIndex, llvm::ConstantInt::get(i32, 0))),
            checkDestination,
            fallback);

        fastBuilder.SetInsertPoint(checkDestination);
        llvm::Value *destinationValue = fastBuilder.CreateStructGEP(variableTy, destination, 5);
        llvm::Value *destinationKind = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(variableTy, destination, 4));
        fastBuilder.CreateCondBr(
            fastBuilder.CreateICmpNE(
                destinationKind,
                llvm::ConstantInt::get(i32, SJIT_SCALAR_BOOL)),
            chooseSource,
            fallback);

        fastBuilder.SetInsertPoint(chooseSource);
        fastBuilder.CreateCondBr(isNull(fastBuilder, valueArguments), numericSource, boxedSource);

        fastBuilder.SetInsertPoint(numericSource);
        fastBuilder.CreateCondBr(
            fastBuilder.CreateNot(isNull(fastBuilder, numericArguments)),
            checkNumericDestination,
            fallback);

        fastBuilder.SetInsertPoint(checkNumericDestination);
        fastBuilder.CreateCondBr(
            fastBuilder.CreateICmpEQ(
                fastBuilder.CreateLoad(i32, fastBuilder.CreateStructGEP(svalueTy, destinationValue, 0)),
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)),
            directNumeric,
            fallback);

        fastBuilder.SetInsertPoint(directNumeric);
        llvm::Value *numericArgument = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateInBoundsGEP(f64, numericArguments, argumentIndex));
        llvm::Value *numericDestinationKind = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(variableTy, destination, 4));
        llvm::Value *normalizedNumericArgument = fastBuilder.CreateSelect(
            fastBuilder.CreateAnd(
                fastBuilder.CreateICmpEQ(
                    numericDestinationKind,
                    llvm::ConstantInt::get(i32, SJIT_SCALAR_NUMBER)),
                fastBuilder.CreateFCmpUNO(numericArgument, numericArgument)),
            llvm::ConstantFP::get(f64, 0.0),
            numericArgument);
        fastBuilder.CreateStore(
            normalizedNumericArgument,
            fastBuilder.CreateStructGEP(svalueTy, destinationValue, 1));
        fastBuilder.CreateStore(
            llvm::ConstantPointerNull::get(ptr),
            fastBuilder.CreateStructGEP(svalueTy, destinationValue, 2));
        fastBuilder.CreateRetVoid();

        fastBuilder.SetInsertPoint(boxedSource);
        llvm::Value *sourceValue = fastBuilder.CreateInBoundsGEP(svalueTy, valueArguments, argumentIndex);
        llvm::Value *sourceTag = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(svalueTy, sourceValue, 0));
        llvm::Value *sourceString = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(svalueTy, sourceValue, 2));
        fastBuilder.CreateCondBr(
            fastBuilder.CreateICmpEQ(sourceTag, llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)),
            checkBoxedNumberDestination,
            checkBoxedString);

        fastBuilder.SetInsertPoint(checkBoxedNumberDestination);
        fastBuilder.CreateCondBr(
            fastBuilder.CreateICmpEQ(
                fastBuilder.CreateLoad(i32, fastBuilder.CreateStructGEP(svalueTy, destinationValue, 0)),
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)),
            directBoxed,
            fallback);

        fastBuilder.SetInsertPoint(checkBoxedString);
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                fastBuilder.CreateICmpNE(
                    destinationKind,
                    llvm::ConstantInt::get(i32, SJIT_SCALAR_NUMBER)),
                fastBuilder.CreateAnd(
                    fastBuilder.CreateICmpEQ(
                        sourceTag,
                        llvm::ConstantInt::get(i32, SJIT_VALUE_STRING)),
                    fastBuilder.CreateNot(isNull(fastBuilder, sourceString)))),
            checkSameString,
            fallback);

        fastBuilder.SetInsertPoint(checkSameString);
        llvm::Value *destinationStringTag = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(svalueTy, destinationValue, 0),
            "destination_string_tag");
        llvm::Value *destinationStringPointer = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(svalueTy, destinationValue, 2),
            "destination_string");
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                fastBuilder.CreateICmpEQ(
                    destinationStringTag,
                    llvm::ConstantInt::get(i32, SJIT_VALUE_STRING)),
                fastBuilder.CreateICmpEQ(destinationStringPointer, sourceString)),
            sameString,
            directString);

        fastBuilder.SetInsertPoint(sameString);
        fastBuilder.CreateRetVoid();

        fastBuilder.SetInsertPoint(directBoxed);
        llvm::Value *boxedArgument = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(svalueTy, sourceValue, 1));
        llvm::Value *boxedDestinationKind = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(variableTy, destination, 4));
        llvm::Value *normalizedBoxedArgument = fastBuilder.CreateSelect(
            fastBuilder.CreateAnd(
                fastBuilder.CreateICmpEQ(
                    boxedDestinationKind,
                    llvm::ConstantInt::get(i32, SJIT_SCALAR_NUMBER)),
                fastBuilder.CreateFCmpUNO(boxedArgument, boxedArgument)),
            llvm::ConstantFP::get(f64, 0.0),
            boxedArgument);
        fastBuilder.CreateStore(
            normalizedBoxedArgument,
            fastBuilder.CreateStructGEP(svalueTy, destinationValue, 1));
        fastBuilder.CreateStore(
            llvm::ConstantPointerNull::get(ptr),
            fastBuilder.CreateStructGEP(svalueTy, destinationValue, 2));
        fastBuilder.CreateRetVoid();

        fastBuilder.SetInsertPoint(directString);
        fastBuilder.CreateCall(
            variableSetStringBorrowed,
            {fastRuntime, destination, sourceString});
        fastBuilder.CreateRetVoid();

        fastBuilder.SetInsertPoint(fallback);
        fastBuilder.CreateCall(
            variableSetFromArgument,
            {fastRuntime, destination, numericArguments, valueArguments, argumentIndex});
        fastBuilder.CreateRetVoid();
    }

    const bool speculativeNumericSets =
        std::getenv("SJIT_DISABLE_SPECULATIVE_NUMERIC_SET") == nullptr;
    const bool directMixedListNumbers =
        std::getenv("SJIT_DISABLE_DIRECT_MIXED_LIST_NUMBER") == nullptr;

    auto *fastListNumber = llvm::Function::Create(
        llvm::FunctionType::get(f64, {ptr, ptr, ptr, f64}, false),
        llvm::Function::InternalLinkage,
        name + "_fast_list_number",
        module.get());
    {
        auto fastArg = fastListNumber->arg_begin();
        llvm::Value *fastRuntime = &*fastArg++;
        llvm::Value *fastVariable = &*fastArg++;
        llvm::Value *fastKnownList = &*fastArg++;
        llvm::Value *fastIndex = &*fastArg++;
        auto *fastEntry = llvm::BasicBlock::Create(*context, "entry", fastListNumber);
        auto *fastChooseList = llvm::BasicBlock::Create(*context, "choose_list", fastListNumber);
        auto *fastCheckVariable = llvm::BasicBlock::Create(*context, "check_variable", fastListNumber);
        auto *fastHaveVariable = llvm::BasicBlock::Create(*context, "have_variable", fastListNumber);
        auto *fastHaveList = llvm::BasicBlock::Create(*context, "have_list", fastListNumber);
        auto *fastHaveStorage = llvm::BasicBlock::Create(*context, "have_storage", fastListNumber);
        auto *fastCached = llvm::BasicBlock::Create(*context, "cached", fastListNumber);
        auto *fastCheckItems = llvm::BasicBlock::Create(*context, "check_items", fastListNumber);
        auto *fastHaveItem = llvm::BasicBlock::Create(*context, "have_item", fastListNumber);
        auto *fastItemNumber = llvm::BasicBlock::Create(*context, "item_number", fastListNumber);
        auto *fastCheckItemString = llvm::BasicBlock::Create(*context, "check_item_string", fastListNumber);
        auto *fastItemString = llvm::BasicBlock::Create(*context, "item_string", fastListNumber);
        auto *fastItemStringCached = llvm::BasicBlock::Create(
            *context,
            "item_string_cached",
            fastListNumber);
        auto *fastFallback = llvm::BasicBlock::Create(*context, "fallback", fastListNumber);
        llvm::IRBuilder<> fastBuilder(fastEntry);
        fastBuilder.CreateBr(fastChooseList);

        fastBuilder.SetInsertPoint(fastChooseList);
        fastBuilder.CreateCondBr(
            isNull(fastBuilder, fastKnownList),
            fastCheckVariable,
            fastHaveList);

        fastBuilder.SetInsertPoint(fastCheckVariable);
        fastBuilder.CreateCondBr(
            isNull(fastBuilder, fastVariable),
            fastFallback,
            fastHaveVariable);

        fastBuilder.SetInsertPoint(fastHaveVariable);
        llvm::Value *fastValue = fastBuilder.CreateStructGEP(
            variableTy,
            fastVariable,
            5,
            "value_ptr");
        llvm::Value *fastTag = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(svalueTy, fastValue, 0),
            "value_tag");
        llvm::Value *fastLoadedList = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(svalueTy, fastValue, 2),
            "list");
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                fastBuilder.CreateICmpEQ(
                    fastTag,
                    llvm::ConstantInt::get(i32, SJIT_VALUE_LIST)),
                fastBuilder.CreateNot(isNull(fastBuilder, fastLoadedList))),
            fastHaveList,
            fastFallback);

        fastBuilder.SetInsertPoint(fastHaveList);
        auto *fastList = fastBuilder.CreatePHI(ptr, 2, "selected_list");
        fastList->addIncoming(fastKnownList, fastChooseList);
        fastList->addIncoming(fastLoadedList, fastHaveVariable);
        llvm::Value *fastStorage = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(listTy, fastList, 0),
            "storage");
        fastBuilder.CreateCondBr(
            isNull(fastBuilder, fastStorage),
            fastFallback,
            fastHaveStorage);

        fastBuilder.SetInsertPoint(fastHaveStorage);
        llvm::Value *fastLength = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(listStorageTy, fastStorage, 1),
            "length");
        llvm::Value *fastNumbers = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(listStorageTy, fastStorage, 4),
            "numbers");
        llvm::Value *fastNumbersValid = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(listStorageTy, fastStorage, 5)),
            llvm::ConstantInt::get(i32, 0),
            "numbers_valid");
        llvm::Value *fastLimit = fastBuilder.CreateFAdd(
            fastBuilder.CreateSIToFP(fastLength, f64),
            llvm::ConstantFP::get(f64, 1.0),
            "index_limit");
        llvm::Value *fastInRange = fastBuilder.CreateAnd(
            fastBuilder.CreateFCmpOGE(
                fastIndex,
                llvm::ConstantFP::get(f64, 1.0)),
            fastBuilder.CreateFCmpOLT(fastIndex, fastLimit),
            "index_in_range");
        llvm::Value *fastCanRead = fastBuilder.CreateAnd(
            fastNumbersValid,
            fastBuilder.CreateNot(isNull(fastBuilder, fastNumbers)));
        fastCanRead = fastBuilder.CreateAnd(fastCanRead, fastInRange);
        fastBuilder.CreateCondBr(
            fastCanRead,
            fastCached,
            directMixedListNumbers ? fastCheckItems : fastFallback);

        fastBuilder.SetInsertPoint(fastCached);
        llvm::Value *fastZeroBased = fastBuilder.CreateSub(
            fastBuilder.CreateFPToSI(fastIndex, i32),
            llvm::ConstantInt::get(i32, 1));
        llvm::Value *fastNumber = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateInBoundsGEP(f64, fastNumbers, fastZeroBased),
            "number");
        fastBuilder.CreateRet(fastBuilder.CreateSelect(
            fastBuilder.CreateFCmpUNO(fastNumber, fastNumber),
            llvm::ConstantFP::get(f64, 0.0),
            fastNumber));

        fastBuilder.SetInsertPoint(fastCheckItems);
        llvm::Value *fastItems = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(listStorageTy, fastStorage, 0),
            "items");
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                fastInRange,
                fastBuilder.CreateNot(isNull(fastBuilder, fastItems))),
            fastHaveItem,
            fastFallback);

        fastBuilder.SetInsertPoint(fastHaveItem);
        llvm::Value *fastItemZeroBased = fastBuilder.CreateSub(
            fastBuilder.CreateFPToSI(fastIndex, i32),
            llvm::ConstantInt::get(i32, 1));
        llvm::Value *fastItem = fastBuilder.CreateInBoundsGEP(
            svalueTy,
            fastItems,
            fastItemZeroBased,
            "item");
        llvm::Value *fastItemTag = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(svalueTy, fastItem, 0),
            "item_tag");
        fastBuilder.CreateCondBr(
            fastBuilder.CreateICmpEQ(
                fastItemTag,
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)),
            fastItemNumber,
            fastCheckItemString);

        fastBuilder.SetInsertPoint(fastItemNumber);
        llvm::Value *fastItemRaw = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(svalueTy, fastItem, 1),
            "item_number_value");
        fastBuilder.CreateRet(fastBuilder.CreateSelect(
            fastBuilder.CreateFCmpUNO(fastItemRaw, fastItemRaw),
            llvm::ConstantFP::get(f64, 0.0),
            fastItemRaw));

        fastBuilder.SetInsertPoint(fastCheckItemString);
        llvm::Value *fastItemStringPtr = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(svalueTy, fastItem, 2),
            "item_string_ptr");
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                fastBuilder.CreateICmpEQ(
                    fastItemTag,
                    llvm::ConstantInt::get(i32, SJIT_VALUE_STRING)),
                fastBuilder.CreateNot(isNull(fastBuilder, fastItemStringPtr))),
            fastItemString,
            fastFallback);

        fastBuilder.SetInsertPoint(fastItemString);
        llvm::Value *fastItemStringCacheValid = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(stringTy, fastItemStringPtr, 3)),
            llvm::ConstantInt::get(i32, 0),
            "item_string_cache_valid");
        fastBuilder.CreateCondBr(
            fastItemStringCacheValid,
            fastItemStringCached,
            fastFallback);

        fastBuilder.SetInsertPoint(fastItemStringCached);
        llvm::Value *fastItemStringCacheOk = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(stringTy, fastItemStringPtr, 4)),
            llvm::ConstantInt::get(i32, 0),
            "item_string_cache_ok");
        llvm::Value *fastItemStringNumber = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(stringTy, fastItemStringPtr, 6),
            "item_string_number");
        llvm::Value *fastItemStringUsable = fastBuilder.CreateAnd(
            fastItemStringCacheOk,
            fastBuilder.CreateFCmpORD(fastItemStringNumber, fastItemStringNumber));
        fastBuilder.CreateRet(fastBuilder.CreateSelect(
            fastItemStringUsable,
            fastItemStringNumber,
            llvm::ConstantFP::get(f64, 0.0)));

        fastBuilder.SetInsertPoint(fastFallback);
        fastBuilder.CreateRet(fastBuilder.CreateCall(
            listVariableItemNumberAt,
            {fastRuntime, fastVariable, fastIndex},
            "fallback_number"));
    }

    auto *fastListNumberAtVariable = llvm::Function::Create(
        listVariableItemNumberAtVariableTy,
        llvm::Function::InternalLinkage,
        name + "_fast_list_number_at_variable",
        module.get());
    fastListNumberAtVariable->addFnAttr(llvm::Attribute::AlwaysInline);
    {
        auto arg = fastListNumberAtVariable->arg_begin();
        llvm::Value *fastRuntime = &*arg++;
        llvm::Value *listVariable = &*arg++;
        llvm::Value *indexVariable = &*arg++;
        auto *entry = llvm::BasicBlock::Create(*context, "entry", fastListNumberAtVariable);
        auto *checkIndex = llvm::BasicBlock::Create(*context, "check_index", fastListNumberAtVariable);
        auto *checkString = llvm::BasicBlock::Create(*context, "check_string", fastListNumberAtVariable);
        auto *cachedString = llvm::BasicBlock::Create(*context, "cached_string", fastListNumberAtVariable);
        auto *direct = llvm::BasicBlock::Create(*context, "direct_number", fastListNumberAtVariable);
        auto *fallback = llvm::BasicBlock::Create(*context, "fallback", fastListNumberAtVariable);
        llvm::IRBuilder<> fastBuilder(entry);
        fastBuilder.CreateCondBr(
            isNull(fastBuilder, indexVariable),
            fallback,
            checkIndex);

        fastBuilder.SetInsertPoint(checkIndex);
        llvm::Value *indexValue = fastBuilder.CreateStructGEP(variableTy, indexVariable, 5);
        llvm::Value *indexTag = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(svalueTy, indexValue, 0));
        llvm::Value *indexNumber = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(svalueTy, indexValue, 1));
        fastBuilder.CreateCondBr(
            fastBuilder.CreateICmpEQ(
                indexTag,
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)),
            direct,
            checkString);

        fastBuilder.SetInsertPoint(checkString);
        llvm::Value *indexString = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(svalueTy, indexValue, 2));
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                fastBuilder.CreateICmpEQ(
                    indexTag,
                    llvm::ConstantInt::get(i32, SJIT_VALUE_STRING)),
                fastBuilder.CreateNot(isNull(fastBuilder, indexString))),
            cachedString,
            fallback);

        fastBuilder.SetInsertPoint(cachedString);
        llvm::Value *cachedIndexNumber = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(stringTy, indexString, 6));
        llvm::Value *cachedIndexUsable = fastBuilder.CreateAnd(
            fastBuilder.CreateICmpNE(
                fastBuilder.CreateLoad(
                    i32,
                    fastBuilder.CreateStructGEP(stringTy, indexString, 3)),
                llvm::ConstantInt::get(i32, 0)),
            fastBuilder.CreateAnd(
                fastBuilder.CreateICmpNE(
                    fastBuilder.CreateLoad(
                        i32,
                        fastBuilder.CreateStructGEP(stringTy, indexString, 4)),
                    llvm::ConstantInt::get(i32, 0)),
                fastBuilder.CreateFCmpORD(cachedIndexNumber, cachedIndexNumber)));
        fastBuilder.CreateCondBr(cachedIndexUsable, direct, fallback);

        fastBuilder.SetInsertPoint(direct);
        auto *resolvedIndex = fastBuilder.CreatePHI(f64, 2, "resolved_index");
        resolvedIndex->addIncoming(indexNumber, checkIndex);
        resolvedIndex->addIncoming(cachedIndexNumber, cachedString);
        fastBuilder.CreateRet(fastBuilder.CreateCall(
            fastListNumber,
            {
                fastRuntime,
                listVariable,
                llvm::ConstantPointerNull::get(ptr),
                resolvedIndex,
            }));

        fastBuilder.SetInsertPoint(fallback);
        fastBuilder.CreateRet(fastBuilder.CreateCall(
            listVariableItemNumberAtVariable,
            {fastRuntime, listVariable, indexVariable}));
    }

    auto *fastListNumberAtArgument = llvm::Function::Create(
        listVariableItemNumberAtArgumentTy,
        llvm::Function::InternalLinkage,
        name + "_fast_list_number_at_argument",
        module.get());
    fastListNumberAtArgument->addFnAttr(llvm::Attribute::AlwaysInline);
    {
        auto arg = fastListNumberAtArgument->arg_begin();
        llvm::Value *fastRuntime = &*arg++;
        llvm::Value *listVariable = &*arg++;
        llvm::Value *numericArguments = &*arg++;
        llvm::Value *valueArguments = &*arg++;
        llvm::Value *argumentIndex = &*arg++;
        auto *entry = llvm::BasicBlock::Create(*context, "entry", fastListNumberAtArgument);
        auto *chooseSource = llvm::BasicBlock::Create(*context, "choose_source", fastListNumberAtArgument);
        auto *numericSource = llvm::BasicBlock::Create(*context, "numeric_source", fastListNumberAtArgument);
        auto *numericDirect = llvm::BasicBlock::Create(*context, "numeric_direct", fastListNumberAtArgument);
        auto *valueSource = llvm::BasicBlock::Create(*context, "value_source", fastListNumberAtArgument);
        auto *checkString = llvm::BasicBlock::Create(*context, "check_string", fastListNumberAtArgument);
        auto *cachedString = llvm::BasicBlock::Create(*context, "cached_string", fastListNumberAtArgument);
        auto *direct = llvm::BasicBlock::Create(*context, "direct_number", fastListNumberAtArgument);
        auto *fallback = llvm::BasicBlock::Create(*context, "fallback", fastListNumberAtArgument);
        llvm::IRBuilder<> fastBuilder(entry);
        fastBuilder.CreateCondBr(
            fastBuilder.CreateICmpSGE(argumentIndex, llvm::ConstantInt::get(i32, 0)),
            chooseSource,
            fallback);

        fastBuilder.SetInsertPoint(chooseSource);
        fastBuilder.CreateCondBr(
            isNull(fastBuilder, valueArguments),
            numericSource,
            valueSource);

        fastBuilder.SetInsertPoint(numericSource);
        fastBuilder.CreateCondBr(
            fastBuilder.CreateNot(isNull(fastBuilder, numericArguments)),
            numericDirect,
            fallback);

        fastBuilder.SetInsertPoint(numericDirect);
        llvm::Value *numericIndex = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateInBoundsGEP(f64, numericArguments, argumentIndex));
        fastBuilder.CreateBr(direct);

        fastBuilder.SetInsertPoint(valueSource);
        llvm::Value *indexValue = fastBuilder.CreateInBoundsGEP(
            svalueTy,
            valueArguments,
            argumentIndex);
        llvm::Value *indexTag = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(svalueTy, indexValue, 0));
        llvm::Value *boxedIndex = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(svalueTy, indexValue, 1));
        fastBuilder.CreateCondBr(
            fastBuilder.CreateICmpEQ(
                indexTag,
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)),
            direct,
            checkString);

        fastBuilder.SetInsertPoint(checkString);
        llvm::Value *indexString = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(svalueTy, indexValue, 2));
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                fastBuilder.CreateICmpEQ(
                    indexTag,
                    llvm::ConstantInt::get(i32, SJIT_VALUE_STRING)),
                fastBuilder.CreateNot(isNull(fastBuilder, indexString))),
            cachedString,
            fallback);

        fastBuilder.SetInsertPoint(cachedString);
        llvm::Value *cachedIndex = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(stringTy, indexString, 6));
        llvm::Value *cachedIndexUsable = fastBuilder.CreateAnd(
            fastBuilder.CreateICmpNE(
                fastBuilder.CreateLoad(
                    i32,
                    fastBuilder.CreateStructGEP(stringTy, indexString, 3)),
                llvm::ConstantInt::get(i32, 0)),
            fastBuilder.CreateAnd(
                fastBuilder.CreateICmpNE(
                    fastBuilder.CreateLoad(
                        i32,
                        fastBuilder.CreateStructGEP(stringTy, indexString, 4)),
                    llvm::ConstantInt::get(i32, 0)),
                fastBuilder.CreateFCmpORD(cachedIndex, cachedIndex)));
        fastBuilder.CreateCondBr(cachedIndexUsable, direct, fallback);

        fastBuilder.SetInsertPoint(direct);
        auto *resolvedIndex = fastBuilder.CreatePHI(f64, 3, "resolved_index");
        resolvedIndex->addIncoming(numericIndex, numericDirect);
        resolvedIndex->addIncoming(boxedIndex, valueSource);
        resolvedIndex->addIncoming(cachedIndex, cachedString);
        fastBuilder.CreateRet(fastBuilder.CreateCall(
            fastListNumber,
            {
                fastRuntime,
                listVariable,
                llvm::ConstantPointerNull::get(ptr),
                resolvedIndex,
            }));

        fastBuilder.SetInsertPoint(fallback);
        fastBuilder.CreateRet(fastBuilder.CreateCall(
            listVariableItemNumberAtArgument,
            {fastRuntime, listVariable, numericArguments, valueArguments, argumentIndex}));
    }

    const bool speculativeNumericListIndices =
        std::getenv("SJIT_DISABLE_SPECULATIVE_LIST_INDEX") == nullptr;

    auto *fastListReplaceNumberAt = llvm::Function::Create(
        listVariableReplaceNumberAtTy,
        llvm::Function::InternalLinkage,
        name + "_fast_list_replace_number_at",
        module.get());
    fastListReplaceNumberAt->addFnAttr(llvm::Attribute::AlwaysInline);
    {
        auto fastArg = fastListReplaceNumberAt->arg_begin();
        llvm::Value *fastRuntime = &*fastArg++;
        llvm::Value *fastListVariable = &*fastArg++;
        llvm::Value *fastIndex = &*fastArg++;
        llvm::Value *fastValue = &*fastArg++;
        auto *entry = llvm::BasicBlock::Create(*context, "entry", fastListReplaceNumberAt);
        auto *haveVariable = llvm::BasicBlock::Create(*context, "have_variable", fastListReplaceNumberAt);
        auto *haveList = llvm::BasicBlock::Create(*context, "have_list", fastListReplaceNumberAt);
        auto *haveStorage = llvm::BasicBlock::Create(*context, "have_storage", fastListReplaceNumberAt);
        auto *haveItem = llvm::BasicBlock::Create(*context, "have_item", fastListReplaceNumberAt);
        auto *direct = llvm::BasicBlock::Create(*context, "direct", fastListReplaceNumberAt);
        auto *cache = llvm::BasicBlock::Create(*context, "cache", fastListReplaceNumberAt);
        auto *fallback = llvm::BasicBlock::Create(*context, "fallback", fastListReplaceNumberAt);
        auto *done = llvm::BasicBlock::Create(*context, "done", fastListReplaceNumberAt);
        llvm::IRBuilder<> fastBuilder(entry);
        fastBuilder.CreateCondBr(
            fastBuilder.CreateNot(isNull(fastBuilder, fastListVariable)),
            haveVariable,
            fallback);

        fastBuilder.SetInsertPoint(haveVariable);
        llvm::Value *listValue = fastBuilder.CreateStructGEP(
            variableTy,
            fastListVariable,
            5,
            "list_value");
        llvm::Value *list = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(svalueTy, listValue, 2),
            "list");
        llvm::Value *headersOk = fastBuilder.CreateAnd(
            fastBuilder.CreateICmpEQ(
                fastBuilder.CreateLoad(
                    i32,
                    fastBuilder.CreateStructGEP(variableTy, fastListVariable, 2)),
                llvm::ConstantInt::get(i32, SJIT_VAR_LIST)),
            fastBuilder.CreateAnd(
                fastBuilder.CreateICmpEQ(
                    fastBuilder.CreateLoad(
                        i32,
                        fastBuilder.CreateStructGEP(svalueTy, listValue, 0)),
                    llvm::ConstantInt::get(i32, SJIT_VALUE_LIST)),
                fastBuilder.CreateNot(isNull(fastBuilder, list))));
        fastBuilder.CreateCondBr(headersOk, haveList, fallback);

        fastBuilder.SetInsertPoint(haveList);
        llvm::Value *storage = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(listTy, list, 0),
            "storage");
        fastBuilder.CreateCondBr(isNull(fastBuilder, storage), fallback, haveStorage);

        fastBuilder.SetInsertPoint(haveStorage);
        llvm::Value *length = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(listStorageTy, storage, 1),
            "length");
        llvm::Value *items = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(listStorageTy, storage, 0),
            "items");
        llvm::Value *numbers = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(listStorageTy, storage, 4),
            "numbers");
        llvm::Value *numbersValid = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(listStorageTy, storage, 5)),
            llvm::ConstantInt::get(i32, 0),
            "numbers_valid");
        llvm::Value *inRange = fastBuilder.CreateAnd(
            fastBuilder.CreateFCmpOGE(fastIndex, llvm::ConstantFP::get(f64, 1.0)),
            fastBuilder.CreateFCmpOLT(
                fastIndex,
                fastBuilder.CreateFAdd(
                    fastBuilder.CreateSIToFP(length, f64),
                    llvm::ConstantFP::get(f64, 1.0))));
        llvm::Value *canWrite = fastBuilder.CreateAnd(
            inRange,
            fastBuilder.CreateAnd(
                fastBuilder.CreateICmpEQ(
                    fastBuilder.CreateLoad(
                        i32,
                        fastBuilder.CreateStructGEP(listStorageTy, storage, 3)),
                    llvm::ConstantInt::get(i32, 1)),
                fastBuilder.CreateAnd(
                    fastBuilder.CreateNot(isNull(fastBuilder, items)),
                    fastBuilder.CreateOr(
                        fastBuilder.CreateNot(numbersValid),
                        fastBuilder.CreateNot(isNull(fastBuilder, numbers))))));
        fastBuilder.CreateCondBr(canWrite, haveItem, fallback);

        fastBuilder.SetInsertPoint(haveItem);
        llvm::Value *zeroBased = fastBuilder.CreateSub(
            fastBuilder.CreateFPToSI(fastIndex, i32),
            llvm::ConstantInt::get(i32, 1));
        llvm::Value *item = fastBuilder.CreateInBoundsGEP(
            svalueTy,
            items,
            zeroBased,
            "item");
        fastBuilder.CreateCondBr(
            fastBuilder.CreateICmpEQ(
                fastBuilder.CreateLoad(
                    i32,
                    fastBuilder.CreateStructGEP(svalueTy, item, 0)),
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)),
            direct,
            fallback);

        fastBuilder.SetInsertPoint(direct);
        fastBuilder.CreateStore(
            llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER),
            fastBuilder.CreateStructGEP(svalueTy, item, 0));
        fastBuilder.CreateStore(
            fastValue,
            fastBuilder.CreateStructGEP(svalueTy, item, 1));
        fastBuilder.CreateStore(
            llvm::ConstantPointerNull::get(ptr),
            fastBuilder.CreateStructGEP(svalueTy, item, 2));
        fastBuilder.CreateCondBr(numbersValid, cache, done);

        fastBuilder.SetInsertPoint(cache);
        fastBuilder.CreateStore(
            fastValue,
            fastBuilder.CreateInBoundsGEP(f64, numbers, zeroBased));
        fastBuilder.CreateBr(done);

        fastBuilder.SetInsertPoint(fallback);
        fastBuilder.CreateCall(
            listVariableReplaceNumberAt,
            {fastRuntime, fastListVariable, fastIndex, fastValue});
        fastBuilder.CreateBr(done);

        fastBuilder.SetInsertPoint(done);
        fastBuilder.CreateRetVoid();
    }

    auto *fastListReplaceNumberAtVariable = llvm::Function::Create(
        listVariableReplaceNumberAtVariableTy,
        llvm::Function::InternalLinkage,
        name + "_fast_list_replace_number_at_variable",
        module.get());
    fastListReplaceNumberAtVariable->addFnAttr(llvm::Attribute::AlwaysInline);
    {
        auto arg = fastListReplaceNumberAtVariable->arg_begin();
        llvm::Value *fastRuntime = &*arg++;
        llvm::Value *listVariable = &*arg++;
        llvm::Value *indexVariable = &*arg++;
        llvm::Value *number = &*arg++;
        auto *entry = llvm::BasicBlock::Create(*context, "entry", fastListReplaceNumberAtVariable);
        auto *checkIndex = llvm::BasicBlock::Create(*context, "check_index", fastListReplaceNumberAtVariable);
        auto *direct = llvm::BasicBlock::Create(*context, "direct_number", fastListReplaceNumberAtVariable);
        auto *fallback = llvm::BasicBlock::Create(*context, "fallback", fastListReplaceNumberAtVariable);
        llvm::IRBuilder<> fastBuilder(entry);
        fastBuilder.CreateCondBr(
            isNull(fastBuilder, indexVariable),
            fallback,
            checkIndex);

        fastBuilder.SetInsertPoint(checkIndex);
        llvm::Value *indexValue = fastBuilder.CreateStructGEP(variableTy, indexVariable, 5);
        fastBuilder.CreateCondBr(
            fastBuilder.CreateICmpEQ(
                fastBuilder.CreateLoad(i32, fastBuilder.CreateStructGEP(svalueTy, indexValue, 0)),
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)),
            direct,
            fallback);

        fastBuilder.SetInsertPoint(direct);
        fastBuilder.CreateCall(
            fastListReplaceNumberAt,
            {
                fastRuntime,
                listVariable,
                fastBuilder.CreateLoad(f64, fastBuilder.CreateStructGEP(svalueTy, indexValue, 1)),
                number,
            });
        fastBuilder.CreateRetVoid();

        fastBuilder.SetInsertPoint(fallback);
        fastBuilder.CreateCall(
            listVariableReplaceNumberAtVariable,
            {fastRuntime, listVariable, indexVariable, number});
        fastBuilder.CreateRetVoid();
    }

    const bool directProcedureListReplace =
        std::getenv("SJIT_DISABLE_DIRECT_PROCEDURE_LIST_REPLACE") == nullptr;
    const bool inlineDirectProcedureListReplace =
        std::getenv("SJIT_INLINE_DIRECT_PROCEDURE_LIST_REPLACE") != nullptr;

    auto *fastListReplaceFromVariables = llvm::Function::Create(
        listVariableReplaceFromVariablesTy,
        llvm::Function::InternalLinkage,
        name + "_fast_list_replace_from_variables",
        module.get());
    fastListReplaceFromVariables->addFnAttr(
        inlineDirectProcedureListReplace ?
            llvm::Attribute::AlwaysInline : llvm::Attribute::NoInline);
    {
        auto arg = fastListReplaceFromVariables->arg_begin();
        llvm::Value *fastRuntime = &*arg++;
        llvm::Value *listVariable = &*arg++;
        llvm::Value *indexVariable = &*arg++;
        llvm::Value *valueVariable = &*arg++;
        auto *entry = llvm::BasicBlock::Create(*context, "entry", fastListReplaceFromVariables);
        auto *haveVariables = llvm::BasicBlock::Create(*context, "have_variables", fastListReplaceFromVariables);
        auto *haveList = llvm::BasicBlock::Create(*context, "have_list", fastListReplaceFromVariables);
        auto *haveStorage = llvm::BasicBlock::Create(*context, "have_storage", fastListReplaceFromVariables);
        auto *haveItem = llvm::BasicBlock::Create(*context, "have_item", fastListReplaceFromVariables);
        auto *direct = llvm::BasicBlock::Create(*context, "direct", fastListReplaceFromVariables);
        auto *cache = llvm::BasicBlock::Create(*context, "cache", fastListReplaceFromVariables);
        auto *fallback = llvm::BasicBlock::Create(*context, "fallback", fastListReplaceFromVariables);
        auto *done = llvm::BasicBlock::Create(*context, "done", fastListReplaceFromVariables);
        llvm::IRBuilder<> fastBuilder(entry);
        llvm::Value *missingVariable = fastBuilder.CreateOr(
            isNull(fastBuilder, listVariable),
            isNull(fastBuilder, indexVariable));
        missingVariable = fastBuilder.CreateOr(
            missingVariable,
            isNull(fastBuilder, valueVariable));
        fastBuilder.CreateCondBr(missingVariable, fallback, haveVariables);

        fastBuilder.SetInsertPoint(haveVariables);
        llvm::Value *listValue = fastBuilder.CreateStructGEP(variableTy, listVariable, 5, "list_value");
        llvm::Value *indexValue = fastBuilder.CreateStructGEP(variableTy, indexVariable, 5, "index_value");
        llvm::Value *sourceValue = fastBuilder.CreateStructGEP(variableTy, valueVariable, 5, "source_value");
        llvm::Value *fastList = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(svalueTy, listValue, 2),
            "list");
        llvm::Value *headersOk = fastBuilder.CreateICmpEQ(
            fastBuilder.CreateLoad(i32, fastBuilder.CreateStructGEP(variableTy, listVariable, 2)),
            llvm::ConstantInt::get(i32, SJIT_VAR_LIST));
        headersOk = fastBuilder.CreateAnd(
            headersOk,
            fastBuilder.CreateICmpEQ(
                fastBuilder.CreateLoad(i32, fastBuilder.CreateStructGEP(svalueTy, listValue, 0)),
                llvm::ConstantInt::get(i32, SJIT_VALUE_LIST)));
        headersOk = fastBuilder.CreateAnd(
            headersOk,
            fastBuilder.CreateICmpEQ(
                fastBuilder.CreateLoad(i32, fastBuilder.CreateStructGEP(svalueTy, indexValue, 0)),
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)));
        headersOk = fastBuilder.CreateAnd(
            headersOk,
            fastBuilder.CreateICmpEQ(
                fastBuilder.CreateLoad(i32, fastBuilder.CreateStructGEP(svalueTy, sourceValue, 0)),
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)));
        headersOk = fastBuilder.CreateAnd(
            headersOk,
            fastBuilder.CreateNot(isNull(fastBuilder, fastList)));
        fastBuilder.CreateCondBr(headersOk, haveList, fallback);

        fastBuilder.SetInsertPoint(haveList);
        llvm::Value *storage = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(listTy, fastList, 0),
            "storage");
        fastBuilder.CreateCondBr(isNull(fastBuilder, storage), fallback, haveStorage);

        fastBuilder.SetInsertPoint(haveStorage);
        llvm::Value *length = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(listStorageTy, storage, 1),
            "length");
        llvm::Value *items = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(listStorageTy, storage, 0),
            "items");
        llvm::Value *numbers = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(listStorageTy, storage, 4),
            "numbers");
        llvm::Value *numbersValid = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(listStorageTy, storage, 5)),
            llvm::ConstantInt::get(i32, 0),
            "numbers_valid");
        llvm::Value *indexNumber = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(svalueTy, indexValue, 1),
            "index");
        llvm::Value *valueNumber = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(svalueTy, sourceValue, 1),
            "value");
        llvm::Value *lengthLimit = fastBuilder.CreateFAdd(
            fastBuilder.CreateSIToFP(length, f64),
            llvm::ConstantFP::get(f64, 1.0),
            "length_limit");
        llvm::Value *canWrite = fastBuilder.CreateAnd(
            fastBuilder.CreateFCmpOGE(indexNumber, llvm::ConstantFP::get(f64, 1.0)),
            fastBuilder.CreateFCmpOLT(indexNumber, lengthLimit),
            "index_in_range");
        canWrite = fastBuilder.CreateAnd(
            canWrite,
            fastBuilder.CreateICmpEQ(
                fastBuilder.CreateLoad(
                    i32,
                    fastBuilder.CreateStructGEP(listStorageTy, storage, 3)),
                llvm::ConstantInt::get(i32, 1)),
            "list_unique");
        canWrite = fastBuilder.CreateAnd(
            canWrite,
            fastBuilder.CreateNot(isNull(fastBuilder, items)),
            "items_ready");
        canWrite = fastBuilder.CreateAnd(
            canWrite,
            fastBuilder.CreateOr(
                fastBuilder.CreateNot(numbersValid),
                fastBuilder.CreateNot(isNull(fastBuilder, numbers))),
            "cache_ready");
        fastBuilder.CreateCondBr(canWrite, haveItem, fallback);

        fastBuilder.SetInsertPoint(haveItem);
        llvm::Value *zeroBased = fastBuilder.CreateSub(
            fastBuilder.CreateFPToSI(indexNumber, i32),
            llvm::ConstantInt::get(i32, 1),
            "zero_based");
        llvm::Value *item = fastBuilder.CreateInBoundsGEP(
            svalueTy,
            items,
            zeroBased,
            "item");
        fastBuilder.CreateCondBr(
            fastBuilder.CreateICmpEQ(
                fastBuilder.CreateLoad(i32, fastBuilder.CreateStructGEP(svalueTy, item, 0)),
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)),
            direct,
            fallback);

        fastBuilder.SetInsertPoint(direct);
        fastBuilder.CreateStore(
            valueNumber,
            fastBuilder.CreateStructGEP(svalueTy, item, 1));
        fastBuilder.CreateStore(
            llvm::ConstantPointerNull::get(ptr),
            fastBuilder.CreateStructGEP(svalueTy, item, 2));
        fastBuilder.CreateCondBr(numbersValid, cache, done);

        fastBuilder.SetInsertPoint(cache);
        fastBuilder.CreateStore(
            valueNumber,
            fastBuilder.CreateInBoundsGEP(f64, numbers, zeroBased));
        fastBuilder.CreateBr(done);

        fastBuilder.SetInsertPoint(fallback);
        fastBuilder.CreateCall(
            listVariableReplaceFromVariables,
            {fastRuntime, listVariable, indexVariable, valueVariable});
        fastBuilder.CreateBr(done);

        fastBuilder.SetInsertPoint(done);
        fastBuilder.CreateRetVoid();
    }

    auto *fastListReplaceListItemAtVariables = llvm::Function::Create(
        listVariableReplaceListItemAtVariablesTy,
        llvm::Function::InternalLinkage,
        name + "_fast_list_replace_list_item_at_variables",
        module.get());
    fastListReplaceListItemAtVariables->addFnAttr(
        inlineDirectProcedureListReplace ?
            llvm::Attribute::AlwaysInline : llvm::Attribute::NoInline);
    {
        auto arg = fastListReplaceListItemAtVariables->arg_begin();
        llvm::Value *fastRuntime = &*arg++;
        llvm::Value *listVariable = &*arg++;
        llvm::Value *indexVariable = &*arg++;
        llvm::Value *sourceListVariable = &*arg++;
        llvm::Value *sourceIndexVariable = &*arg++;
        auto *entry = llvm::BasicBlock::Create(*context, "entry", fastListReplaceListItemAtVariables);
        auto *haveVariables = llvm::BasicBlock::Create(*context, "have_variables", fastListReplaceListItemAtVariables);
        auto *haveLists = llvm::BasicBlock::Create(*context, "have_lists", fastListReplaceListItemAtVariables);
        auto *haveStorage = llvm::BasicBlock::Create(*context, "have_storage", fastListReplaceListItemAtVariables);
        auto *direct = llvm::BasicBlock::Create(*context, "direct", fastListReplaceListItemAtVariables);
        auto *cache = llvm::BasicBlock::Create(*context, "cache", fastListReplaceListItemAtVariables);
        auto *fallback = llvm::BasicBlock::Create(*context, "fallback", fastListReplaceListItemAtVariables);
        auto *done = llvm::BasicBlock::Create(*context, "done", fastListReplaceListItemAtVariables);
        llvm::IRBuilder<> fastBuilder(entry);
        llvm::Value *missingVariable = fastBuilder.CreateOr(
            isNull(fastBuilder, listVariable),
            isNull(fastBuilder, indexVariable));
        missingVariable = fastBuilder.CreateOr(
            missingVariable,
            isNull(fastBuilder, sourceListVariable));
        missingVariable = fastBuilder.CreateOr(
            missingVariable,
            isNull(fastBuilder, sourceIndexVariable));
        fastBuilder.CreateCondBr(missingVariable, fallback, haveVariables);

        fastBuilder.SetInsertPoint(haveVariables);
        llvm::Value *listValue = fastBuilder.CreateStructGEP(variableTy, listVariable, 5, "list_value");
        llvm::Value *indexValue = fastBuilder.CreateStructGEP(variableTy, indexVariable, 5, "index_value");
        llvm::Value *sourceListValue = fastBuilder.CreateStructGEP(
            variableTy,
            sourceListVariable,
            5,
            "source_list_value");
        llvm::Value *sourceIndexValue = fastBuilder.CreateStructGEP(
            variableTy,
            sourceIndexVariable,
            5,
            "source_index_value");
        llvm::Value *fastList = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(svalueTy, listValue, 2),
            "list");
        llvm::Value *sourceList = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(svalueTy, sourceListValue, 2),
            "source_list");
        llvm::Value *headersOk = fastBuilder.CreateICmpEQ(
            fastBuilder.CreateLoad(i32, fastBuilder.CreateStructGEP(variableTy, listVariable, 2)),
            llvm::ConstantInt::get(i32, SJIT_VAR_LIST));
        headersOk = fastBuilder.CreateAnd(
            headersOk,
            fastBuilder.CreateICmpEQ(
                fastBuilder.CreateLoad(i32, fastBuilder.CreateStructGEP(variableTy, sourceListVariable, 2)),
                llvm::ConstantInt::get(i32, SJIT_VAR_LIST)));
        headersOk = fastBuilder.CreateAnd(
            headersOk,
            fastBuilder.CreateICmpEQ(
                fastBuilder.CreateLoad(i32, fastBuilder.CreateStructGEP(svalueTy, listValue, 0)),
                llvm::ConstantInt::get(i32, SJIT_VALUE_LIST)));
        headersOk = fastBuilder.CreateAnd(
            headersOk,
            fastBuilder.CreateICmpEQ(
                fastBuilder.CreateLoad(i32, fastBuilder.CreateStructGEP(svalueTy, sourceListValue, 0)),
                llvm::ConstantInt::get(i32, SJIT_VALUE_LIST)));
        headersOk = fastBuilder.CreateAnd(
            headersOk,
            fastBuilder.CreateICmpEQ(
                fastBuilder.CreateLoad(i32, fastBuilder.CreateStructGEP(svalueTy, indexValue, 0)),
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)));
        headersOk = fastBuilder.CreateAnd(
            headersOk,
            fastBuilder.CreateICmpEQ(
                fastBuilder.CreateLoad(i32, fastBuilder.CreateStructGEP(svalueTy, sourceIndexValue, 0)),
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)));
        headersOk = fastBuilder.CreateAnd(headersOk, fastBuilder.CreateNot(isNull(fastBuilder, fastList)));
        headersOk = fastBuilder.CreateAnd(headersOk, fastBuilder.CreateNot(isNull(fastBuilder, sourceList)));
        fastBuilder.CreateCondBr(headersOk, haveLists, fallback);

        fastBuilder.SetInsertPoint(haveLists);
        llvm::Value *storage = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(listTy, fastList, 0),
            "storage");
        llvm::Value *sourceStorage = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(listTy, sourceList, 0),
            "source_storage");
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                fastBuilder.CreateNot(isNull(fastBuilder, storage)),
                fastBuilder.CreateNot(isNull(fastBuilder, sourceStorage))),
            haveStorage,
            fallback);

        fastBuilder.SetInsertPoint(haveStorage);
        llvm::Value *length = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(listStorageTy, storage, 1),
            "length");
        llvm::Value *sourceLength = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(listStorageTy, sourceStorage, 1),
            "source_length");
        llvm::Value *items = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(listStorageTy, storage, 0),
            "items");
        llvm::Value *numbers = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(listStorageTy, storage, 4),
            "numbers");
        llvm::Value *sourceNumbers = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(listStorageTy, sourceStorage, 4),
            "source_numbers");
        llvm::Value *numbersValid = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(i32, fastBuilder.CreateStructGEP(listStorageTy, storage, 5)),
            llvm::ConstantInt::get(i32, 0),
            "numbers_valid");
        llvm::Value *sourceNumbersValid = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(i32, fastBuilder.CreateStructGEP(listStorageTy, sourceStorage, 5)),
            llvm::ConstantInt::get(i32, 0),
            "source_numbers_valid");
        llvm::Value *indexNumber = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(svalueTy, indexValue, 1),
            "index");
        llvm::Value *sourceIndexNumber = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(svalueTy, sourceIndexValue, 1),
            "source_index");
        llvm::Value *lengthLimit = fastBuilder.CreateFAdd(
            fastBuilder.CreateSIToFP(length, f64),
            llvm::ConstantFP::get(f64, 1.0));
        llvm::Value *sourceLengthLimit = fastBuilder.CreateFAdd(
            fastBuilder.CreateSIToFP(sourceLength, f64),
            llvm::ConstantFP::get(f64, 1.0));
        llvm::Value *canWrite = fastBuilder.CreateAnd(
            fastBuilder.CreateFCmpOGE(indexNumber, llvm::ConstantFP::get(f64, 1.0)),
            fastBuilder.CreateFCmpOLT(indexNumber, lengthLimit));
        canWrite = fastBuilder.CreateAnd(
            canWrite,
            fastBuilder.CreateFCmpOGE(sourceIndexNumber, llvm::ConstantFP::get(f64, 1.0)));
        canWrite = fastBuilder.CreateAnd(
            canWrite,
            fastBuilder.CreateFCmpOLT(sourceIndexNumber, sourceLengthLimit));
        canWrite = fastBuilder.CreateAnd(
            canWrite,
            fastBuilder.CreateICmpEQ(
                fastBuilder.CreateLoad(i32, fastBuilder.CreateStructGEP(listStorageTy, storage, 3)),
                llvm::ConstantInt::get(i32, 1)));
        canWrite = fastBuilder.CreateAnd(canWrite, fastBuilder.CreateNot(isNull(fastBuilder, items)));
        canWrite = fastBuilder.CreateAnd(canWrite, numbersValid);
        canWrite = fastBuilder.CreateAnd(canWrite, fastBuilder.CreateNot(isNull(fastBuilder, numbers)));
        canWrite = fastBuilder.CreateAnd(canWrite, sourceNumbersValid);
        canWrite = fastBuilder.CreateAnd(canWrite, fastBuilder.CreateNot(isNull(fastBuilder, sourceNumbers)));
        fastBuilder.CreateCondBr(canWrite, direct, fallback);

        fastBuilder.SetInsertPoint(direct);
        llvm::Value *zeroBased = fastBuilder.CreateSub(
            fastBuilder.CreateFPToSI(indexNumber, i32),
            llvm::ConstantInt::get(i32, 1));
        llvm::Value *sourceZeroBased = fastBuilder.CreateSub(
            fastBuilder.CreateFPToSI(sourceIndexNumber, i32),
            llvm::ConstantInt::get(i32, 1));
        llvm::Value *valueNumber = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateInBoundsGEP(f64, sourceNumbers, sourceZeroBased),
            "value");
        llvm::Value *item = fastBuilder.CreateInBoundsGEP(svalueTy, items, zeroBased, "item");
        fastBuilder.CreateStore(
            llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER),
            fastBuilder.CreateStructGEP(svalueTy, item, 0));
        fastBuilder.CreateStore(valueNumber, fastBuilder.CreateStructGEP(svalueTy, item, 1));
        fastBuilder.CreateStore(
            llvm::ConstantPointerNull::get(ptr),
            fastBuilder.CreateStructGEP(svalueTy, item, 2));
        fastBuilder.CreateCondBr(numbersValid, cache, done);

        fastBuilder.SetInsertPoint(cache);
        fastBuilder.CreateStore(
            valueNumber,
            fastBuilder.CreateInBoundsGEP(f64, numbers, zeroBased));
        fastBuilder.CreateBr(done);

        fastBuilder.SetInsertPoint(fallback);
        fastBuilder.CreateCall(
            listVariableReplaceListItemAtVariables,
            {fastRuntime, listVariable, indexVariable, sourceListVariable, sourceIndexVariable});
        fastBuilder.CreateBr(done);

        fastBuilder.SetInsertPoint(done);
        fastBuilder.CreateRetVoid();
    }

    auto *fastListValueAtNumber = llvm::Function::Create(
        listVariableItemValueAtNumberTy,
        llvm::Function::InternalLinkage,
        name + "_fast_list_value_at_number",
        module.get());
    fastListValueAtNumber->addFnAttr(llvm::Attribute::AlwaysInline);
    {
        auto fastArg = fastListValueAtNumber->arg_begin();
        llvm::Value *fastRuntime = &*fastArg++;
        llvm::Value *fastVariable = &*fastArg++;
        llvm::Value *fastKnownList = &*fastArg++;
        llvm::Value *fastIndex = &*fastArg++;
        llvm::Value *fastOut = &*fastArg++;
        auto *fastEntry = llvm::BasicBlock::Create(*context, "entry", fastListValueAtNumber);
        auto *fastChooseList = llvm::BasicBlock::Create(*context, "choose_list", fastListValueAtNumber);
        auto *fastCheckVariable = llvm::BasicBlock::Create(*context, "check_variable", fastListValueAtNumber);
        auto *fastHaveVariable = llvm::BasicBlock::Create(*context, "have_variable", fastListValueAtNumber);
        auto *fastHaveList = llvm::BasicBlock::Create(*context, "have_list", fastListValueAtNumber);
        auto *fastHaveStorage = llvm::BasicBlock::Create(*context, "have_storage", fastListValueAtNumber);
        auto *fastReadItem = llvm::BasicBlock::Create(*context, "read_item", fastListValueAtNumber);
        auto *fastMixedItem = llvm::BasicBlock::Create(*context, "mixed_item", fastListValueAtNumber);
        auto *fastMixedLoadItem = llvm::BasicBlock::Create(*context, "mixed_load_item", fastListValueAtNumber);
        auto *fastMixedStringCheck = llvm::BasicBlock::Create(*context, "mixed_string_check", fastListValueAtNumber);
        auto *fastMixedString = llvm::BasicBlock::Create(*context, "mixed_string", fastListValueAtNumber);
        auto *fastMixedScalarCheck = llvm::BasicBlock::Create(*context, "mixed_scalar_check", fastListValueAtNumber);
        auto *fastMixedScalar = llvm::BasicBlock::Create(*context, "mixed_scalar", fastListValueAtNumber);
        auto *fastCached = llvm::BasicBlock::Create(*context, "cached", fastListValueAtNumber);
        auto *fastFallback = llvm::BasicBlock::Create(*context, "fallback", fastListValueAtNumber);
        llvm::IRBuilder<> fastBuilder(fastEntry);
        fastBuilder.CreateCondBr(
            isNull(fastBuilder, fastOut),
            fastFallback,
            fastChooseList);

        fastBuilder.SetInsertPoint(fastChooseList);
        fastBuilder.CreateCondBr(
            isNull(fastBuilder, fastKnownList),
            fastCheckVariable,
            fastHaveList);

        fastBuilder.SetInsertPoint(fastCheckVariable);
        fastBuilder.CreateCondBr(
            isNull(fastBuilder, fastVariable),
            fastFallback,
            fastHaveVariable);

        fastBuilder.SetInsertPoint(fastHaveVariable);
        llvm::Value *fastValue = fastBuilder.CreateStructGEP(variableTy, fastVariable, 5, "value_ptr");
        llvm::Value *fastTag = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(svalueTy, fastValue, 0),
            "value_tag");
        llvm::Value *fastLoadedList = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(svalueTy, fastValue, 2),
            "list");
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                fastBuilder.CreateICmpEQ(fastTag, llvm::ConstantInt::get(i32, SJIT_VALUE_LIST)),
                fastBuilder.CreateNot(isNull(fastBuilder, fastLoadedList))),
            fastHaveList,
            fastFallback);

        fastBuilder.SetInsertPoint(fastHaveList);
        auto *fastList = fastBuilder.CreatePHI(ptr, 2, "selected_list");
        fastList->addIncoming(fastKnownList, fastChooseList);
        fastList->addIncoming(fastLoadedList, fastHaveVariable);
        llvm::Value *fastStorage = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(listTy, fastList, 0),
            "storage");
        fastBuilder.CreateCondBr(
            isNull(fastBuilder, fastStorage),
            fastFallback,
            fastHaveStorage);

        fastBuilder.SetInsertPoint(fastHaveStorage);
        llvm::Value *fastLength = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(listStorageTy, fastStorage, 1),
            "length");
        llvm::Value *fastNumbers = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(listStorageTy, fastStorage, 4),
            "numbers");
        llvm::Value *fastItems = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(listStorageTy, fastStorage, 0),
            "items");
        llvm::Value *fastNumbersValid = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(listStorageTy, fastStorage, 5)),
            llvm::ConstantInt::get(i32, 0),
            "numbers_valid");
        llvm::Value *fastLimit = fastBuilder.CreateFAdd(
            fastBuilder.CreateSIToFP(fastLength, f64),
            llvm::ConstantFP::get(f64, 1.0),
            "index_limit");
        llvm::Value *fastInRange = fastBuilder.CreateAnd(
            fastBuilder.CreateFCmpOGE(fastIndex, llvm::ConstantFP::get(f64, 1.0)),
            fastBuilder.CreateFCmpOLT(fastIndex, fastLimit),
            "index_in_range");
        llvm::Value *fastCanRead = fastBuilder.CreateAnd(
            fastNumbersValid,
            fastBuilder.CreateNot(isNull(fastBuilder, fastNumbers)));
        llvm::Value *fastZeroBased = fastBuilder.CreateSub(
            fastBuilder.CreateFPToSI(fastIndex, i32),
            llvm::ConstantInt::get(i32, 1));
        fastBuilder.CreateCondBr(
            fastInRange,
            fastReadItem,
            fastFallback);

        fastBuilder.SetInsertPoint(fastReadItem);
        fastBuilder.CreateCondBr(fastCanRead, fastCached, fastMixedItem);

        fastBuilder.SetInsertPoint(fastMixedItem);
        fastBuilder.CreateCondBr(
            isNull(fastBuilder, fastItems),
            fastFallback,
            fastMixedLoadItem);

        fastBuilder.SetInsertPoint(fastMixedLoadItem);
        llvm::Value *fastItem = fastBuilder.CreateInBoundsGEP(
            svalueTy,
            fastItems,
            fastZeroBased,
            "item");
        llvm::Value *fastItemTag = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(svalueTy, fastItem, 0),
            "item_tag");
        llvm::Value *fastItemNumber = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateStructGEP(svalueTy, fastItem, 1),
            "item_number");
        llvm::Value *fastItemPointer = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(svalueTy, fastItem, 2),
            "item_pointer");
        fastBuilder.CreateCondBr(
            fastBuilder.CreateICmpEQ(
                fastItemTag,
                llvm::ConstantInt::get(i32, SJIT_VALUE_STRING)),
            fastMixedStringCheck,
            fastMixedScalarCheck);

        fastBuilder.SetInsertPoint(fastMixedStringCheck);
        fastBuilder.CreateCondBr(
            fastBuilder.CreateNot(isNull(fastBuilder, fastItemPointer)),
            fastMixedString,
            fastFallback);

        fastBuilder.SetInsertPoint(fastMixedString);
        llvm::Value *stringRefCount = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(stringTy, fastItemPointer, 2),
            "string_ref_count");
        fastBuilder.CreateStore(
            fastBuilder.CreateAdd(stringRefCount, llvm::ConstantInt::get(i32, 1)),
            fastBuilder.CreateStructGEP(stringTy, fastItemPointer, 2));
        fastBuilder.CreateStore(
            fastItemTag,
            fastBuilder.CreateStructGEP(svalueTy, fastOut, 0));
        fastBuilder.CreateStore(
            fastItemNumber,
            fastBuilder.CreateStructGEP(svalueTy, fastOut, 1));
        fastBuilder.CreateStore(
            fastItemPointer,
            fastBuilder.CreateStructGEP(svalueTy, fastOut, 2));
        fastBuilder.CreateRetVoid();

        fastBuilder.SetInsertPoint(fastMixedScalarCheck);
        llvm::Value *fastItemIsNumber = fastBuilder.CreateICmpEQ(
            fastItemTag,
            llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER));
        llvm::Value *fastItemIsBool = fastBuilder.CreateICmpEQ(
            fastItemTag,
            llvm::ConstantInt::get(i32, SJIT_VALUE_BOOL));
        fastBuilder.CreateCondBr(
            fastBuilder.CreateOr(fastItemIsNumber, fastItemIsBool),
            fastMixedScalar,
            fastFallback);

        fastBuilder.SetInsertPoint(fastMixedScalar);
        fastBuilder.CreateStore(
            fastItemTag,
            fastBuilder.CreateStructGEP(svalueTy, fastOut, 0));
        fastBuilder.CreateStore(
            fastItemNumber,
            fastBuilder.CreateStructGEP(svalueTy, fastOut, 1));
        fastBuilder.CreateStore(
            fastItemPointer,
            fastBuilder.CreateStructGEP(svalueTy, fastOut, 2));
        fastBuilder.CreateRetVoid();

        fastBuilder.SetInsertPoint(fastCached);
        llvm::Value *fastCachedNumber = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateInBoundsGEP(f64, fastNumbers, fastZeroBased),
            "number");
        fastBuilder.CreateStore(
            llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER),
            fastBuilder.CreateStructGEP(svalueTy, fastOut, 0));
        fastBuilder.CreateStore(
            fastCachedNumber,
            fastBuilder.CreateStructGEP(svalueTy, fastOut, 1));
        fastBuilder.CreateStore(
            llvm::ConstantPointerNull::get(ptr),
            fastBuilder.CreateStructGEP(svalueTy, fastOut, 2));
        fastBuilder.CreateRetVoid();

        fastBuilder.SetInsertPoint(fastFallback);
        fastBuilder.CreateCall(
            listVariableItemValueAtNumber,
            {fastRuntime, fastVariable, fastIndex, fastOut});
        fastBuilder.CreateRetVoid();
    }

    auto *fastVariableSetFromNumericListItem = llvm::Function::Create(
        llvm::FunctionType::get(i1, {ptr, ptr, f64}, false),
        llvm::Function::InternalLinkage,
        name + "_fast_variable_set_from_numeric_list_item",
        module.get());
    fastVariableSetFromNumericListItem->addFnAttr(llvm::Attribute::AlwaysInline);
    {
        auto fastArg = fastVariableSetFromNumericListItem->arg_begin();
        llvm::Value *fastDestination = &*fastArg++;
        llvm::Value *fastListVariable = &*fastArg++;
        llvm::Value *fastIndex = &*fastArg++;
        auto *fastEntry = llvm::BasicBlock::Create(*context, "entry", fastVariableSetFromNumericListItem);
        auto *fastHaveVariables = llvm::BasicBlock::Create(*context, "have_variables", fastVariableSetFromNumericListItem);
        auto *fastHaveList = llvm::BasicBlock::Create(*context, "have_list", fastVariableSetFromNumericListItem);
        auto *fastHaveStorage = llvm::BasicBlock::Create(*context, "have_storage", fastVariableSetFromNumericListItem);
        auto *fastCached = llvm::BasicBlock::Create(*context, "cached", fastVariableSetFromNumericListItem);
        auto *fastMiss = llvm::BasicBlock::Create(*context, "miss", fastVariableSetFromNumericListItem);
        llvm::IRBuilder<> fastBuilder(fastEntry);
        fastBuilder.CreateCondBr(
            fastBuilder.CreateOr(
                isNull(fastBuilder, fastDestination),
                isNull(fastBuilder, fastListVariable)),
            fastMiss,
            fastHaveVariables);

        fastBuilder.SetInsertPoint(fastHaveVariables);
        llvm::Value *fastDestinationValue = fastBuilder.CreateStructGEP(
            variableTy,
            fastDestination,
            5,
            "destination_value");
        llvm::Value *fastListValue = fastBuilder.CreateStructGEP(
            variableTy,
            fastListVariable,
            5,
            "list_value");
        llvm::Value *fastDestinationIsNumber = fastBuilder.CreateICmpEQ(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(svalueTy, fastDestinationValue, 0)),
            llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER),
            "destination_is_number");
        llvm::Value *fastListIsList = fastBuilder.CreateICmpEQ(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(svalueTy, fastListValue, 0)),
            llvm::ConstantInt::get(i32, SJIT_VALUE_LIST),
            "list_is_list");
        llvm::Value *fastList = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(svalueTy, fastListValue, 2),
            "list");
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(
                fastDestinationIsNumber,
                fastBuilder.CreateAnd(
                    fastListIsList,
                    fastBuilder.CreateNot(isNull(fastBuilder, fastList)))),
            fastHaveList,
            fastMiss);

        fastBuilder.SetInsertPoint(fastHaveList);
        llvm::Value *fastStorage = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(listTy, fastList, 0),
            "storage");
        fastBuilder.CreateCondBr(
            isNull(fastBuilder, fastStorage),
            fastMiss,
            fastHaveStorage);

        fastBuilder.SetInsertPoint(fastHaveStorage);
        llvm::Value *fastLength = fastBuilder.CreateLoad(
            i32,
            fastBuilder.CreateStructGEP(listStorageTy, fastStorage, 1),
            "length");
        llvm::Value *fastNumbers = fastBuilder.CreateLoad(
            ptr,
            fastBuilder.CreateStructGEP(listStorageTy, fastStorage, 4),
            "numbers");
        llvm::Value *fastNumbersValid = fastBuilder.CreateICmpNE(
            fastBuilder.CreateLoad(
                i32,
                fastBuilder.CreateStructGEP(listStorageTy, fastStorage, 5)),
            llvm::ConstantInt::get(i32, 0),
            "numbers_valid");
        llvm::Value *fastLimit = fastBuilder.CreateFAdd(
            fastBuilder.CreateSIToFP(fastLength, f64),
            llvm::ConstantFP::get(f64, 1.0),
            "index_limit");
        llvm::Value *fastInRange = fastBuilder.CreateAnd(
            fastBuilder.CreateFCmpOGE(fastIndex, llvm::ConstantFP::get(f64, 1.0)),
            fastBuilder.CreateFCmpOLT(fastIndex, fastLimit),
            "index_in_range");
        llvm::Value *fastCanRead = fastBuilder.CreateAnd(
            fastNumbersValid,
            fastBuilder.CreateNot(isNull(fastBuilder, fastNumbers)));
        fastBuilder.CreateCondBr(
            fastBuilder.CreateAnd(fastCanRead, fastInRange),
            fastCached,
            fastMiss);

        fastBuilder.SetInsertPoint(fastCached);
        llvm::Value *fastZeroBased = fastBuilder.CreateSub(
            fastBuilder.CreateFPToSI(fastIndex, i32),
            llvm::ConstantInt::get(i32, 1));
        llvm::Value *fastNumber = fastBuilder.CreateLoad(
            f64,
            fastBuilder.CreateInBoundsGEP(f64, fastNumbers, fastZeroBased),
            "number");
        fastBuilder.CreateStore(
            fastNumber,
            fastBuilder.CreateStructGEP(svalueTy, fastDestinationValue, 1));
        fastBuilder.CreateRet(llvm::ConstantInt::getTrue(*context));

        fastBuilder.SetInsertPoint(fastMiss);
        fastBuilder.CreateRet(llvm::ConstantInt::getFalse(*context));
    }

    std::vector<bool> directProcedures = findJitProcedureEligibility(script);
    std::vector<bool> statelessWarpProcedures = findStatelessWarpProcedureEligibility(script, directProcedures);
    std::vector<std::vector<bool>> procedureArgumentNeedsValue =
        findProcedureArgumentValueRequirements(script);
    const bool directSimpleMathops =
        std::getenv("SJIT_DISABLE_DIRECT_SIMPLE_MATHOP") == nullptr;
    const bool directProcedureListNumbers =
        std::getenv("SJIT_DISABLE_DIRECT_PROCEDURE_LIST_NUMBER") == nullptr;
    const bool directProcedureListValues =
        std::getenv("SJIT_DISABLE_DIRECT_PROCEDURE_LIST_VALUE") == nullptr;
    const bool directNumericLiteralComparisons =
        std::getenv("SJIT_DISABLE_DIRECT_NUMERIC_LITERAL_COMPARE") == nullptr;
    const bool directCachedComparisons =
        std::getenv("SJIT_DISABLE_DIRECT_CACHED_COMPARE") == nullptr;
    const bool directProcedureVariableNumbers =
        std::getenv("SJIT_DISABLE_DIRECT_PROCEDURE_VARIABLE_NUMBER") == nullptr;
    const bool borrowedCompareArguments =
        std::getenv("SJIT_DISABLE_BORROWED_COMPARE_ARGUMENT") == nullptr;
    const bool preboundListPointers =
        std::getenv("SJIT_DISABLE_PREBOUND_LIST_POINTER") == nullptr;
    auto emitDirectSimpleMathop = [&](llvm::IRBuilder<> &activeBuilder,
                                      const SExpr *expr,
                                      llvm::Value *value,
                                      const char *resultName) -> llvm::Value * {
        if (!directSimpleMathops || !expr || expr->literal.tag != SJIT_VALUE_STRING) {
            return nullptr;
        }
        const int operatorId = sjit_op_mathop_id(
            sjit_string_cstr(static_cast<const SString *>(expr->literal.ptr)));
        llvm::Intrinsic::ID intrinsic = llvm::Intrinsic::not_intrinsic;
        switch (operatorId) {
        case 1:
            intrinsic = llvm::Intrinsic::fabs;
            break;
        case 2:
            intrinsic = llvm::Intrinsic::floor;
            break;
        case 3:
            intrinsic = llvm::Intrinsic::ceil;
            break;
        case 4:
            intrinsic = llvm::Intrinsic::sqrt;
            break;
        case 5:
        case 6: {
            const llvm::Intrinsic::ID trigIntrinsic = operatorId == 5 ?
                llvm::Intrinsic::sin : llvm::Intrinsic::cos;
            llvm::Value *radians = activeBuilder.CreateFMul(
                value,
                llvm::ConstantFP::get(f64, 3.14159265358979323846 / 180.0),
                "mathop_radians");
            llvm::Value *trig = activeBuilder.CreateUnaryIntrinsic(trigIntrinsic, radians);
            llvm::Value *scaled = activeBuilder.CreateFMul(
                trig,
                llvm::ConstantFP::get(f64, 10000000000.0),
                "mathop_round_scaled");
            llvm::Value *rounded = activeBuilder.CreateUnaryIntrinsic(llvm::Intrinsic::round, scaled);
            llvm::Value *result = activeBuilder.CreateFDiv(
                rounded,
                llvm::ConstantFP::get(f64, 10000000000.0),
                resultName);
            return result;
        }
        case 7: {
            llvm::Value *angle = activeBuilder.CreateFRem(
                value,
                llvm::ConstantFP::get(f64, 360.0),
                "mathop_tan_angle");
            llvm::Value *radians = activeBuilder.CreateFMul(
                angle,
                llvm::ConstantFP::get(f64, 3.14159265358979323846 / 180.0),
                "mathop_tan_radians");
            llvm::Value *tangent = activeBuilder.CreateUnaryIntrinsic(llvm::Intrinsic::tan, radians);
            llvm::Value *scaled = activeBuilder.CreateFMul(
                tangent,
                llvm::ConstantFP::get(f64, 10000000000.0),
                "mathop_tan_round_scaled");
            llvm::Value *rounded = activeBuilder.CreateUnaryIntrinsic(llvm::Intrinsic::round, scaled);
            llvm::Value *finiteResult = activeBuilder.CreateFDiv(
                rounded,
                llvm::ConstantFP::get(f64, 10000000000.0),
                "mathop_tan_rounded");
            llvm::Value *positiveInfinity = activeBuilder.CreateOr(
                activeBuilder.CreateFCmpOEQ(angle, llvm::ConstantFP::get(f64, 90.0)),
                activeBuilder.CreateFCmpOEQ(angle, llvm::ConstantFP::get(f64, -270.0)));
            llvm::Value *negativeInfinity = activeBuilder.CreateOr(
                activeBuilder.CreateFCmpOEQ(angle, llvm::ConstantFP::get(f64, -90.0)),
                activeBuilder.CreateFCmpOEQ(angle, llvm::ConstantFP::get(f64, 270.0)));
            llvm::Value *withPositiveInfinity = activeBuilder.CreateSelect(
                positiveInfinity,
                llvm::ConstantFP::getInfinity(f64),
                finiteResult);
            return activeBuilder.CreateSelect(
                negativeInfinity,
                llvm::ConstantFP::getInfinity(f64, true),
                withPositiveInfinity,
                resultName);
        }
        case 8:
        case 9:
        case 10: {
            const llvm::Intrinsic::ID inverseIntrinsic = operatorId == 8 ?
                llvm::Intrinsic::asin :
                (operatorId == 9 ? llvm::Intrinsic::acos : llvm::Intrinsic::atan);
            llvm::Value *radians = activeBuilder.CreateUnaryIntrinsic(inverseIntrinsic, value);
            return activeBuilder.CreateFMul(
                radians,
                llvm::ConstantFP::get(f64, 180.0 / 3.14159265358979323846),
                resultName);
        }
        case 11:
            intrinsic = llvm::Intrinsic::log;
            break;
        case 12:
            intrinsic = llvm::Intrinsic::log10;
            break;
        case 13:
            intrinsic = llvm::Intrinsic::exp;
            break;
        case 14:
            return activeBuilder.CreateBinaryIntrinsic(
                llvm::Intrinsic::pow,
                llvm::ConstantFP::get(f64, 10.0),
                value,
                nullptr,
                resultName);
        default:
            return nullptr;
        }
        llvm::Value *result = activeBuilder.CreateUnaryIntrinsic(intrinsic, value);
        result->setName(resultName);
        return result;
    };
    std::vector<llvm::Function *> procedureFunctions(static_cast<size_t>(script.procedure_count), nullptr);
    for (int i = 0; i < script.procedure_count; ++i) {
        if (!directProcedures[static_cast<size_t>(i)]) {
            continue;
        }
        procedureFunctions[static_cast<size_t>(i)] = llvm::Function::Create(
            procedureTy,
            llvm::Function::ExternalLinkage,
            name + "_procedure_" + std::to_string(i),
            module.get());
        if (std::getenv("SJIT_ENABLE_WARP_ALWAYS_INLINE") != nullptr &&
            shouldAlwaysInlineWarpProcedure(script.procedures[i])) {
            procedureFunctions[static_cast<size_t>(i)]->addFnAttr(llvm::Attribute::AlwaysInline);
        }
    }

    auto *fn = llvm::Function::Create(entryTy, llvm::Function::ExternalLinkage, name, module.get());
    auto arg = fn->arg_begin();
    llvm::Value *runtime = &*arg++;
    runtime->setName("runtime");
    llvm::Value *thread = &*arg++;
    thread->setName("thread");
    llvm::Value *frame = &*arg++;
    frame->setName("frame");

    auto *entry = llvm::BasicBlock::Create(*context, "entry", fn);
    auto *nullReturn = llvm::BasicBlock::Create(*context, "null_return", fn);
    auto *havePointers = llvm::BasicBlock::Create(*context, "have_pointers", fn);
    auto *scriptNullReturn = llvm::BasicBlock::Create(*context, "script_null_return", fn);
    auto *targetDispatch = llvm::BasicBlock::Create(*context, "target_dispatch", fn);
    auto *dynamicTargetFallback = llvm::BasicBlock::Create(*context, "dynamic_target_fallback", fn);
    auto *nativeEntryPrologue = llvm::BasicBlock::Create(*context, "native_entry_prologue", fn);
    auto *dispatch = llvm::BasicBlock::Create(*context, "pc_dispatch", fn);
    auto *done = llvm::BasicBlock::Create(*context, "done", fn);

    std::vector<JitStatementNode> nodes;
    std::vector<int> branchCounters;
    auto addNode = [&](JitNodeKind kind, const SStatement *statement) -> int {
        const int pc = static_cast<int>(nodes.size());
        JitStatementNode node;
        node.kind = kind;
        node.statement = statement;
        node.pc = pc;
        nodes.push_back(node);
        return pc;
    };
    std::function<int(const SStatement *, int, int)> buildSequence;
    std::function<int(const SStatement *, int)> buildStatement = [&](const SStatement *statement, int nextPc) -> int {
        if (!statement) {
            return nextPc;
        }
        switch (statement->opcode) {
        case SJIT_STMT_REPEAT: {
            const int entryPc = addNode(JitNodeKind::RepeatEntry, statement);
            const int continuePc = addNode(JitNodeKind::RepeatContinue, statement);
            const int afterPc = addNode(JitNodeKind::RepeatAfter, statement);
            const int counter = static_cast<int>(branchCounters.size());
            branchCounters.push_back(entryPc);
            const int bodyPc = buildSequence(statement->substack, statement->substack_count, afterPc);
            nodes[entryPc].continuePc = continuePc;
            nodes[entryPc].branchCounter = counter;
            nodes[continuePc].bodyPc = bodyPc;
            nodes[continuePc].nextPc = nextPc;
            nodes[continuePc].controlPc = entryPc;
            nodes[afterPc].continuePc = continuePc;
            nodes[afterPc].controlPc = entryPc;
            nodes[afterPc].nextPc = nextPc;
            nodes[afterPc].branchCounter = counter;
            return entryPc;
        }
        case SJIT_STMT_REPEAT_UNTIL: {
            const int entryPc = addNode(JitNodeKind::RepeatUntilEntry, statement);
            const int continuePc = addNode(JitNodeKind::RepeatUntilContinue, statement);
            const int afterPc = addNode(JitNodeKind::RepeatUntilAfter, statement);
            const int bodyPc = buildSequence(statement->substack, statement->substack_count, afterPc);
            nodes[entryPc].continuePc = continuePc;
            nodes[continuePc].bodyPc = bodyPc;
            nodes[continuePc].nextPc = nextPc;
            nodes[continuePc].controlPc = entryPc;
            nodes[afterPc].continuePc = continuePc;
            nodes[afterPc].controlPc = entryPc;
            nodes[afterPc].nextPc = nextPc;
            return entryPc;
        }
        case SJIT_STMT_WHILE: {
            const int entryPc = addNode(JitNodeKind::WhileEntry, statement);
            const int continuePc = addNode(JitNodeKind::WhileContinue, statement);
            const int afterPc = addNode(JitNodeKind::WhileAfter, statement);
            const int counter = static_cast<int>(branchCounters.size());
            branchCounters.push_back(entryPc);
            const int bodyPc = buildSequence(statement->substack, statement->substack_count, afterPc);
            nodes[entryPc].continuePc = continuePc;
            nodes[entryPc].branchCounter = counter;
            nodes[continuePc].bodyPc = bodyPc;
            nodes[continuePc].nextPc = nextPc;
            nodes[continuePc].controlPc = entryPc;
            nodes[afterPc].continuePc = continuePc;
            nodes[afterPc].controlPc = entryPc;
            nodes[afterPc].nextPc = nextPc;
            nodes[afterPc].branchCounter = counter;
            return entryPc;
        }
        case SJIT_STMT_FOREVER: {
            const int entryPc = addNode(JitNodeKind::ForeverEntry, statement);
            const int continuePc = addNode(JitNodeKind::ForeverContinue, statement);
            const int afterPc = addNode(JitNodeKind::ForeverAfter, statement);
            const int counter = static_cast<int>(branchCounters.size());
            branchCounters.push_back(entryPc);
            const int bodyPc = buildSequence(statement->substack, statement->substack_count, afterPc);
            nodes[entryPc].continuePc = continuePc;
            nodes[entryPc].branchCounter = counter;
            nodes[continuePc].bodyPc = bodyPc;
            nodes[continuePc].controlPc = entryPc;
            nodes[afterPc].continuePc = continuePc;
            nodes[afterPc].controlPc = entryPc;
            nodes[afterPc].branchCounter = counter;
            return entryPc;
        }
        case SJIT_STMT_FOR_EACH: {
            const int entryPc = addNode(JitNodeKind::ForEachEntry, statement);
            const int continuePc = addNode(JitNodeKind::ForEachContinue, statement);
            const int afterPc = addNode(JitNodeKind::ForEachAfter, statement);
            const int bodyPc = buildSequence(statement->substack, statement->substack_count, afterPc);
            nodes[entryPc].continuePc = continuePc;
            nodes[continuePc].bodyPc = bodyPc;
            nodes[continuePc].nextPc = nextPc;
            nodes[continuePc].controlPc = entryPc;
            nodes[afterPc].continuePc = continuePc;
            nodes[afterPc].controlPc = entryPc;
            nodes[afterPc].nextPc = nextPc;
            return entryPc;
        }
        case SJIT_STMT_IF: {
            const int entryPc = addNode(JitNodeKind::IfEntry, statement);
            const int afterPc = addNode(JitNodeKind::IfAfter, statement);
            const int bodyPc = buildSequence(statement->substack, statement->substack_count, afterPc);
            nodes[entryPc].bodyPc = bodyPc;
            nodes[entryPc].nextPc = nextPc;
            nodes[afterPc].nextPc = nextPc;
            return entryPc;
        }
        case SJIT_STMT_IF_ELSE: {
            const int entryPc = addNode(JitNodeKind::IfElseEntry, statement);
            const int afterPc = addNode(JitNodeKind::IfElseAfter, statement);
            const int bodyPc = buildSequence(statement->substack, statement->substack_count, afterPc);
            const int elsePc = buildSequence(statement->substack2, statement->substack2_count, afterPc);
            nodes[entryPc].bodyPc = bodyPc;
            nodes[entryPc].elsePc = elsePc;
            nodes[afterPc].nextPc = nextPc;
            return entryPc;
        }
        default: {
            const int pc = addNode(JitNodeKind::Statement, statement);
            nodes[pc].nextPc = nextPc;
            return pc;
        }
        }
    };
    buildSequence = [&](const SStatement *statements, int count, int exitPc) -> int {
        int nextPc = exitPc;
        if (!statements || count <= 0) {
            return nextPc;
        }
        for (int i = count - 1; i >= 0; --i) {
            nextPc = buildStatement(&statements[i], nextPc);
        }
        return nextPc;
    };
    const int scriptEntryPc = addNode(JitNodeKind::Statement, nullptr);
    const int firstScriptPc = buildSequence(script.statements, script.statement_count, -1);
    nodes[scriptEntryPc].nextPc = firstScriptPc;

    std::vector<llvm::BasicBlock *> statementBlocks;
    statementBlocks.reserve(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        statementBlocks.push_back(llvm::BasicBlock::Create(*context, "pc_" + std::to_string(i), fn));
    }

    auto *intptrTy = dataLayout.getIntPtrType(*context);
    auto constantPointer = [&](const void *address) -> llvm::Constant * {
        return llvm::ConstantExpr::getIntToPtr(
            llvm::ConstantInt::get(intptrTy, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(address))),
            ptr);
    };

    llvm::IRBuilder<> builder(entry);
    llvm::Value *badPointers = builder.CreateOr(isNull(builder, runtime), isNull(builder, thread));
    badPointers = builder.CreateOr(badPointers, isNull(builder, frame));
    builder.CreateCondBr(badPointers, nullReturn, havePointers);

    builder.SetInsertPoint(nullReturn);
    builder.CreateRet(llvm::ConstantInt::get(i32, SJIT_STATUS_ERROR));

    auto threadScriptDataPtr = [&]() -> llvm::Value * {
        return builder.CreateStructGEP(threadTy, thread, 6, "script_data_ptr");
    };
    auto framePcPtr = [&]() -> llvm::Value * {
        return builder.CreateStructGEP(frameTy, frame, 0, "frame_pc_ptr");
    };
    auto frameFinishedPtr = [&]() -> llvm::Value * {
        return builder.CreateStructGEP(frameTy, frame, 4, "frame_finished_ptr");
    };
    auto loadFramePc = [&]() -> llvm::Value * {
        return builder.CreateLoad(i32, framePcPtr(), "raw_pc");
    };
    auto storeFramePc = [&](llvm::Value *pcValue) {
        builder.CreateStore(pcValue, framePcPtr());
    };
    auto markFrameFinished = [&]() {
        builder.CreateStore(llvm::ConstantInt::get(i32, 1), frameFinishedPtr());
    };

    builder.SetInsertPoint(havePointers);
    llvm::Value *scriptData = builder.CreateLoad(ptr, threadScriptDataPtr(), "script_data");
    builder.CreateCondBr(
        builder.CreateICmpEQ(
            scriptData,
            constantPointer(&script),
            "script_data_matches_compiled_arena"),
        targetDispatch,
        scriptNullReturn);

    builder.SetInsertPoint(scriptNullReturn);
    builder.CreateRet(llvm::ConstantInt::get(i32, SJIT_STATUS_ERROR));

    builder.SetInsertPoint(targetDispatch);
    llvm::Value *threadTargetId = builder.CreateLoad(
        i32,
        builder.CreateStructGEP(threadTy, thread, 1, "thread_target_id_ptr"),
        "thread_target_id");
    llvm::Value *runtimeIdentityMatches = llvm::ConstantInt::getTrue(*context);
    if (script.jit_runtime_instance_id != 0) {
        llvm::Value *runtimeIdentity = builder.CreateLoad(
            i64,
            builder.CreateStructGEP(runtimeTy, runtime, 23, "runtime_instance_id_ptr"),
            "runtime_instance_id");
        runtimeIdentityMatches = builder.CreateICmpEQ(
            runtimeIdentity,
            llvm::ConstantInt::get(i64, script.jit_runtime_instance_id),
            "runtime_matches_compiled_handles");
    }
    llvm::Value *targetMatches = builder.CreateICmpEQ(
        threadTargetId,
        llvm::ConstantInt::get(i32, script.target_id),
        "thread_uses_compiled_target");
    builder.CreateCondBr(
        builder.CreateAnd(targetMatches, runtimeIdentityMatches, "native_entry_guards_match"),
        nativeEntryPrologue,
        dynamicTargetFallback);

    builder.SetInsertPoint(dynamicTargetFallback);
    builder.CreateRet(builder.CreateCall(
        scriptInterpreter,
        {runtime, thread, frame},
        "dynamic_target_interpreter_status"));

    builder.SetInsertPoint(nativeEntryPrologue);
    builder.CreateBr(dispatch);

    llvm::IRBuilder<> entryBuilder(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    llvm::AllocaInst *pcSlot = entryBuilder.CreateAlloca(i32, nullptr, "pc_slot");
    std::vector<llvm::AllocaInst *> branchCounterSlots;
    branchCounterSlots.reserve(branchCounters.size());
    for (size_t i = 0; i < branchCounters.size(); ++i) {
        branchCounterSlots.push_back(entryBuilder.CreateAlloca(i32, nullptr, "branch_count_" + std::to_string(i)));
    }
    builder.SetInsertPoint(havePointers->getTerminator());
    for (llvm::AllocaInst *slot : branchCounterSlots) {
        builder.CreateStore(llvm::ConstantInt::get(i32, 0), slot);
    }
    llvm::Value *rawPc = loadFramePc();
    llvm::Value *negativePc = builder.CreateICmpSLT(rawPc, llvm::ConstantInt::get(i32, 0));
    llvm::Value *pastEndPc = builder.CreateICmpSGE(rawPc, llvm::ConstantInt::get(i32, static_cast<int>(nodes.size())));
    llvm::Value *resetPc = builder.CreateOr(negativePc, pastEndPc);
    llvm::Value *startPc = builder.CreateSelect(resetPc, llvm::ConstantInt::get(i32, 0), rawPc, "start_pc");
    builder.CreateStore(startPc, pcSlot);

    builder.SetInsertPoint(dispatch);
    llvm::Value *pc = builder.CreateLoad(i32, pcSlot, "pc");
    llvm::SwitchInst *pcSwitch = builder.CreateSwitch(pc, done, static_cast<unsigned>(statementBlocks.size()));
    for (size_t i = 0; i < nodes.size(); ++i) {
        pcSwitch->addCase(llvm::ConstantInt::get(i32, static_cast<int>(i)), statementBlocks[i]);
    }

    auto emitProcedureFunction = [&](int procedureIndex) {
        llvm::Function *procedureFn = procedureFunctions[static_cast<size_t>(procedureIndex)];
        if (!procedureFn) {
            return;
        }
        const SCompiledProcedure &procedure = script.procedures[procedureIndex];
        const bool statelessWarpProcedure =
            statelessWarpProcedures[static_cast<size_t>(procedureIndex)];
        const bool fastVariableAccess = procedure.warp_mode;
        const bool procedureNeedsSprite = procedureStatementsNeedSprite(
            procedure.statements,
            procedure.statement_count);
        ProcedureLoopVariableUses loopVariableUses;
        collectProcedureLoopVariableUses(
            procedure.statements,
            procedure.statement_count,
            false,
            loopVariableUses);
        const bool hoistWarpVariableAccess = fastVariableAccess &&
            std::getenv("SJIT_ENABLE_WARP_VARIABLE_HOIST") != nullptr;
        auto argIt = procedureFn->arg_begin();
        llvm::Value *procRuntime = &*argIt++;
        procRuntime->setName("runtime");
        llvm::Value *procThread = &*argIt++;
        procThread->setName("thread");
        llvm::Value *procFrame = &*argIt++;
        procFrame->setName("frame");
        llvm::Value *procScriptData = &*argIt++;
        procScriptData->setName("script_data");
        llvm::Value *procDepth = &*argIt++;
        procDepth->setName("procedure_depth");
        llvm::Value *procArgs = &*argIt++;
        procArgs->setName("procedure_args");
        llvm::Value *procValueArgs = &*argIt++;
        procValueArgs->setName("procedure_value_args");

        auto *procEntry = llvm::BasicBlock::Create(*context, "entry", procedureFn);
        llvm::IRBuilder<> procBuilder(procEntry);
        auto *procIdentityGuard = llvm::BasicBlock::Create(
            *context, "procedure_identity_guard", procedureFn);
        auto *procHandlePrologue = llvm::BasicBlock::Create(
            *context, "procedure_handle_prologue", procedureFn);
        auto *procBody = llvm::BasicBlock::Create(*context, "procedure_body", procedureFn);
        auto *procGuardFailed = llvm::BasicBlock::Create(
            *context, "procedure_guard_failed", procedureFn);
        llvm::Value *procedureGuardFailed = procBuilder.CreateOr(
            isNull(procBuilder, procRuntime),
            isNull(procBuilder, procThread),
            "procedure_runtime_or_thread_null");
        procedureGuardFailed = procBuilder.CreateOr(
            procedureGuardFailed,
            isNull(procBuilder, procFrame),
            "procedure_frame_null");
        procedureGuardFailed = procBuilder.CreateOr(
            procedureGuardFailed,
            procBuilder.CreateICmpSLT(
                procDepth,
                llvm::ConstantInt::get(i32, 0),
                "procedure_depth_negative"),
            "procedure_invalid_pointer_or_negative_depth");
        procedureGuardFailed = procBuilder.CreateOr(
            procedureGuardFailed,
            procBuilder.CreateICmpSGE(
                procDepth,
                llvm::ConstantInt::get(i32, SJIT_MAX_PROCEDURE_CALL_DEPTH),
                "procedure_depth_too_deep"),
            "procedure_pointer_or_depth_guard_failed");
        procBuilder.CreateCondBr(
            procedureGuardFailed,
            procGuardFailed,
            procIdentityGuard);

        procBuilder.SetInsertPoint(procIdentityGuard);
        llvm::Value *procedureIdentityMatches = procBuilder.CreateICmpEQ(
            procScriptData,
            constantPointer(&script),
            "procedure_script_argument_matches");
        llvm::Value *procedureThreadScriptData = procBuilder.CreateLoad(
            ptr,
            procBuilder.CreateStructGEP(
                threadTy, procThread, 6, "procedure_thread_script_data_ptr"),
            "procedure_thread_script_data");
        procedureIdentityMatches = procBuilder.CreateAnd(
            procedureIdentityMatches,
            procBuilder.CreateICmpEQ(
                procedureThreadScriptData,
                constantPointer(&script),
                "procedure_thread_script_matches"),
            "procedure_script_identity_matches");
        llvm::Value *procedureThreadTargetId = procBuilder.CreateLoad(
            i32,
            procBuilder.CreateStructGEP(
                threadTy, procThread, 1, "procedure_thread_target_id_ptr"),
            "procedure_thread_target_id");
        procedureIdentityMatches = procBuilder.CreateAnd(
            procedureIdentityMatches,
            procBuilder.CreateICmpEQ(
                procedureThreadTargetId,
                llvm::ConstantInt::get(i32, script.target_id),
                "procedure_thread_target_matches"),
            "procedure_script_and_target_match");
        if (script.jit_runtime_instance_id != 0) {
            llvm::Value *runtimeIdentity = procBuilder.CreateLoad(
                i64,
                procBuilder.CreateStructGEP(
                    runtimeTy, procRuntime, 23, "procedure_runtime_instance_id_ptr"),
                "procedure_runtime_instance_id");
            llvm::Value *runtimeIdentityMatches = procBuilder.CreateICmpEQ(
                runtimeIdentity,
                llvm::ConstantInt::get(i64, script.jit_runtime_instance_id),
                "procedure_runtime_matches_handles");
            procedureIdentityMatches = procBuilder.CreateAnd(
                procedureIdentityMatches,
                runtimeIdentityMatches,
                "procedure_entry_identity_matches");
        }
        procBuilder.CreateCondBr(
            procedureIdentityMatches,
            procHandlePrologue,
            procGuardFailed);

        procBuilder.SetInsertPoint(procGuardFailed);
        procBuilder.CreateRet(llvm::ConstantInt::get(i32, SJIT_STATUS_ERROR));

        procBuilder.SetInsertPoint(procHandlePrologue);
        procBuilder.CreateBr(procBody);

        procBuilder.SetInsertPoint(procBody);
        const bool hasUnsafeBoundTarget =
            unsafeRawRuntimeObjectConstantsEnabled() && script.bound_target;
        llvm::Value *procTarget = hasUnsafeBoundTarget ?
            static_cast<llvm::Value *>(constantPointer(script.bound_target)) :
            static_cast<llvm::Value *>(llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(ptr)));
        if (procedureNeedsSprite && !hasUnsafeBoundTarget) {
            procTarget = procBuilder.CreateCall(
                runtimeGetSprite,
                {procRuntime, llvm::ConstantInt::get(i32, script.target_id)},
                "procedure_target");
        }
        auto createProcAlloca = [&](llvm::Type *type, llvm::Value *count, const llvm::Twine &allocaName) -> llvm::AllocaInst * {
            llvm::IRBuilder<> allocaBuilder(&procedureFn->getEntryBlock(), procedureFn->getEntryBlock().begin());
            return allocaBuilder.CreateAlloca(type, count, allocaName);
        };
        struct ProcVariableHandle {
            llvm::AllocaInst *ownerSlot;
            llvm::AllocaInst *indexSlot;
        };
        std::unordered_map<const SSprite *, llvm::Value *> procOwnerVariableBases;
        auto emitProcStableVariable = [&](
            const SSprite *stableOwner,
            int variableIndex,
            const std::string &name) -> llvm::Value * {
            llvm::IRBuilder<> hoistBuilder(procHandlePrologue->getTerminator());
            llvm::Value *variables = nullptr;
            auto owner = procOwnerVariableBases.find(stableOwner);
            if (owner != procOwnerVariableBases.end()) {
                variables = owner->second;
            } else {
                llvm::Value *sprite = constantPointer(stableOwner);
                llvm::Value *target = hoistBuilder.CreateStructGEP(
                    spriteTy, sprite, 0, name + "_owner_base");
                variables = hoistBuilder.CreateLoad(
                    ptr,
                    hoistBuilder.CreateStructGEP(
                        targetTy, target, 4, name + "_owner_variables_ptr"),
                    name + "_owner_variables");
                procOwnerVariableBases.emplace(stableOwner, variables);
            }
            return hoistBuilder.CreateInBoundsGEP(
                variableTy,
                variables,
                llvm::ConstantInt::get(i32, variableIndex),
                name + "_stable_handle");
        };
        std::unordered_map<std::string, llvm::Value *> procHoistedScalarVariables;
        std::unordered_map<std::string, llvm::Value *> procHoistedListVariables;
        std::unordered_map<const SExpr *, ProcVariableHandle> procScalarVariables;
        auto emitProcScalarVariable = [&](const SExpr *expr) -> llvm::Value * {
            if (!unsafeRawRuntimeObjectConstantsEnabled()) {
                if (script.jit_runtime_instance_id != 0 && expr &&
                    expr->literal.tag == SJIT_VALUE_STRING) {
                    const std::string variableKey = compiledVariableKey(
                        SJIT_VAR_SCALAR,
                        expr->variable_id,
                        static_cast<const SString *>(expr->literal.ptr));
                    auto hoisted = procHoistedScalarVariables.find(variableKey);
                    if (hoisted != procHoistedScalarVariables.end()) {
                        return hoisted->second;
                    }
                    /* Resolve a checked handle once per native procedure
                       invocation.  The runtime identity guard prevents this
                       specialized entry from being used by another runtime;
                       a yield returns from the function, so the next resume
                       resolves the handle again. */
                    llvm::Value *variable = nullptr;
                    if (expr->variable_cache_owner &&
                        expr->variable_cache_owner_is_original &&
                        expr->variable_cache_runtime_instance_id ==
                            script.jit_runtime_instance_id &&
                        expr->variable_cache_owner_target_id > 0 &&
                        expr->variable_cache_index >= 0 &&
                        expr->variable_cache_type == SJIT_VAR_SCALAR) {
                        variable = emitProcStableVariable(
                            expr->variable_cache_owner,
                            expr->variable_cache_index,
                            "procedure_scalar_variable");
                    } else {
                        llvm::IRBuilder<> hoistBuilder(procBody, procBody->begin());
                        variable = hoistBuilder.CreateCall(
                            exprScalarVariable,
                            {procRuntime, llvm::ConstantInt::get(i32, script.target_id), constantPointer(expr)},
                            "procedure_scalar_variable_handle");
                    }
                    procHoistedScalarVariables.emplace(variableKey, variable);
                    return variable;
                }
                return procBuilder.CreateCall(
                    exprScalarVariable,
                    {procRuntime, llvm::ConstantInt::get(i32, script.target_id), constantPointer(expr)},
                    "procedure_scalar_variable_handle");
            }
            if (SVariable *variable = preboundExprVariable(expr, SJIT_VAR_SCALAR)) {
                return constantPointer(variable);
            }
            if (hoistWarpVariableAccess && loopVariableUses.scalarExpressions.count(expr) != 0) {
                const std::string variableKey = expr && expr->literal.tag == SJIT_VALUE_STRING ?
                    compiledVariableKey(
                        SJIT_VAR_SCALAR,
                        expr->variable_id,
                        static_cast<const SString *>(expr->literal.ptr)) : "";
                auto hoisted = procHoistedScalarVariables.find(variableKey);
                if (hoisted != procHoistedScalarVariables.end()) {
                    return hoisted->second;
                }
                llvm::IRBuilder<> hoistBuilder(procBody, procBody->begin());
                llvm::Value *variable = hoistBuilder.CreateCall(
                    exprScalarVariable,
                    {procRuntime, llvm::ConstantInt::get(i32, script.target_id), constantPointer(expr)},
                    "procedure_scalar_variable");
                procHoistedScalarVariables.emplace(variableKey, variable);
                return variable;
            }
            auto found = procScalarVariables.find(expr);
            if (found == procScalarVariables.end()) {
                llvm::IRBuilder<> hoistBuilder(procBody, procBody->begin());
                llvm::Value *exprPtr = constantPointer(expr);
                llvm::AllocaInst *ownerSlot = createProcAlloca(ptr, nullptr, "procedure_variable_owner_slot");
                llvm::AllocaInst *indexSlot = createProcAlloca(i32, nullptr, "procedure_variable_index_slot");
                hoistBuilder.CreateStore(
                    hoistBuilder.CreateLoad(
                        ptr,
                        hoistBuilder.CreateStructGEP(exprTy, exprPtr, 6, "procedure_variable_owner_ptr"),
                        "procedure_variable_owner"),
                    ownerSlot);
                hoistBuilder.CreateStore(
                    hoistBuilder.CreateLoad(
                        i32,
                        hoistBuilder.CreateStructGEP(exprTy, exprPtr, 9, "procedure_variable_index_ptr"),
                        "procedure_variable_index"),
                    indexSlot);
                ProcVariableHandle handle{
                    ownerSlot,
                    indexSlot,
                };
                found = procScalarVariables.emplace(expr, handle).first;
            }
            const ProcVariableHandle handle = found->second;
            llvm::Value *owner = procBuilder.CreateLoad(ptr, handle.ownerSlot, "procedure_variable_owner");
            llvm::Value *index = procBuilder.CreateLoad(i32, handle.indexSlot, "procedure_variable_index");
            llvm::AllocaInst *resultSlot = createProcAlloca(ptr, nullptr, "procedure_variable_ptr_slot");
            auto *checkIndex = llvm::BasicBlock::Create(*context, "procedure_variable_check_index", procedureFn);
            auto *directVariable = llvm::BasicBlock::Create(*context, "procedure_variable_cached", procedureFn);
            auto *fallbackVariable = llvm::BasicBlock::Create(*context, "procedure_variable_lookup", procedureFn);
            auto *continueBlock = llvm::BasicBlock::Create(*context, "procedure_variable_ptr_continue", procedureFn);
            procBuilder.CreateCondBr(isNull(procBuilder, owner), fallbackVariable, checkIndex);

            procBuilder.SetInsertPoint(checkIndex);
            llvm::Value *base = procBuilder.CreateStructGEP(spriteTy, owner, 0, "procedure_variable_owner_base");
            llvm::Value *count = procBuilder.CreateLoad(
                i32,
                procBuilder.CreateStructGEP(targetTy, base, 5, "procedure_variable_count_ptr"),
                "procedure_variable_count");
            llvm::Value *variables = procBuilder.CreateLoad(
                ptr,
                procBuilder.CreateStructGEP(targetTy, base, 4, "procedure_variables_ptr"),
                "procedure_variables");
            llvm::Value *indexValid = procBuilder.CreateAnd(
                procBuilder.CreateICmpSGE(index, llvm::ConstantInt::get(i32, 0)),
                procBuilder.CreateICmpSLT(index, count),
                "procedure_variable_index_valid");
            procBuilder.CreateCondBr(
                procBuilder.CreateAnd(indexValid, procBuilder.CreateNot(isNull(procBuilder, variables))),
                directVariable,
                fallbackVariable);

            procBuilder.SetInsertPoint(directVariable);
            procBuilder.CreateStore(
                procBuilder.CreateInBoundsGEP(variableTy, variables, index, "procedure_variable_cached_ptr"),
                resultSlot);
            procBuilder.CreateBr(continueBlock);

            procBuilder.SetInsertPoint(fallbackVariable);
            procBuilder.CreateStore(
                procBuilder.CreateCall(
                    exprScalarVariable,
                    {procRuntime, llvm::ConstantInt::get(i32, script.target_id), constantPointer(expr)},
                    "procedure_variable_lookup_result"),
                resultSlot);
            llvm::Value *exprPtr = constantPointer(expr);
            procBuilder.CreateStore(
                procBuilder.CreateLoad(
                    ptr,
                    procBuilder.CreateStructGEP(exprTy, exprPtr, 6, "procedure_variable_refreshed_owner_ptr")),
                handle.ownerSlot);
            procBuilder.CreateStore(
                procBuilder.CreateLoad(
                    i32,
                    procBuilder.CreateStructGEP(exprTy, exprPtr, 9, "procedure_variable_refreshed_index_ptr")),
                handle.indexSlot);
            procBuilder.CreateBr(continueBlock);

            procBuilder.SetInsertPoint(continueBlock);
            return procBuilder.CreateLoad(ptr, resultSlot, "procedure_variable_ptr");
        };
        auto emitProcListVariable = [&](const SExpr *expr) -> llvm::Value * {
            if (!unsafeRawRuntimeObjectConstantsEnabled()) {
                if (script.jit_runtime_instance_id != 0 && expr &&
                    expr->literal.tag == SJIT_VALUE_STRING) {
                    const std::string variableKey = compiledVariableKey(
                        SJIT_VAR_LIST,
                        expr->variable_id,
                        static_cast<const SString *>(expr->literal.ptr));
                    auto hoisted = procHoistedListVariables.find(variableKey);
                    if (hoisted != procHoistedListVariables.end()) {
                        return hoisted->second;
                    }
                    llvm::Value *variable = nullptr;
                    if (expr->variable_cache_owner &&
                        expr->variable_cache_owner_is_original &&
                        expr->variable_cache_runtime_instance_id ==
                            script.jit_runtime_instance_id &&
                        expr->variable_cache_owner_target_id > 0 &&
                        expr->variable_cache_index >= 0 &&
                        expr->variable_cache_type == SJIT_VAR_LIST) {
                        variable = emitProcStableVariable(
                            expr->variable_cache_owner,
                            expr->variable_cache_index,
                            "procedure_list_variable");
                    } else {
                        llvm::IRBuilder<> hoistBuilder(procBody, procBody->begin());
                        variable = hoistBuilder.CreateCall(
                            exprListVariable,
                            {procRuntime, llvm::ConstantInt::get(i32, script.target_id), constantPointer(expr)},
                            "procedure_list_variable_handle");
                    }
                    procHoistedListVariables.emplace(variableKey, variable);
                    return variable;
                }
                return procBuilder.CreateCall(
                    exprListVariable,
                    {procRuntime, llvm::ConstantInt::get(i32, script.target_id), constantPointer(expr)},
                    "procedure_list_variable_handle");
            }
            if (SVariable *variable = preboundExprVariable(expr, SJIT_VAR_LIST)) {
                return constantPointer(variable);
            }
            return procBuilder.CreateCall(
                exprListVariable,
                {procRuntime, llvm::ConstantInt::get(i32, script.target_id), constantPointer(expr)},
                "procedure_list_variable");
        };
        std::unordered_map<const SStatement *, ProcVariableHandle> procStatementVariables;
        auto emitProcStatementScalarVariable = [&](const SStatement *statement) -> llvm::Value * {
            if (!unsafeRawRuntimeObjectConstantsEnabled()) {
                if (script.jit_runtime_instance_id != 0 && statement && statement->variable_name) {
                    const std::string variableKey = compiledVariableKey(
                        SJIT_VAR_SCALAR,
                        statement->variable_id,
                        statement->variable_name);
                    auto hoisted = procHoistedScalarVariables.find(variableKey);
                    if (hoisted != procHoistedScalarVariables.end()) {
                        return hoisted->second;
                    }
                    llvm::Value *variable = nullptr;
                    if (statement->variable_cache_owner &&
                        statement->variable_cache_owner_is_original &&
                        statement->variable_cache_runtime_instance_id ==
                            script.jit_runtime_instance_id &&
                        statement->variable_cache_owner_target_id > 0 &&
                        statement->variable_cache_index >= 0 &&
                        statement->variable_cache_type == SJIT_VAR_SCALAR) {
                        variable = emitProcStableVariable(
                            statement->variable_cache_owner,
                            statement->variable_cache_index,
                            "procedure_statement_scalar_variable");
                    } else {
                        llvm::IRBuilder<> hoistBuilder(procBody, procBody->begin());
                        variable = hoistBuilder.CreateCall(
                            statementScalarVariablePtr,
                            {procRuntime, procScriptData, constantPointer(statement)},
                            "procedure_statement_scalar_variable_handle");
                    }
                    procHoistedScalarVariables.emplace(variableKey, variable);
                    return variable;
                }
                return procBuilder.CreateCall(
                    statementScalarVariablePtr,
                    {procRuntime, procScriptData, constantPointer(statement)},
                    "procedure_statement_scalar_variable_handle");
            }
            if (SVariable *variable = preboundStatementVariable(statement, SJIT_VAR_SCALAR)) {
                return constantPointer(variable);
            }
            if (hoistWarpVariableAccess && loopVariableUses.scalarStatements.count(statement) != 0) {
                const std::string variableKey = statement ?
                    compiledVariableKey(
                        SJIT_VAR_SCALAR,
                        statement->variable_id,
                        statement->variable_name) : "";
                auto hoisted = procHoistedScalarVariables.find(variableKey);
                if (hoisted != procHoistedScalarVariables.end()) {
                    return hoisted->second;
                }
                llvm::IRBuilder<> hoistBuilder(procBody, procBody->begin());
                llvm::Value *variable = hoistBuilder.CreateCall(
                    statementScalarVariablePtr,
                    {procRuntime, procScriptData, constantPointer(statement)},
                    "procedure_statement_scalar_variable");
                procHoistedScalarVariables.emplace(variableKey, variable);
                return variable;
            }
            auto found = procStatementVariables.find(statement);
            if (found == procStatementVariables.end()) {
                llvm::IRBuilder<> hoistBuilder(procBody, procBody->begin());
                llvm::Value *statementPtr = constantPointer(statement);
                llvm::AllocaInst *ownerSlot = createProcAlloca(ptr, nullptr, "procedure_statement_owner_slot");
                llvm::AllocaInst *indexSlot = createProcAlloca(i32, nullptr, "procedure_statement_index_slot");
                hoistBuilder.CreateStore(
                    hoistBuilder.CreateLoad(
                        ptr,
                        hoistBuilder.CreateStructGEP(statementTy, statementPtr, 13, "procedure_statement_owner_ptr"),
                        "procedure_statement_owner"),
                    ownerSlot);
                hoistBuilder.CreateStore(
                    hoistBuilder.CreateLoad(
                        i32,
                        hoistBuilder.CreateStructGEP(statementTy, statementPtr, 16, "procedure_statement_index_ptr"),
                        "procedure_statement_index"),
                    indexSlot);
                ProcVariableHandle handle{
                    ownerSlot,
                    indexSlot,
                };
                found = procStatementVariables.emplace(statement, handle).first;
            }
            const ProcVariableHandle handle = found->second;
            llvm::Value *owner = procBuilder.CreateLoad(ptr, handle.ownerSlot, "procedure_statement_owner");
            llvm::Value *index = procBuilder.CreateLoad(i32, handle.indexSlot, "procedure_statement_index");
            llvm::AllocaInst *resultSlot = createProcAlloca(ptr, nullptr, "procedure_statement_variable_ptr_slot");
            auto *checkIndex = llvm::BasicBlock::Create(*context, "procedure_statement_variable_check", procedureFn);
            auto *directVariable = llvm::BasicBlock::Create(*context, "procedure_statement_variable_cached", procedureFn);
            auto *fallbackVariable = llvm::BasicBlock::Create(*context, "procedure_statement_variable_lookup", procedureFn);
            auto *continueBlock = llvm::BasicBlock::Create(*context, "procedure_statement_variable_continue", procedureFn);
            procBuilder.CreateCondBr(isNull(procBuilder, owner), fallbackVariable, checkIndex);

            procBuilder.SetInsertPoint(checkIndex);
            llvm::Value *base = procBuilder.CreateStructGEP(spriteTy, owner, 0, "procedure_statement_owner_base");
            llvm::Value *count = procBuilder.CreateLoad(
                i32,
                procBuilder.CreateStructGEP(targetTy, base, 5, "procedure_statement_variable_count_ptr"),
                "procedure_statement_variable_count");
            llvm::Value *variables = procBuilder.CreateLoad(
                ptr,
                procBuilder.CreateStructGEP(targetTy, base, 4, "procedure_statement_variables_ptr"),
                "procedure_statement_variables");
            llvm::Value *indexValid = procBuilder.CreateAnd(
                procBuilder.CreateICmpSGE(index, llvm::ConstantInt::get(i32, 0)),
                procBuilder.CreateICmpSLT(index, count));
            procBuilder.CreateCondBr(
                procBuilder.CreateAnd(indexValid, procBuilder.CreateNot(isNull(procBuilder, variables))),
                directVariable,
                fallbackVariable);

            procBuilder.SetInsertPoint(directVariable);
            procBuilder.CreateStore(
                procBuilder.CreateInBoundsGEP(variableTy, variables, index, "procedure_statement_variable_cached_ptr"),
                resultSlot);
            procBuilder.CreateBr(continueBlock);

            procBuilder.SetInsertPoint(fallbackVariable);
            procBuilder.CreateStore(
                procBuilder.CreateCall(
                    statementScalarVariablePtr,
                    {procRuntime, procScriptData, constantPointer(statement)},
                    "procedure_statement_variable_lookup_result"),
                resultSlot);
            llvm::Value *statementPtr = constantPointer(statement);
            procBuilder.CreateStore(
                procBuilder.CreateLoad(
                    ptr,
                    procBuilder.CreateStructGEP(statementTy, statementPtr, 13, "procedure_statement_refreshed_owner_ptr")),
                handle.ownerSlot);
            procBuilder.CreateStore(
                procBuilder.CreateLoad(
                    i32,
                    procBuilder.CreateStructGEP(statementTy, statementPtr, 16, "procedure_statement_refreshed_index_ptr")),
                handle.indexSlot);
            procBuilder.CreateBr(continueBlock);

            procBuilder.SetInsertPoint(continueBlock);
            return procBuilder.CreateLoad(ptr, resultSlot, "procedure_statement_variable_ptr");
        };
        auto emitProcStatementListVariable = [&](const SStatement *statement) -> llvm::Value * {
            if (!unsafeRawRuntimeObjectConstantsEnabled()) {
                if (script.jit_runtime_instance_id != 0 && statement && statement->variable_name) {
                    const std::string variableKey = compiledVariableKey(
                        SJIT_VAR_LIST,
                        statement->variable_id,
                        statement->variable_name);
                    auto hoisted = procHoistedListVariables.find(variableKey);
                    if (hoisted != procHoistedListVariables.end()) {
                        return hoisted->second;
                    }
                    llvm::Value *variable = nullptr;
                    if (statement->variable_cache_owner &&
                        statement->variable_cache_owner_is_original &&
                        statement->variable_cache_runtime_instance_id ==
                            script.jit_runtime_instance_id &&
                        statement->variable_cache_owner_target_id > 0 &&
                        statement->variable_cache_index >= 0 &&
                        statement->variable_cache_type == SJIT_VAR_LIST) {
                        variable = emitProcStableVariable(
                            statement->variable_cache_owner,
                            statement->variable_cache_index,
                            "procedure_statement_list_variable");
                    } else {
                        llvm::IRBuilder<> hoistBuilder(procBody, procBody->begin());
                        variable = hoistBuilder.CreateCall(
                            statementListVariablePtr,
                            {procRuntime, procScriptData, constantPointer(statement)},
                            "procedure_statement_list_variable_handle");
                    }
                    procHoistedListVariables.emplace(variableKey, variable);
                    return variable;
                }
                return procBuilder.CreateCall(
                    statementListVariablePtr,
                    {procRuntime, procScriptData, constantPointer(statement)},
                    "procedure_statement_list_variable_handle");
            }
            if (SVariable *variable = preboundStatementVariable(statement, SJIT_VAR_LIST)) {
                return constantPointer(variable);
            }
            return procBuilder.CreateCall(
                statementListVariablePtr,
                {procRuntime, procScriptData, constantPointer(statement)},
                "procedure_statement_list_variable");
        };
        auto emitProcSetVariableNumber = [&](const SStatement *statement, llvm::Value *number) {
            llvm::Value *variable = emitProcStatementScalarVariable(statement);
            if (SVariable *bound = preboundStatementVariable(statement, SJIT_VAR_SCALAR);
                bound && bound->scalar_kind == SJIT_SCALAR_NUMBER) {
                llvm::Value *valuePtr = procBuilder.CreateStructGEP(
                    variableTy,
                    variable,
                    5,
                    "procedure_set_number_value_ptr");
                procBuilder.CreateStore(
                    number,
                    procBuilder.CreateStructGEP(svalueTy, valuePtr, 1, "procedure_set_number_raw_ptr"));
                return;
            }
            auto *directStore = llvm::BasicBlock::Create(*context, "procedure_set_number_direct", procedureFn);
            auto *fallbackStore = llvm::BasicBlock::Create(*context, "procedure_set_number_fallback", procedureFn);
            auto *continueBlock = llvm::BasicBlock::Create(*context, "procedure_set_number_continue", procedureFn);
            auto *haveVariable = llvm::BasicBlock::Create(*context, "procedure_set_number_have_variable", procedureFn);
            procBuilder.CreateCondBr(isNull(procBuilder, variable), fallbackStore, haveVariable);

            procBuilder.SetInsertPoint(haveVariable);
            llvm::Value *valuePtr = procBuilder.CreateStructGEP(variableTy, variable, 5, "procedure_set_number_value_ptr");
            llvm::Value *tag = procBuilder.CreateLoad(
                i32,
                procBuilder.CreateStructGEP(svalueTy, valuePtr, 0, "procedure_set_number_tag_ptr"));
            procBuilder.CreateCondBr(
                procBuilder.CreateICmpEQ(tag, llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)),
                directStore,
                fallbackStore);

            procBuilder.SetInsertPoint(directStore);
            procBuilder.CreateStore(number, procBuilder.CreateStructGEP(svalueTy, valuePtr, 1, "procedure_set_number_raw_ptr"));
            procBuilder.CreateBr(continueBlock);

            procBuilder.SetInsertPoint(fallbackStore);
            procBuilder.CreateCall(
                statementSetVariableNumber,
                {procRuntime, procScriptData, constantPointer(statement), number});
            procBuilder.CreateBr(continueBlock);

            procBuilder.SetInsertPoint(continueBlock);
        };
        auto emitProcSetVariableFromNumericListItem = [&](
            llvm::Value *destination,
            llvm::Value *listVariable,
            llvm::Value *numericIndex,
            const std::function<void(llvm::Value *)> &emitFallback) {
            auto *fallbackBlock = llvm::BasicBlock::Create(
                *context,
                "procedure_set_list_item_fallback",
                procedureFn);
            auto *continueBlock = llvm::BasicBlock::Create(
                *context,
                "procedure_set_list_item_continue",
                procedureFn);
            llvm::Value *stored = procBuilder.CreateCall(
                fastVariableSetFromNumericListItem,
                {destination, listVariable, numericIndex},
                "procedure_set_list_item_cached");
            procBuilder.CreateCondBr(stored, continueBlock, fallbackBlock);

            procBuilder.SetInsertPoint(fallbackBlock);
            emitFallback(numericIndex);
            if (!procBuilder.GetInsertBlock()->getTerminator()) {
                procBuilder.CreateBr(continueBlock);
            }

            procBuilder.SetInsertPoint(continueBlock);
        };
        auto emitProcChangeVariableNumber = [&](const SStatement *statement, llvm::Value *delta) {
            llvm::Value *variable = emitProcStatementScalarVariable(statement);
            if (SVariable *bound = preboundStatementVariable(statement, SJIT_VAR_SCALAR);
                bound && bound->scalar_kind == SJIT_SCALAR_NUMBER) {
                llvm::Value *valuePtr = procBuilder.CreateStructGEP(
                    variableTy,
                    variable,
                    5,
                    "procedure_change_number_value_ptr");
                llvm::Value *rawPtr = procBuilder.CreateStructGEP(
                    svalueTy,
                    valuePtr,
                    1,
                    "procedure_change_number_raw_ptr");
                llvm::Value *raw = procBuilder.CreateLoad(f64, rawPtr, "procedure_change_number_raw");
                llvm::Value *clean = procBuilder.CreateSelect(
                    procBuilder.CreateFCmpUNO(raw, raw),
                    llvm::ConstantFP::get(f64, 0.0),
                    raw);
                procBuilder.CreateStore(procBuilder.CreateFAdd(clean, delta), rawPtr);
                return;
            }
            auto *directStore = llvm::BasicBlock::Create(*context, "procedure_change_number_direct", procedureFn);
            auto *fallbackStore = llvm::BasicBlock::Create(*context, "procedure_change_number_fallback", procedureFn);
            auto *continueBlock = llvm::BasicBlock::Create(*context, "procedure_change_number_continue", procedureFn);
            auto *haveVariable = llvm::BasicBlock::Create(*context, "procedure_change_number_have_variable", procedureFn);
            procBuilder.CreateCondBr(isNull(procBuilder, variable), fallbackStore, haveVariable);

            procBuilder.SetInsertPoint(haveVariable);
            llvm::Value *valuePtr = procBuilder.CreateStructGEP(variableTy, variable, 5, "procedure_change_number_value_ptr");
            llvm::Value *tag = procBuilder.CreateLoad(
                i32,
                procBuilder.CreateStructGEP(svalueTy, valuePtr, 0, "procedure_change_number_tag_ptr"));
            procBuilder.CreateCondBr(
                procBuilder.CreateICmpEQ(tag, llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)),
                directStore,
                fallbackStore);

            procBuilder.SetInsertPoint(directStore);
            llvm::Value *rawPtr = procBuilder.CreateStructGEP(svalueTy, valuePtr, 1, "procedure_change_number_raw_ptr");
            llvm::Value *raw = procBuilder.CreateLoad(f64, rawPtr, "procedure_change_number_raw");
            llvm::Value *clean = procBuilder.CreateSelect(
                procBuilder.CreateFCmpUNO(raw, raw),
                llvm::ConstantFP::get(f64, 0.0),
                raw);
            procBuilder.CreateStore(procBuilder.CreateFAdd(clean, delta), rawPtr);
            procBuilder.CreateBr(continueBlock);

            procBuilder.SetInsertPoint(fallbackStore);
            procBuilder.CreateCall(
                statementChangeVariableNumber,
                {procRuntime, procScriptData, constantPointer(statement), delta});
            procBuilder.CreateBr(continueBlock);

            procBuilder.SetInsertPoint(continueBlock);
        };
        auto emitProcVariableNumber = [&](const SExpr *expr) -> llvm::Value * {
            llvm::Value *variable = emitProcScalarVariable(expr);
            if (!directProcedureVariableNumbers) {
                return procBuilder.CreateCall(
                    variableNumber,
                    {procRuntime, variable},
                    "procedure_variable_number_fallback");
            }
            if (SVariable *bound = preboundExprVariable(expr, SJIT_VAR_SCALAR);
                bound && bound->scalar_kind == SJIT_SCALAR_NUMBER) {
                llvm::Value *valuePtr = procBuilder.CreateStructGEP(
                    variableTy,
                    variable,
                    5,
                    "procedure_variable_value_ptr");
                llvm::Value *raw = procBuilder.CreateLoad(
                    f64,
                    procBuilder.CreateStructGEP(svalueTy, valuePtr, 1, "procedure_variable_raw_ptr"),
                    "procedure_variable_raw");
                return procBuilder.CreateSelect(
                    procBuilder.CreateFCmpUNO(raw, raw),
                    llvm::ConstantFP::get(f64, 0.0),
                    raw,
                    "procedure_variable_clean");
            }
            llvm::AllocaInst *resultSlot = createProcAlloca(f64, nullptr, "procedure_variable_number_slot");
            auto *haveVariable = llvm::BasicBlock::Create(*context, "procedure_variable_have", procedureFn);
            auto *directNumber = llvm::BasicBlock::Create(*context, "procedure_variable_direct", procedureFn);
            auto *checkBoolean = llvm::BasicBlock::Create(*context, "procedure_variable_check_bool", procedureFn);
            auto *directBoolean = llvm::BasicBlock::Create(*context, "procedure_variable_bool", procedureFn);
            auto *checkNull = llvm::BasicBlock::Create(*context, "procedure_variable_check_null", procedureFn);
            auto *directNull = llvm::BasicBlock::Create(*context, "procedure_variable_null", procedureFn);
            auto *checkString = llvm::BasicBlock::Create(*context, "procedure_variable_check_string", procedureFn);
            auto *checkStringCache = llvm::BasicBlock::Create(*context, "procedure_variable_string_cache", procedureFn);
            auto *directString = llvm::BasicBlock::Create(*context, "procedure_variable_string", procedureFn);
            auto *fallbackNumber = llvm::BasicBlock::Create(*context, "procedure_variable_fallback", procedureFn);
            auto *continueBlock = llvm::BasicBlock::Create(*context, "procedure_variable_continue", procedureFn);
            procBuilder.CreateCondBr(isNull(procBuilder, variable), fallbackNumber, haveVariable);

            procBuilder.SetInsertPoint(haveVariable);
            llvm::Value *valuePtr = procBuilder.CreateStructGEP(variableTy, variable, 5, "procedure_variable_value_ptr");
            llvm::Value *tag = procBuilder.CreateLoad(
                i32,
                procBuilder.CreateStructGEP(svalueTy, valuePtr, 0, "procedure_variable_tag_ptr"),
                "procedure_variable_tag");
            procBuilder.CreateCondBr(
                procBuilder.CreateICmpEQ(tag, llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER)),
                directNumber,
                checkBoolean);

            procBuilder.SetInsertPoint(directNumber);
            llvm::Value *raw = procBuilder.CreateLoad(
                f64,
                procBuilder.CreateStructGEP(svalueTy, valuePtr, 1, "procedure_variable_raw_ptr"),
                "procedure_variable_raw");
            procBuilder.CreateStore(
                procBuilder.CreateSelect(
                    procBuilder.CreateFCmpUNO(raw, raw),
                    llvm::ConstantFP::get(f64, 0.0),
                    raw,
                    "procedure_variable_clean"),
                resultSlot);
            procBuilder.CreateBr(continueBlock);

            procBuilder.SetInsertPoint(checkBoolean);
            llvm::Value *isBoolean = procBuilder.CreateICmpEQ(
                tag,
                llvm::ConstantInt::get(i32, SJIT_VALUE_BOOL),
                "procedure_variable_is_bool");
            procBuilder.CreateCondBr(isBoolean, directBoolean, checkNull);

            procBuilder.SetInsertPoint(directBoolean);
            llvm::Value *booleanRaw = procBuilder.CreateLoad(
                f64,
                procBuilder.CreateStructGEP(svalueTy, valuePtr, 1, "procedure_variable_bool_raw_ptr"),
                "procedure_variable_bool_raw");
            procBuilder.CreateStore(
                procBuilder.CreateSelect(
                    procBuilder.CreateFCmpUNE(booleanRaw, llvm::ConstantFP::get(f64, 0.0)),
                    llvm::ConstantFP::get(f64, 1.0),
                    llvm::ConstantFP::get(f64, 0.0),
                    "procedure_variable_bool_number"),
                resultSlot);
            procBuilder.CreateBr(continueBlock);

            procBuilder.SetInsertPoint(checkNull);
            llvm::Value *isNullValue = procBuilder.CreateICmpEQ(
                tag,
                llvm::ConstantInt::get(i32, SJIT_VALUE_NULL),
                "procedure_variable_is_null");
            procBuilder.CreateCondBr(isNullValue, directNull, checkString);

            procBuilder.SetInsertPoint(directNull);
            procBuilder.CreateStore(llvm::ConstantFP::get(f64, 0.0), resultSlot);
            procBuilder.CreateBr(continueBlock);

            procBuilder.SetInsertPoint(checkString);
            llvm::Value *stringPtr = procBuilder.CreateLoad(
                ptr,
                procBuilder.CreateStructGEP(svalueTy, valuePtr, 2, "procedure_variable_string_ptr"),
                "procedure_variable_string");
            llvm::Value *isString = procBuilder.CreateICmpEQ(
                tag,
                llvm::ConstantInt::get(i32, SJIT_VALUE_STRING),
                "procedure_variable_is_string");
            llvm::Value *hasString = procBuilder.CreateAnd(
                isString,
                procBuilder.CreateNot(isNull(procBuilder, stringPtr)),
                "procedure_variable_has_string");
            procBuilder.CreateCondBr(hasString, checkStringCache, directNull);

            procBuilder.SetInsertPoint(checkStringCache);
            llvm::Value *cacheValid = procBuilder.CreateICmpNE(
                procBuilder.CreateLoad(
                    i32,
                    procBuilder.CreateStructGEP(stringTy, stringPtr, 3, "procedure_variable_cache_valid_ptr"),
                    "procedure_variable_cache_valid_raw"),
                llvm::ConstantInt::get(i32, 0),
                "procedure_variable_cache_valid");
            procBuilder.CreateCondBr(cacheValid, directString, fallbackNumber);

            procBuilder.SetInsertPoint(directString);
            llvm::Value *cachedNumber = procBuilder.CreateLoad(
                f64,
                procBuilder.CreateStructGEP(stringTy, stringPtr, 6, "procedure_variable_cached_number_ptr"),
                "procedure_variable_cached_number");
            llvm::Value *cacheOk = procBuilder.CreateICmpNE(
                procBuilder.CreateLoad(
                    i32,
                    procBuilder.CreateStructGEP(stringTy, stringPtr, 4, "procedure_variable_cache_ok_ptr"),
                    "procedure_variable_cache_ok_raw"),
                llvm::ConstantInt::get(i32, 0),
                "procedure_variable_cache_ok");
            llvm::Value *cachedUsable = procBuilder.CreateAnd(
                cacheOk,
                procBuilder.CreateFCmpORD(cachedNumber, cachedNumber),
                "procedure_variable_cached_usable");
            procBuilder.CreateStore(
                procBuilder.CreateSelect(
                    cachedUsable,
                    cachedNumber,
                    llvm::ConstantFP::get(f64, 0.0),
                    "procedure_variable_cached_clean"),
                resultSlot);
            procBuilder.CreateBr(continueBlock);

            procBuilder.SetInsertPoint(fallbackNumber);
            procBuilder.CreateStore(
                procBuilder.CreateCall(variableNumber, {procRuntime, variable}, "procedure_variable_coerced"),
                resultSlot);
            procBuilder.CreateBr(continueBlock);

            procBuilder.SetInsertPoint(continueBlock);
            return procBuilder.CreateLoad(f64, resultSlot, "procedure_variable_number");
        };
        auto emitProcSpriteCoordinate = [&](unsigned fieldIndex, llvm::Function *fallback, const char *name) -> llvm::Value * {
            (void)fieldIndex;
            return procBuilder.CreateCall(fallback, {procTarget}, name);
        };
        auto emitProcSetXy = [&](llvm::Value *x, llvm::Value *y) {
            procBuilder.CreateCall(spriteSetXy, {procRuntime, procTarget, x, y, llvm::ConstantInt::get(i32, 0)});
        };

        auto procFrameWarpModePtr = [&]() -> llvm::Value * {
            return procBuilder.CreateStructGEP(frameTy, procFrame, 3, "procedure_frame_warp_ptr");
        };
        auto procRuntimeDoubleField = [&](unsigned fieldIndex, const char *fieldName) -> llvm::Value * {
            llvm::Value *field = procBuilder.CreateStructGEP(runtimeTy, procRuntime, fieldIndex, std::string(fieldName) + "_ptr");
            return procBuilder.CreateLoad(f64, field, fieldName);
        };
        auto procRuntimeInputDoubleField = [&](unsigned fieldIndex, const char *fieldName) -> llvm::Value * {
            llvm::Value *input = procBuilder.CreateStructGEP(runtimeTy, procRuntime, 0, "procedure_input_ptr");
            llvm::Value *field = procBuilder.CreateStructGEP(inputTy, input, fieldIndex, std::string(fieldName) + "_ptr");
            return procBuilder.CreateLoad(f64, field, fieldName);
        };
        auto procRuntimeInputIntField = [&](unsigned fieldIndex, const char *fieldName) -> llvm::Value * {
            llvm::Value *input = procBuilder.CreateStructGEP(runtimeTy, procRuntime, 0, "procedure_input_ptr");
            llvm::Value *field = procBuilder.CreateStructGEP(inputTy, input, fieldIndex, std::string(fieldName) + "_ptr");
            return procBuilder.CreateLoad(i32, field, fieldName);
        };
        auto procLoopStateCounterPtr = [&](llvm::Value *state) -> llvm::Value * {
            return procBuilder.CreateStructGEP(loopStateTy, state, 2, "procedure_loop_counter_ptr");
        };
        auto procLoopStateBranchActivePtr = [&](llvm::Value *state) -> llvm::Value * {
            return procBuilder.CreateStructGEP(loopStateTy, state, 3, "procedure_loop_branch_active_ptr");
        };
        auto procLoopStateSubPcPtr = [&](llvm::Value *state) -> llvm::Value * {
            return procBuilder.CreateStructGEP(loopStateTy, state, 4, "procedure_loop_sub_pc_ptr");
        };
        auto procSetLoopBranchActive = [&](llvm::Value *state) {
            llvm::Value *activePtr = procLoopStateBranchActivePtr(state);
            llvm::Value *subPcPtr = procLoopStateSubPcPtr(state);
            llvm::Value *wasActive = procBuilder.CreateLoad(i32, activePtr, "procedure_loop_was_active");
            llvm::Value *currentSubPc = procBuilder.CreateLoad(i32, subPcPtr, "procedure_loop_current_sub_pc");
            llvm::Value *nextSubPc = procBuilder.CreateSelect(
                procBuilder.CreateICmpNE(wasActive, llvm::ConstantInt::get(i32, 0)),
                currentSubPc,
                llvm::ConstantInt::get(i32, 0),
                "procedure_loop_next_sub_pc");
            procBuilder.CreateStore(llvm::ConstantInt::get(i32, 1), activePtr);
            procBuilder.CreateStore(nextSubPc, subPcPtr);
        };
        auto procFinishLoopBranch = [&](llvm::Value *state,
                                        llvm::Value *branchCount,
                                        bool batched,
                                        const llvm::Twine &name) -> llvm::Value * {
            if (!procedure.warp_mode ||
                std::getenv("SJIT_DISABLE_DIRECT_WARP_LOOP_FINISH") != nullptr) {
                return batched ?
                    procBuilder.CreateCall(
                        finishBatchedLoopBranch,
                        {procRuntime, procFrame, state, branchCount},
                        name) :
                    procBuilder.CreateCall(finishControlBranch, {procFrame, state}, name);
            }
            procBuilder.CreateStore(
                llvm::ConstantInt::get(i32, 0),
                procLoopStateBranchActivePtr(state));
            procBuilder.CreateStore(
                llvm::ConstantInt::get(i32, 0),
                procLoopStateSubPcPtr(state));
            if (!batched) {
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            }
            return procBuilder.CreateSelect(
                procBuilder.CreateICmpSGE(
                    branchCount,
                    llvm::ConstantInt::get(i32, 16384)),
                llvm::ConstantInt::get(i32, SJIT_STATUS_YIELDED),
                llvm::ConstantInt::get(i32, SJIT_STATUS_OK),
                name);
        };

        std::function<llvm::Value *(const SExpr *)> emitProcNumberExpr;
        std::function<llvm::Value *(const SExpr *)> emitProcBoolExpr;
        std::function<void(const SExpr *, llvm::Value *)> emitProcValueInto;
        auto destroyProcValueSlot = [&](llvm::Value *valueSlot) {
            llvm::Value *tag = procBuilder.CreateLoad(
                i32,
                procBuilder.CreateStructGEP(svalueTy, valueSlot, 0, "procedure_destroy_value_tag_ptr"),
                "procedure_destroy_value_tag");
            llvm::Value *ownsStorage = procBuilder.CreateOr(
                procBuilder.CreateICmpEQ(tag, llvm::ConstantInt::get(i32, SJIT_VALUE_STRING)),
                procBuilder.CreateICmpEQ(tag, llvm::ConstantInt::get(i32, SJIT_VALUE_LIST)),
                "procedure_destroy_value_owned");
            auto *destroyBlock = llvm::BasicBlock::Create(*context, "procedure_destroy_value", procedureFn);
            auto *continueBlock = llvm::BasicBlock::Create(*context, "procedure_destroy_value_done", procedureFn);
            procBuilder.CreateCondBr(ownsStorage, destroyBlock, continueBlock);

            procBuilder.SetInsertPoint(destroyBlock);
            procBuilder.CreateCall(valueDestroyPtr, {valueSlot});
            procBuilder.CreateBr(continueBlock);

            procBuilder.SetInsertPoint(continueBlock);
        };
        auto emitProcValueExprToNumber = [&](const SExpr *expr, const llvm::Twine &name) -> llvm::Value * {
            llvm::AllocaInst *valueSlot = createProcAlloca(svalueTy, nullptr, name + "_value_slot");
            emitProcValueInto(expr, valueSlot);
            llvm::Value *number = procBuilder.CreateCall(valuePtrToNumber, {procRuntime, valueSlot}, name);
            destroyProcValueSlot(valueSlot);
            return number;
        };
        auto emitProcValueExprTruthy = [&](const SExpr *expr, const llvm::Twine &name) -> llvm::Value * {
            llvm::AllocaInst *valueSlot = createProcAlloca(svalueTy, nullptr, name + "_value_slot");
            emitProcValueInto(expr, valueSlot);
            llvm::Value *truthy = procBuilder.CreateCall(valueTruthy, {procRuntime, valueSlot}, name + "_raw");
            destroyProcValueSlot(valueSlot);
            return procBuilder.CreateICmpNE(truthy, llvm::ConstantInt::get(i32, 0), name);
        };
        auto storeProcScalarValue = [&](llvm::Value *valueSlot, int tag, llvm::Value *number) {
            procBuilder.CreateStore(
                llvm::ConstantInt::get(i32, tag),
                procBuilder.CreateStructGEP(svalueTy, valueSlot, 0));
            procBuilder.CreateStore(
                number,
                procBuilder.CreateStructGEP(svalueTy, valueSlot, 1));
            procBuilder.CreateStore(
                llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr)),
                procBuilder.CreateStructGEP(svalueTy, valueSlot, 2));
        };
        emitProcNumberExpr = [&](const SExpr *expr) -> llvm::Value * {
            double literal = 0.0;
            if (literalNumber(expr, literal)) {
                return llvm::ConstantFP::get(f64, literal);
            }
            if (!expr) {
                return llvm::ConstantFP::get(f64, 0.0);
            }
            switch (expr->opcode) {
            case SJIT_EXPR_LITERAL:
                return emitProcValueExprToNumber(expr, "procedure_literal_number");
            case SJIT_EXPR_TIMER: {
                llvm::Value *now = procRuntimeDoubleField(1, "procedure_runtime_now_ms");
                llvm::Value *timerStart = procRuntimeDoubleField(18, "procedure_timer_start_ms");
                return procBuilder.CreateFDiv(
                    procBuilder.CreateFSub(now, timerStart, "procedure_timer_elapsed_ms"),
                    llvm::ConstantFP::get(f64, 1000.0),
                    "procedure_timer_seconds");
            }
            case SJIT_EXPR_DAYS_SINCE_2000:
                return procBuilder.CreateCall(daysSince2000, {}, "procedure_days_since_2000");
            case SJIT_EXPR_MOUSE_X:
                return procRuntimeInputDoubleField(2, "procedure_mouse_x");
            case SJIT_EXPR_MOUSE_Y:
                return procRuntimeInputDoubleField(3, "procedure_mouse_y");
            case SJIT_EXPR_X_POSITION:
                return emitProcSpriteCoordinate(3, spriteX, "procedure_x_position");
            case SJIT_EXPR_Y_POSITION:
                return emitProcSpriteCoordinate(4, spriteY, "procedure_y_position");
            case SJIT_EXPR_ARGUMENT: {
                const int index = procedureArgumentIndex(procedure, expr);
                if (index < 0) {
                    return llvm::ConstantFP::get(f64, 0.0);
                }
                llvm::Value *slot = procBuilder.CreateInBoundsGEP(
                    f64,
                    procArgs,
                    llvm::ConstantInt::get(i32, index),
                    "procedure_argument_ptr");
                return procBuilder.CreateLoad(f64, slot, "procedure_argument");
            }
            case SJIT_EXPR_VARIABLE:
                if (fastVariableAccess) {
                    return emitProcVariableNumber(expr);
                }
                return procBuilder.CreateCall(
                    exprVariableNumber,
                    {procRuntime, llvm::ConstantInt::get(i32, script.target_id), constantPointer(expr)},
                    "procedure_variable_number");
            case SJIT_EXPR_LIST_LENGTH:
                return procBuilder.CreateCall(
                    listVariableLengthNumber,
                    {emitProcListVariable(expr)},
                    "procedure_list_length");
            case SJIT_EXPR_LIST_ITEM:
                if (fastVariableAccess && expr->left && expr->left->opcode == SJIT_EXPR_VARIABLE) {
                    if (SVariable *indexVariable = preboundExprVariable(
                            expr->left,
                            SJIT_VAR_SCALAR);
                        directProcedureListNumbers && indexVariable &&
                        indexVariable->scalar_kind == SJIT_SCALAR_NUMBER) {
                        SList *knownList = preboundListPointers ?
                            preboundExprList(expr) : nullptr;
                        return procBuilder.CreateCall(
                            fastListNumber,
                            {
                                procRuntime,
                                emitProcListVariable(expr),
                                knownList ? constantPointer(knownList) :
                                    llvm::ConstantPointerNull::get(ptr),
                                emitProcVariableNumber(expr->left),
                            },
                            "procedure_list_item_cached_number");
                    }
                    return procBuilder.CreateCall(
                        speculativeNumericListIndices ?
                            fastListNumberAtVariable : listVariableItemNumberAtVariable,
                        {
                            procRuntime,
                            emitProcListVariable(expr),
                            emitProcScalarVariable(expr->left),
                        },
                        "procedure_list_item_number_at_variable");
                }
                if (fastVariableAccess && expr->left && expr->left->opcode == SJIT_EXPR_ARGUMENT) {
                    return procBuilder.CreateCall(
                        speculativeNumericListIndices ?
                            fastListNumberAtArgument : listVariableItemNumberAtArgument,
                        {
                            procRuntime,
                            emitProcListVariable(expr),
                            procArgs,
                            procValueArgs,
                            llvm::ConstantInt::get(i32, procedureArgumentIndex(procedure, expr->left)),
                        },
                        "procedure_list_item_number_at_argument");
                }
                if (fastVariableAccess && procedureExprHasNumberResult(expr->left)) {
                    llvm::Value *index = emitProcNumberExpr(expr->left);
                    llvm::Value *listVariable = emitProcListVariable(expr);
                    if (directProcedureListNumbers) {
                        return procBuilder.CreateCall(
                            fastListNumber,
                            {
                                procRuntime,
                                listVariable,
                                llvm::ConstantPointerNull::get(ptr),
                                index,
                            },
                            "procedure_list_item_cached_number");
                    }
                    return procBuilder.CreateCall(
                        listVariableItemNumberAt,
                        {procRuntime, listVariable, index},
                        "procedure_list_item_number_at");
                }
                return emitProcValueExprToNumber(expr, "procedure_list_item_number");
            case SJIT_EXPR_LIST_ITEM_NUMBER: {
                llvm::AllocaInst *itemSlot = createProcAlloca(svalueTy, nullptr, "procedure_list_item_number_item");
                emitProcValueInto(expr->left, itemSlot);
                llvm::Value *number = procBuilder.CreateCall(
                    listVariableItemNumberValue,
                    {procRuntime, emitProcListVariable(expr), itemSlot},
                    "procedure_list_item_number");
                destroyProcValueSlot(itemSlot);
                return number;
            }
            case SJIT_EXPR_LENGTH: {
                int length = 0;
                if (literalStringLength(expr->left, length)) {
                    return llvm::ConstantFP::get(f64, (double)length);
                }
                llvm::AllocaInst *valueSlot = createProcAlloca(svalueTy, nullptr, "procedure_length_value");
                emitProcValueInto(expr->left, valueSlot);
                llvm::Value *number = procBuilder.CreateCall(
                    valueLengthNumber,
                    {procRuntime, valueSlot},
                    "procedure_length_number");
                destroyProcValueSlot(valueSlot);
                return number;
            }
            case SJIT_EXPR_JOIN:
            case SJIT_EXPR_LETTER_OF:
                return emitProcValueExprToNumber(expr, "procedure_value_number");
            case SJIT_EXPR_MATHOP: {
                llvm::Value *operand = emitProcNumberExpr(expr->left);
                if (llvm::Value *direct = emitDirectSimpleMathop(
                        procBuilder,
                        expr,
                        operand,
                        "procedure_mathop_direct")) {
                    return direct;
                }
                return procBuilder.CreateCall(
                    mathopNumber,
                    {procRuntime, constantPointer(expr), operand},
                    "procedure_mathop_number");
            }
            case SJIT_EXPR_ROUND:
                return procBuilder.CreateCall(
                    roundNumber,
                    {emitProcNumberExpr(expr->left)},
                    "procedure_round_number");
            case SJIT_EXPR_ADD:
                return procBuilder.CreateFAdd(
                    emitProcNumberExpr(expr->left),
                    emitProcNumberExpr(expr->right),
                    "procedure_number_add");
            case SJIT_EXPR_SUB:
                return procBuilder.CreateFSub(
                    emitProcNumberExpr(expr->left),
                    emitProcNumberExpr(expr->right),
                    "procedure_number_sub");
            case SJIT_EXPR_MUL:
                return procBuilder.CreateFMul(
                    emitProcNumberExpr(expr->left),
                    emitProcNumberExpr(expr->right),
                    "procedure_number_mul");
            case SJIT_EXPR_DIV:
                return procBuilder.CreateFDiv(
                    emitProcNumberExpr(expr->left),
                    emitProcNumberExpr(expr->right),
                    "procedure_number_div");
            case SJIT_EXPR_MOD: {
                llvm::Value *left = emitProcNumberExpr(expr->left);
                llvm::Value *right = emitProcNumberExpr(expr->right);
                llvm::Value *remainder = procBuilder.CreateFRem(left, right, "procedure_number_mod");
                llvm::Value *quotient = procBuilder.CreateFDiv(remainder, right, "procedure_number_mod_sign");
                llvm::Value *negative = procBuilder.CreateFCmpOLT(
                    quotient,
                    llvm::ConstantFP::get(f64, 0.0),
                    "procedure_number_mod_negative");
                return procBuilder.CreateSelect(
                    negative,
                    procBuilder.CreateFAdd(remainder, right, "procedure_number_mod_adjusted"),
                    remainder,
                    "procedure_number_mod_result");
            }
            case SJIT_EXPR_RANDOM:
                return procBuilder.CreateCall(
                    randomNumber,
                    {emitProcNumberExpr(expr->left), emitProcNumberExpr(expr->right)},
                    "procedure_random_number");
            case SJIT_EXPR_MOUSE_DOWN:
            case SJIT_EXPR_KEY_PRESSED:
            case SJIT_EXPR_LIST_CONTAINS:
            case SJIT_EXPR_CONTAINS:
            case SJIT_EXPR_LT:
            case SJIT_EXPR_EQ:
            case SJIT_EXPR_GT:
            case SJIT_EXPR_AND:
            case SJIT_EXPR_OR:
            case SJIT_EXPR_NOT:
                return procBuilder.CreateUIToFP(emitProcBoolExpr(expr), f64, "procedure_bool_number");
            default:
                return llvm::ConstantFP::get(f64, 0.0);
            }
        };
        emitProcBoolExpr = [&](const SExpr *expr) -> llvm::Value * {
            bool literal = false;
            if (literalBool(expr, literal)) {
                return llvm::ConstantInt::get(i1, literal);
            }
            if (!expr) {
                return llvm::ConstantInt::getFalse(*context);
            }
            switch (expr->opcode) {
            case SJIT_EXPR_MOUSE_DOWN:
                return procBuilder.CreateICmpNE(
                    procBuilder.CreateOr(
                        procRuntimeInputIntField(4, "procedure_mouse_down"),
                        procRuntimeInputIntField(9, "procedure_mouse_pressed_edge"),
                        "procedure_mouse_down_or_edge"),
                    llvm::ConstantInt::get(i32, 0),
                    "procedure_mouse_down_bool");
            case SJIT_EXPR_KEY_PRESSED: {
                const int keyIndex = literalKeyIndex(expr->left);
                if (keyIndex < 0) {
                    llvm::AllocaInst *keySlot = createProcAlloca(svalueTy, nullptr, "procedure_key_value");
                    emitProcValueInto(expr->left, keySlot);
                    llvm::Value *pressed = procBuilder.CreateCall(keyPressedValue, {procRuntime, keySlot}, "procedure_key_pressed_value");
                    destroyProcValueSlot(keySlot);
                    return procBuilder.CreateICmpNE(pressed, llvm::ConstantInt::get(i32, 0), "procedure_key_pressed_bool");
                }
                llvm::Value *input = procBuilder.CreateStructGEP(runtimeTy, procRuntime, 0, "procedure_key_input_ptr");
                llvm::Value *keyDown = procBuilder.CreateStructGEP(inputTy, input, 5, "procedure_key_down_ptr");
                llvm::Value *keyPtr = procBuilder.CreateInBoundsGEP(
                    keyStateTy,
                    keyDown,
                    {llvm::ConstantInt::get(i32, 0), llvm::ConstantInt::get(i32, keyIndex)},
                    "procedure_key_down_slot");
                llvm::Value *pressed = procBuilder.CreateLoad(i32, keyPtr, "procedure_key_down");
                return procBuilder.CreateICmpNE(pressed, llvm::ConstantInt::get(i32, 0), "procedure_key_pressed_bool");
            }
            case SJIT_EXPR_LIST_CONTAINS: {
                llvm::AllocaInst *itemSlot = createProcAlloca(svalueTy, nullptr, "procedure_list_contains_item");
                emitProcValueInto(expr->left, itemSlot);
                llvm::Value *contains = procBuilder.CreateCall(
                    listVariableContainsValue,
                    {procRuntime, emitProcListVariable(expr), itemSlot},
                    "procedure_list_contains");
                destroyProcValueSlot(itemSlot);
                return procBuilder.CreateICmpNE(
                    contains,
                    llvm::ConstantInt::get(i32, 0),
                    "procedure_list_contains_bool");
            }
            case SJIT_EXPR_CONTAINS: {
                llvm::AllocaInst *textSlot = createProcAlloca(
                    svalueTy,
                    nullptr,
                    "procedure_contains_text");
                llvm::AllocaInst *substringSlot = createProcAlloca(
                    svalueTy,
                    nullptr,
                    "procedure_contains_substring");
                emitProcValueInto(expr->left, textSlot);
                emitProcValueInto(expr->right, substringSlot);
                llvm::Value *contains = procBuilder.CreateCall(
                    valueContains,
                    {procRuntime, textSlot, substringSlot},
                    "procedure_contains");
                destroyProcValueSlot(textSlot);
                destroyProcValueSlot(substringSlot);
                return procBuilder.CreateICmpNE(
                    contains,
                    llvm::ConstantInt::get(i32, 0),
                    "procedure_contains_bool");
            }
            case SJIT_EXPR_VARIABLE: {
                llvm::Value *variable = procBuilder.CreateCall(
                    exprScalarVariable,
                    {procRuntime, llvm::ConstantInt::get(i32, script.target_id), constantPointer(expr)},
                    "procedure_bool_variable");
                llvm::Value *truthy = procBuilder.CreateCall(variableTruthy, {procRuntime, variable}, "procedure_variable_truthy");
                return procBuilder.CreateICmpNE(truthy, llvm::ConstantInt::get(i32, 0), "procedure_variable_bool");
            }
            case SJIT_EXPR_LT:
            case SJIT_EXPR_EQ:
            case SJIT_EXPR_GT: {
                auto hasDirectNumericComparisonValue = [&](const SExpr *operand) {
                    if (!operand) {
                        return false;
                    }
                    double literal = 0.0;
                    if (procedureExprHasNumberResult(operand) ||
                        (directNumericLiteralComparisons && literalNumber(operand, literal))) {
                        return true;
                    }
                    if (directNumericLiteralComparisons && fastVariableAccess &&
                        operand->opcode == SJIT_EXPR_VARIABLE) {
                        SVariable *bound = preboundExprVariable(operand, SJIT_VAR_SCALAR);
                        return bound && bound->scalar_kind == SJIT_SCALAR_NUMBER;
                    }
                    return false;
                };
                if (hasDirectNumericComparisonValue(expr->left) &&
                    hasDirectNumericComparisonValue(expr->right)) {
                    llvm::Value *left = emitProcNumberExpr(expr->left);
                    llvm::Value *right = emitProcNumberExpr(expr->right);
                    if (expr->opcode == SJIT_EXPR_LT) {
                        return procBuilder.CreateFCmpOLT(left, right, "procedure_number_lt");
                    }
                    if (expr->opcode == SJIT_EXPR_EQ) {
                        return procBuilder.CreateFCmpOEQ(left, right, "procedure_number_eq");
                    }
                    return procBuilder.CreateFCmpOGT(left, right, "procedure_number_gt");
                }
                if (fastVariableAccess &&
                    expr->left && expr->left->opcode == SJIT_EXPR_LIST_ITEM &&
                    expr->left->left && expr->left->left->opcode == SJIT_EXPR_VARIABLE &&
                    expr->right && expr->right->opcode == SJIT_EXPR_LITERAL) {
                    llvm::Value *compared = procBuilder.CreateCall(
                        listItemCompareLiteralAtVariable,
                        {
                            procRuntime,
                            llvm::ConstantInt::get(i32, script.target_id),
                            constantPointer(expr->left),
                            emitProcScalarVariable(expr->left->left),
                            constantPointer(expr->right),
                            llvm::ConstantInt::get(i32, expr->opcode),
                        },
                        "procedure_list_item_literal_compare");
                    return procBuilder.CreateICmpNE(
                        compared,
                        llvm::ConstantInt::get(i32, 0),
                        "procedure_list_item_literal_compare_bool");
                }
                auto numericTextLiteral = [&](const SExpr *operand, double &number) {
                    return directNumericLiteralComparisons && operand &&
                        operand->opcode == SJIT_EXPR_LITERAL &&
                        operand->literal.tag == SJIT_VALUE_STRING &&
                        literalNumber(operand, number);
                };
                double numericText = 0.0;
                const SExpr *dynamicOperand = nullptr;
                const SExpr *literalOperand = nullptr;
                bool dynamicIsLeft = true;
                if (numericTextLiteral(expr->right, numericText)) {
                    dynamicOperand = expr->left;
                    literalOperand = expr->right;
                } else if (numericTextLiteral(expr->left, numericText)) {
                    dynamicOperand = expr->right;
                    literalOperand = expr->left;
                    dynamicIsLeft = false;
                }
                if (dynamicOperand && literalOperand) {
                    llvm::AllocaInst *dynamicSlot = createProcAlloca(
                        svalueTy,
                        nullptr,
                        "procedure_numeric_text_compare_value");
                    emitProcValueInto(dynamicOperand, dynamicSlot);
                    llvm::Value *compared = procBuilder.CreateCall(
                        fastNumericTextCompare,
                        {
                            procRuntime,
                            dynamicSlot,
                            constantPointer(&literalOperand->literal),
                            llvm::ConstantFP::get(f64, numericText),
                            llvm::ConstantInt::get(i32, expr->opcode),
                            llvm::ConstantInt::get(i1, dynamicIsLeft),
                        },
                        "procedure_numeric_text_compare");
                    destroyProcValueSlot(dynamicSlot);
                    return procBuilder.CreateICmpNE(
                        compared,
                        llvm::ConstantInt::get(i32, 0),
                        "procedure_numeric_text_compare_bool");
                }
                auto emitCompareOperand = [&](const SExpr *operand, const char *slotName) {
                    if (borrowedCompareArguments && operand &&
                        operand->opcode == SJIT_EXPR_ARGUMENT) {
                        const int index = procedureArgumentIndex(procedure, operand);
                        if (index >= 0) {
                            llvm::AllocaInst *scratch = createProcAlloca(
                                svalueTy,
                                nullptr,
                                llvm::Twine(slotName) + "_scratch");
                            llvm::Value *borrowed = procBuilder.CreateCall(
                                fastArgumentValuePointer,
                                {
                                    procArgs,
                                    procValueArgs,
                                    llvm::ConstantInt::get(i32, index),
                                    scratch,
                                },
                                llvm::Twine(slotName) + "_borrowed");
                            return std::pair<llvm::Value *, llvm::AllocaInst *>(
                                borrowed,
                                nullptr);
                        }
                    }
                    if (borrowedCompareArguments && operand &&
                        operand->opcode == SJIT_EXPR_VARIABLE) {
                        if (SVariable *bound = preboundExprVariable(
                                operand,
                                SJIT_VAR_SCALAR)) {
                            return std::pair<llvm::Value *, llvm::AllocaInst *>(
                                procBuilder.CreateStructGEP(
                                    variableTy,
                                    constantPointer(bound),
                                    5,
                                    llvm::Twine(slotName) + "_borrowed_variable"),
                                nullptr);
                        }
                    }
                    llvm::AllocaInst *owned = createProcAlloca(
                        svalueTy,
                        nullptr,
                        slotName);
                    emitProcValueInto(operand, owned);
                    return std::pair<llvm::Value *, llvm::AllocaInst *>(owned, owned);
                };
                auto leftOperand = emitCompareOperand(expr->left, "procedure_compare_left");
                auto rightOperand = emitCompareOperand(expr->right, "procedure_compare_right");
                llvm::Value *compared = procBuilder.CreateCall(
                    directCachedComparisons ? fastCachedValueCompare : valueCompare,
                    {
                        procRuntime,
                        leftOperand.first,
                        rightOperand.first,
                        llvm::ConstantInt::get(i32, expr->opcode),
                    },
                    "procedure_value_compare");
                if (leftOperand.second) {
                    destroyProcValueSlot(leftOperand.second);
                }
                if (rightOperand.second) {
                    destroyProcValueSlot(rightOperand.second);
                }
                return procBuilder.CreateICmpNE(
                    compared,
                    llvm::ConstantInt::get(i32, 0),
                    "procedure_value_compare_bool");
            }
            case SJIT_EXPR_AND:
                return procBuilder.CreateAnd(
                    emitProcBoolExpr(expr->left),
                    emitProcBoolExpr(expr->right),
                    "procedure_logic_and");
            case SJIT_EXPR_OR:
                return procBuilder.CreateOr(
                    emitProcBoolExpr(expr->left),
                    emitProcBoolExpr(expr->right),
                    "procedure_logic_or");
            case SJIT_EXPR_NOT:
                return procBuilder.CreateNot(emitProcBoolExpr(expr->left), "procedure_logic_not");
            default:
                if (procedureExprHasNumberResult(expr) && procedureCanEvaluateNumberExpr(procedure, expr)) {
                    return procBuilder.CreateFCmpONE(
                        emitProcNumberExpr(expr),
                        llvm::ConstantFP::get(f64, 0.0),
                        "procedure_number_truthy");
                }
                if (procedureSupportsValueExpr(procedure, expr)) {
                    return emitProcValueExprTruthy(expr, "procedure_value_truthy");
                }
                return procBuilder.CreateFCmpONE(
                    emitProcNumberExpr(expr),
                    llvm::ConstantFP::get(f64, 0.0),
                    "procedure_number_truthy");
            }
        };

        emitProcValueInto = [&](const SExpr *expr, llvm::Value *valueSlot) {
            if (!valueSlot) {
                return;
            }
            if (!expr) {
                storeProcScalarValue(
                    valueSlot,
                    SJIT_VALUE_NUMBER,
                    llvm::ConstantFP::get(f64, 0.0));
                return;
            }
            switch (expr->opcode) {
            case SJIT_EXPR_LITERAL:
                if (expr->literal.tag == SJIT_VALUE_NUMBER ||
                    expr->literal.tag == SJIT_VALUE_BOOL ||
                    expr->literal.tag == SJIT_VALUE_NULL) {
                    storeProcScalarValue(
                        valueSlot,
                        expr->literal.tag,
                        llvm::ConstantFP::get(f64, expr->literal.number));
                    return;
                }
                procBuilder.CreateCall(exprLiteralValue, {constantPointer(expr), valueSlot});
                return;
            case SJIT_EXPR_ARGUMENT: {
                const int index = procedureArgumentIndex(procedure, expr);
                procBuilder.CreateCall(
                    procedureArgumentValueAt,
                    {procArgs, procValueArgs, llvm::ConstantInt::get(i32, index), valueSlot});
                return;
            }
            case SJIT_EXPR_VARIABLE:
                if (SVariable *bound = preboundExprVariable(expr, SJIT_VAR_SCALAR);
                    bound && bound->scalar_kind == SJIT_SCALAR_NUMBER) {
                    storeProcScalarValue(
                        valueSlot,
                        SJIT_VALUE_NUMBER,
                        emitProcVariableNumber(expr));
                    return;
                }
                procBuilder.CreateCall(
                    variableValue,
                    {emitProcScalarVariable(expr), valueSlot});
                return;
            case SJIT_EXPR_LIST_ITEM: {
                if (directProcedureListValues &&
                    procedureSupportsNumberExpr(procedure, expr->left)) {
                    SList *knownList = preboundListPointers ? preboundExprList(expr) : nullptr;
                    procBuilder.CreateCall(
                        fastListValueAtNumber,
                        {
                            procRuntime,
                            emitProcListVariable(expr),
                            knownList ? constantPointer(knownList) :
                                llvm::ConstantPointerNull::get(ptr),
                            emitProcNumberExpr(expr->left),
                            valueSlot,
                        });
                    return;
                }
                llvm::AllocaInst *indexSlot = createProcAlloca(svalueTy, nullptr, "procedure_list_item_index");
                emitProcValueInto(expr->left, indexSlot);
                procBuilder.CreateCall(
                    listVariableItemValue,
                    {procRuntime, emitProcListVariable(expr), indexSlot, valueSlot});
                destroyProcValueSlot(indexSlot);
                return;
            }
            case SJIT_EXPR_JOIN: {
                llvm::AllocaInst *leftSlot = createProcAlloca(svalueTy, nullptr, "procedure_join_left");
                llvm::AllocaInst *rightSlot = createProcAlloca(svalueTy, nullptr, "procedure_join_right");
                emitProcValueInto(expr->left, leftSlot);
                emitProcValueInto(expr->right, rightSlot);
                procBuilder.CreateCall(valueJoinPtr, {procRuntime, leftSlot, rightSlot, valueSlot});
                destroyProcValueSlot(leftSlot);
                destroyProcValueSlot(rightSlot);
                return;
            }
            case SJIT_EXPR_LETTER_OF: {
                llvm::AllocaInst *indexSlot = createProcAlloca(svalueTy, nullptr, "procedure_letter_index");
                llvm::AllocaInst *textSlot = createProcAlloca(svalueTy, nullptr, "procedure_letter_text");
                emitProcValueInto(expr->left, indexSlot);
                emitProcValueInto(expr->right, textSlot);
                procBuilder.CreateCall(valueLetterOf, {procRuntime, indexSlot, textSlot, valueSlot});
                destroyProcValueSlot(indexSlot);
                destroyProcValueSlot(textSlot);
                return;
            }
            default:
                break;
            }
            if (procedureExprHasNumberResult(expr) && procedureCanEvaluateNumberExpr(procedure, expr)) {
                storeProcScalarValue(valueSlot, SJIT_VALUE_NUMBER, emitProcNumberExpr(expr));
                return;
            }
            if (procedureSupportsBoolExpr(procedure, expr)) {
                llvm::Value *truthy = emitProcBoolExpr(expr);
                storeProcScalarValue(
                    valueSlot,
                    SJIT_VALUE_BOOL,
                    procBuilder.CreateUIToFP(truthy, f64, "procedure_bool_value_number"));
                return;
            }
            storeProcScalarValue(
                valueSlot,
                SJIT_VALUE_NUMBER,
                llvm::ConstantFP::get(f64, 0.0));
        };

        llvm::Value *procedurePtr = constantPointer(&procedure);
        auto emitProcNumberSlot = [&](const SStatement *statement, int slot, const char *name) -> llvm::Value * {
            (void)name;
            const SExpr *expr = statement ? statementExprForSlot(*statement, slot) : nullptr;
            if (procedureCanEvaluateNumberExpr(procedure, expr)) {
                return emitProcNumberExpr(expr);
            }
            throw std::runtime_error("procedure number expression was not LLVM-lowerable after eligibility check");
        };
        auto emitProcBoolSlot = [&](const SStatement *statement, int slot, const char *name) -> llvm::Value * {
            (void)name;
            const SExpr *expr = statement ? statementExprForSlot(*statement, slot) : nullptr;
            if (procedureSupportsBoolExpr(procedure, expr)) {
                return emitProcBoolExpr(expr);
            }
            throw std::runtime_error("procedure bool expression was not LLVM-lowerable after eligibility check");
        };

        auto emitProcValueSlot = [&](const SStatement *statement, int slot, const char *name) -> llvm::AllocaInst * {
            llvm::AllocaInst *valueSlot = createProcAlloca(svalueTy, nullptr, name);
            const SExpr *expr = statement ? statementExprForSlot(*statement, slot) : nullptr;
            if (procedureSupportsValueExpr(procedure, expr)) {
                emitProcValueInto(expr, valueSlot);
                return valueSlot;
            }
            throw std::runtime_error("procedure value expression was not LLVM-lowerable after eligibility check");
        };

        auto emitProcStatusCheck = [&](llvm::Value *status) {
            if (auto *constantStatus = llvm::dyn_cast<llvm::ConstantInt>(status);
                constantStatus && constantStatus->getSExtValue() == SJIT_STATUS_OK) {
                return;
            }
            auto *returnStatus = llvm::BasicBlock::Create(*context, "procedure_return_status", procedureFn);
            auto *continueBlock = llvm::BasicBlock::Create(*context, "procedure_status_ok", procedureFn);
            procBuilder.CreateCondBr(
                procBuilder.CreateICmpNE(status, llvm::ConstantInt::get(i32, SJIT_STATUS_OK)),
                returnStatus,
                continueBlock);

            procBuilder.SetInsertPoint(returnStatus);
            procBuilder.CreateRet(status);

            procBuilder.SetInsertPoint(continueBlock);
        };

        std::function<llvm::Value *(const SStatement *)> emitProcProcedureCall = [&](const SStatement *statement) -> llvm::Value * {
            const int target = findProcedureIndex(script, statement ? statement->procedure_name : nullptr);
            if (target < 0) {
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            }
            if (!statement || statement->argument_count < 0 ||
                (statement->argument_count > 0 && !statement->arguments)) {
                throw std::runtime_error("procedure call arguments were invalid during LLVM lowering");
            }
            llvm::Function *callee = procedureFunctions[static_cast<size_t>(target)];
            if (!callee) {
                return llvm::ConstantInt::get(i32, SJIT_STATUS_ERROR);
            }
            for (int i = 0; i < statement->argument_count; ++i) {
                if (!procedureSupportsValueExpr(procedure, statement->arguments[i].value)) {
                    throw std::runtime_error("procedure call argument was not LLVM-lowerable after eligibility check");
                }
            }
            llvm::Value *argArray = createProcAlloca(
                f64,
                llvm::ConstantInt::get(i32, statement->argument_count > 0 ? statement->argument_count : 1),
                "procedure_call_args");
            bool callNeedsValueArguments = false;
            for (int i = 0; i < statement->argument_count; ++i) {
                const bool calleeNeedsValue =
                    target >= (int)procedureArgumentNeedsValue.size() ||
                    i >= (int)procedureArgumentNeedsValue[static_cast<size_t>(target)].size() ||
                    procedureArgumentNeedsValue[static_cast<size_t>(target)][static_cast<size_t>(i)];
                if (calleeNeedsValue || !procedureCanEvaluateNumberExpr(
                        procedure,
                        statement->arguments[i].value)) {
                    callNeedsValueArguments = true;
                    break;
                }
            }
            llvm::Value *valueArgArray = callNeedsValueArguments ?
                static_cast<llvm::Value *>(createProcAlloca(
                    svalueTy,
                    llvm::ConstantInt::get(i32, statement->argument_count > 0 ? statement->argument_count : 1),
                    "procedure_call_value_args")) :
                llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr));
            for (int i = 0; i < statement->argument_count; ++i) {
                llvm::Value *slot = procBuilder.CreateInBoundsGEP(
                    f64,
                    argArray,
                    llvm::ConstantInt::get(i32, i),
                    "procedure_call_arg_ptr");
                SArgumentExpr *argument = &statement->arguments[i];
                const bool calleeNeedsValue =
                    target >= (int)procedureArgumentNeedsValue.size() ||
                    i >= (int)procedureArgumentNeedsValue[static_cast<size_t>(target)].size() ||
                    procedureArgumentNeedsValue[static_cast<size_t>(target)][static_cast<size_t>(i)];
                const bool numericOnly = !calleeNeedsValue &&
                    procedureCanEvaluateNumberExpr(procedure, argument->value);
                llvm::Value *valueSlot = callNeedsValueArguments ? procBuilder.CreateInBoundsGEP(
                    svalueTy,
                    valueArgArray,
                    llvm::ConstantInt::get(i32, i),
                    "procedure_call_value_arg_ptr") : nullptr;
                llvm::Value *argumentValue = nullptr;
                if (procedureExprHasNumberResult(argument->value) || numericOnly) {
                    argumentValue = emitProcNumberExpr(argument->value);
                    if (calleeNeedsValue) {
                        procBuilder.CreateStore(
                            llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER),
                            procBuilder.CreateStructGEP(svalueTy, valueSlot, 0));
                        procBuilder.CreateStore(
                            argumentValue,
                            procBuilder.CreateStructGEP(svalueTy, valueSlot, 1));
                        procBuilder.CreateStore(
                            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr)),
                            procBuilder.CreateStructGEP(svalueTy, valueSlot, 2));
                    }
                } else if (argument->value && argument->value->opcode == SJIT_EXPR_VARIABLE) {
                    argumentValue = procBuilder.CreateCall(
                        fastVariableArgumentValue,
                        {procRuntime, emitProcScalarVariable(argument->value), valueSlot},
                        "procedure_call_variable_arg");
                } else if (argument->value && argument->value->opcode == SJIT_EXPR_ARGUMENT) {
                    argumentValue = procBuilder.CreateCall(
                        fastProcedureArgumentCopy,
                        {
                            procArgs,
                            procValueArgs,
                            llvm::ConstantInt::get(
                                i32,
                                procedureArgumentIndex(procedure, argument->value)),
                            valueSlot,
                        },
                        "procedure_call_forwarded_arg");
                } else if (argument->value && argument->value->opcode == SJIT_EXPR_LIST_ITEM &&
                    argument->value->left && argument->value->left->opcode == SJIT_EXPR_LITERAL) {
                    argumentValue = procBuilder.CreateCall(
                        listItemArgumentLiteral,
                        {
                            procRuntime,
                            llvm::ConstantInt::get(i32, script.target_id),
                            constantPointer(argument->value),
                            valueSlot,
                        },
                        "procedure_call_list_item_literal_arg");
                } else if (argument->value && argument->value->opcode == SJIT_EXPR_LIST_ITEM &&
                    argument->value->left && argument->value->left->opcode == SJIT_EXPR_VARIABLE) {
                    argumentValue = procBuilder.CreateCall(
                        listItemArgumentAtVariable,
                        {
                            procRuntime,
                            llvm::ConstantInt::get(i32, script.target_id),
                            constantPointer(argument->value),
                            emitProcScalarVariable(argument->value->left),
                            valueSlot,
                        },
                        "procedure_call_list_item_variable_arg");
                } else {
                    emitProcValueInto(argument->value, valueSlot);
                    argumentValue = procBuilder.CreateCall(
                        valuePtrToNumber,
                        {procRuntime, valueSlot},
                        "procedure_call_arg_number");
                }
                procBuilder.CreateStore(argumentValue, slot);
            }

            llvm::Value *previousWarp = procBuilder.CreateLoad(i32, procFrameWarpModePtr(), "procedure_previous_warp");
            const bool enterWarpMode = script.procedures[target].warp_mode && !procedure.warp_mode;
            if (enterWarpMode) {
                procBuilder.CreateStore(llvm::ConstantInt::get(i32, 1), procFrameWarpModePtr());
            }
            llvm::Value *status = procBuilder.CreateCall(
                callee,
                {
                    procRuntime,
                    procThread,
                    procFrame,
                    procScriptData,
                    procBuilder.CreateAdd(procDepth, llvm::ConstantInt::get(i32, 1), "procedure_next_depth"),
                    argArray,
                    valueArgArray,
                },
                "procedure_call_status");
            if (enterWarpMode) {
                procBuilder.CreateStore(previousWarp, procFrameWarpModePtr());
            }
            for (int i = 0; i < statement->argument_count; ++i) {
                const bool calleeNeedsValue =
                    target >= (int)procedureArgumentNeedsValue.size() ||
                    i >= (int)procedureArgumentNeedsValue[static_cast<size_t>(target)].size() ||
                    procedureArgumentNeedsValue[static_cast<size_t>(target)][static_cast<size_t>(i)];
                if (procedureExprHasNumberResult(statement->arguments[i].value) ||
                    (!calleeNeedsValue && procedureCanEvaluateNumberExpr(
                        procedure,
                        statement->arguments[i].value))) {
                    continue;
                }
                llvm::Value *valueSlot = procBuilder.CreateInBoundsGEP(
                    svalueTy,
                    valueArgArray,
                    llvm::ConstantInt::get(i32, i),
                    "procedure_call_value_arg_destroy_ptr");
                destroyProcValueSlot(valueSlot);
            }
            return procBuilder.CreateSelect(
                procBuilder.CreateICmpEQ(status, llvm::ConstantInt::get(i32, SJIT_STATUS_DONE)),
                llvm::ConstantInt::get(i32, SJIT_STATUS_OK),
                status,
                "procedure_call_normalized_status");
        };

        std::function<bool(const SStatement *, int)> canInlineNonWarpLoopStatements =
            [&](const SStatement *statements, int count) -> bool {
            if (!procedureSupportsStatements(script, procedure, statements, count, nullptr)) {
                return false;
            }
            if (count <= 0) {
                return true;
            }
            if (!statements) {
                return false;
            }
            for (int i = 0; i < count; ++i) {
                const SStatement &substatement = statements[i];
                switch (substatement.opcode) {
                case SJIT_STMT_IF:
                    if (!canInlineNonWarpLoopStatements(substatement.substack, substatement.substack_count)) {
                        return false;
                    }
                    break;
                case SJIT_STMT_IF_ELSE:
                    if (!canInlineNonWarpLoopStatements(substatement.substack, substatement.substack_count) ||
                        !canInlineNonWarpLoopStatements(substatement.substack2, substatement.substack2_count)) {
                        return false;
                    }
                    break;
                case SJIT_STMT_REPEAT:
                case SJIT_STMT_REPEAT_UNTIL:
                case SJIT_STMT_WHILE:
                case SJIT_STMT_FOREVER:
                case SJIT_STMT_FOR_EACH:
                    if (!canInlineNonWarpLoopStatements(substatement.substack, substatement.substack_count)) {
                        return false;
                    }
                    break;
                case SJIT_STMT_PROCEDURE_CALL: {
                    const int target = findProcedureIndex(script, substatement.procedure_name);
                    if (target >= 0 && !procedureFunctions[static_cast<size_t>(target)]) {
                        return false;
                    }
                    for (int argument = 0; argument < substatement.argument_count; ++argument) {
                        if (!substatement.arguments || !substatement.arguments[argument].value) {
                            return false;
                        }
                    }
                    break;
                }
                default:
                    break;
                }
            }
            return true;
        };

        auto canInlineNonWarpLoopSubstack = [&](const SStatement *statement) -> bool {
            if (!statement) {
                return false;
            }
            switch (statement->opcode) {
            case SJIT_STMT_REPEAT:
            case SJIT_STMT_REPEAT_UNTIL:
            case SJIT_STMT_WHILE:
            case SJIT_STMT_FOREVER:
            case SJIT_STMT_FOR_EACH:
                return canInlineNonWarpLoopStatements(statement->substack, statement->substack_count);
            default:
                return false;
            }
        };

        std::function<void(const SStatement *, int)> emitProcStatements;
        std::function<void(const SStatement *, int, llvm::Value *, const std::string &)> emitProcStatefulStatements;
        std::function<void(const SStatement *)> emitProcStatement = [&](const SStatement *statement) {
            if (!statement || procBuilder.GetInsertBlock()->getTerminator()) {
                return;
            }
            llvm::Value *statementPtr = constantPointer(statement);
            switch (statement->opcode) {
            case SJIT_STMT_NOOP:
                break;
            case SJIT_STMT_RESET_TIMER:
                procBuilder.CreateCall(resetTimer, {procRuntime});
                break;
            case SJIT_STMT_STOP_THIS_SCRIPT:
                procBuilder.CreateCall(procedureScopeReset, {procFrame, procDepth});
                procBuilder.CreateRet(llvm::ConstantInt::get(i32, SJIT_STATUS_DONE));
                break;
            case SJIT_STMT_SET_VARIABLE:
                if (fastVariableAccess && statement->value &&
                    statement->value->opcode == SJIT_EXPR_LITERAL &&
                    !procedureExprHasNumberResult(statement->value)) {
                    procBuilder.CreateCall(
                        variableSetFromLiteral,
                        {
                            procRuntime,
                            emitProcStatementScalarVariable(statement),
                            constantPointer(statement->value),
                        });
                } else if (fastVariableAccess && statement->value &&
                    statement->value->opcode == SJIT_EXPR_VARIABLE) {
                    SVariable *destination = preboundStatementVariable(
                        statement,
                        SJIT_VAR_SCALAR);
                    SVariable *source = preboundExprVariable(
                        statement->value,
                        SJIT_VAR_SCALAR);
                    if (destination && source &&
                        destination->scalar_kind == SJIT_SCALAR_NUMBER &&
                        source->scalar_kind == SJIT_SCALAR_NUMBER) {
                        emitProcSetVariableNumber(
                            statement,
                            emitProcVariableNumber(statement->value));
                    } else {
                        procBuilder.CreateCall(
                            speculativeNumericSets ? fastVariableSetFromVariable : variableSetFromVariable,
                            {
                                procRuntime,
                                emitProcStatementScalarVariable(statement),
                                emitProcScalarVariable(statement->value),
                            });
                    }
                } else if (fastVariableAccess && statement->value &&
                    statement->value->opcode == SJIT_EXPR_ARGUMENT) {
                    procBuilder.CreateCall(
                        speculativeNumericSets ? fastVariableSetFromArgument : variableSetFromArgument,
                        {
                            procRuntime,
                            emitProcStatementScalarVariable(statement),
                            procArgs,
                            procValueArgs,
                            llvm::ConstantInt::get(
                                i32,
                                procedureArgumentIndex(procedure, statement->value)),
                        });
                } else if (fastVariableAccess && statement->value &&
                    statement->value->opcode == SJIT_EXPR_LIST_ITEM &&
                    statement->value->left &&
                    statement->value->left->opcode == SJIT_EXPR_VARIABLE) {
                    llvm::Value *destination = emitProcStatementScalarVariable(statement);
                    llvm::Value *indexVariable = emitProcScalarVariable(statement->value->left);
                    if (directProcedureListValues &&
                        procedureSupportsNumberExpr(procedure, statement->value->left)) {
                        emitProcSetVariableFromNumericListItem(
                            destination,
                            emitProcListVariable(statement->value),
                            emitProcNumberExpr(statement->value->left),
                            [&](llvm::Value *fallbackIndex) {
                                procBuilder.CreateCall(
                                    variableSetFromListItemAtNumber,
                                    {
                                        procRuntime,
                                        llvm::ConstantInt::get(i32, script.target_id),
                                        destination,
                                        constantPointer(statement->value),
                                        fallbackIndex,
                                    });
                            });
                    } else {
                        procBuilder.CreateCall(
                            variableSetFromListItemAtVariable,
                            {
                                procRuntime,
                                llvm::ConstantInt::get(i32, script.target_id),
                                destination,
                                constantPointer(statement->value),
                                indexVariable,
                            });
                    }
                } else if (fastVariableAccess && statement->value &&
                    statement->value->opcode == SJIT_EXPR_LIST_ITEM &&
                    statement->value->left &&
                    statement->value->left->opcode == SJIT_EXPR_LITERAL) {
                    llvm::Value *destination = emitProcStatementScalarVariable(statement);
                    if (directProcedureListValues &&
                        procedureSupportsNumberExpr(procedure, statement->value->left)) {
                        emitProcSetVariableFromNumericListItem(
                            destination,
                            emitProcListVariable(statement->value),
                            emitProcNumberExpr(statement->value->left),
                            [&](llvm::Value *fallbackIndex) {
                                procBuilder.CreateCall(
                                    variableSetFromListItemAtNumber,
                                    {
                                        procRuntime,
                                        llvm::ConstantInt::get(i32, script.target_id),
                                        destination,
                                        constantPointer(statement->value),
                                        fallbackIndex,
                                    });
                            });
                    } else {
                        procBuilder.CreateCall(
                            variableSetFromListItemLiteral,
                            {
                                procRuntime,
                                llvm::ConstantInt::get(i32, script.target_id),
                                destination,
                                constantPointer(statement->value),
                            });
                    }
                } else if (fastVariableAccess && statement->value &&
                    statement->value->opcode == SJIT_EXPR_LIST_ITEM &&
                    statement->value->left &&
                    statement->value->left->opcode == SJIT_EXPR_ARGUMENT) {
                    llvm::Value *destination = emitProcStatementScalarVariable(statement);
                    const int argumentIndex = procedureArgumentIndex(
                        procedure,
                        statement->value->left);
                    if (directProcedureListValues &&
                        procedureSupportsNumberExpr(procedure, statement->value->left)) {
                        emitProcSetVariableFromNumericListItem(
                            destination,
                            emitProcListVariable(statement->value),
                            emitProcNumberExpr(statement->value->left),
                            [&](llvm::Value *fallbackIndex) {
                                procBuilder.CreateCall(
                                    variableSetFromListItemAtNumber,
                                    {
                                        procRuntime,
                                        llvm::ConstantInt::get(i32, script.target_id),
                                        destination,
                                        constantPointer(statement->value),
                                        fallbackIndex,
                                    });
                            });
                    } else {
                        procBuilder.CreateCall(
                            variableSetFromListItemAtArgument,
                            {
                                procRuntime,
                                llvm::ConstantInt::get(i32, script.target_id),
                                destination,
                                constantPointer(statement->value),
                                procArgs,
                                procValueArgs,
                                llvm::ConstantInt::get(i32, argumentIndex),
                            });
                    }
                } else if (procedureExprHasNumberResult(statement->value)) {
                    llvm::Value *number = emitProcNumberSlot(
                        statement,
                        SJIT_STMT_EXPR_VALUE,
                        "procedure_set_value");
                    if (fastVariableAccess) {
                        emitProcSetVariableNumber(statement, number);
                    } else {
                        procBuilder.CreateCall(
                            statementSetVariableNumber,
                            {procRuntime, procScriptData, statementPtr, number});
                    }
                } else {
                    llvm::AllocaInst *valueSlot = emitProcValueSlot(
                        statement,
                        SJIT_STMT_EXPR_VALUE,
                        "procedure_set_value_slot");
                    procBuilder.CreateCall(statementSetVariableValuePtr, {procRuntime, procScriptData, statementPtr, valueSlot});
                    destroyProcValueSlot(valueSlot);
                }
                break;
            case SJIT_STMT_CHANGE_VARIABLE:
                if (fastVariableAccess) {
                    emitProcChangeVariableNumber(
                        statement,
                        emitProcNumberSlot(statement, SJIT_STMT_EXPR_VALUE, "procedure_change_value"));
                } else {
                    procBuilder.CreateCall(
                        statementChangeVariableNumber,
                        {procRuntime, procScriptData, statementPtr, emitProcNumberSlot(statement, SJIT_STMT_EXPR_VALUE, "procedure_change_value")});
                }
                break;
            case SJIT_STMT_LIST_ADD:
                if (fastVariableAccess && statement->value &&
                    statement->value->opcode == SJIT_EXPR_LITERAL &&
                    !procedureExprHasNumberResult(statement->value)) {
                    procBuilder.CreateCall(
                        statementListAddFromLiteral,
                        {procRuntime, procScriptData, statementPtr});
                } else if (fastVariableAccess && statement->value &&
                    statement->value->opcode == SJIT_EXPR_VARIABLE) {
                    procBuilder.CreateCall(
                        statementListAddFromVariable,
                        {
                            procRuntime,
                            procScriptData,
                            statementPtr,
                            emitProcScalarVariable(statement->value),
                        });
                } else if (fastVariableAccess && statement->value &&
                    statement->value->opcode == SJIT_EXPR_ARGUMENT) {
                    procBuilder.CreateCall(
                        statementListAddFromArgument,
                        {
                            procRuntime,
                            procScriptData,
                            statementPtr,
                            procArgs,
                            procValueArgs,
                            llvm::ConstantInt::get(
                                i32,
                                procedureArgumentIndex(procedure, statement->value)),
                        });
                } else if (fastVariableAccess && statement->value &&
                    statement->value->opcode == SJIT_EXPR_LIST_ITEM &&
                    statement->value->left &&
                    statement->value->left->opcode == SJIT_EXPR_VARIABLE) {
                    procBuilder.CreateCall(
                        statementListAddListItemAtVariable,
                        {
                            procRuntime,
                            procScriptData,
                            statementPtr,
                            constantPointer(statement->value),
                            emitProcScalarVariable(statement->value->left),
                        });
                } else if (procedureExprHasNumberResult(statement->value)) {
                    procBuilder.CreateCall(
                        listVariableAddNumber,
                        {emitProcStatementListVariable(statement), emitProcNumberSlot(statement, SJIT_STMT_EXPR_VALUE, "procedure_list_add_value")});
                } else {
                    llvm::AllocaInst *valueSlot = emitProcValueSlot(
                        statement,
                        SJIT_STMT_EXPR_VALUE,
                        "procedure_list_add_value_slot");
                    procBuilder.CreateCall(statementListAddValuePtr, {procRuntime, procScriptData, statementPtr, valueSlot});
                    destroyProcValueSlot(valueSlot);
                }
                break;
            case SJIT_STMT_LIST_DELETE:
                if (procedureExprHasNumberResult(statement->index)) {
                    procBuilder.CreateCall(
                        statementListDeleteAtNumber,
                        {
                            procRuntime,
                            procScriptData,
                            statementPtr,
                            emitProcNumberSlot(statement, SJIT_STMT_EXPR_INDEX, "procedure_list_delete_index"),
                        });
                } else {
                    llvm::AllocaInst *indexSlot = emitProcValueSlot(
                        statement,
                        SJIT_STMT_EXPR_INDEX,
                        "procedure_list_delete_index_slot");
                    procBuilder.CreateCall(
                        statementListDeleteIndexValuePtr,
                        {procRuntime, procScriptData, statementPtr, indexSlot});
                    destroyProcValueSlot(indexSlot);
                }
                break;
            case SJIT_STMT_LIST_DELETE_ALL:
                procBuilder.CreateCall(listVariableClear, {emitProcStatementListVariable(statement)});
                break;
            case SJIT_STMT_LIST_INSERT:
                if (procedureExprHasNumberResult(statement->index) &&
                    procedureExprHasNumberResult(statement->value)) {
                    procBuilder.CreateCall(
                        statementListInsertNumberAt,
                        {
                            procRuntime,
                            procScriptData,
                            statementPtr,
                            emitProcNumberSlot(statement, SJIT_STMT_EXPR_INDEX, "procedure_list_insert_index"),
                            emitProcNumberSlot(statement, SJIT_STMT_EXPR_VALUE, "procedure_list_insert_value"),
                        });
                } else {
                    llvm::AllocaInst *indexSlot = emitProcValueSlot(
                        statement,
                        SJIT_STMT_EXPR_INDEX,
                        "procedure_list_insert_index_slot");
                    llvm::AllocaInst *valueSlot = emitProcValueSlot(
                        statement,
                        SJIT_STMT_EXPR_VALUE,
                        "procedure_list_insert_value_slot");
                    procBuilder.CreateCall(
                        statementListInsertValuePtr,
                        {procRuntime, procScriptData, statementPtr, indexSlot, valueSlot});
                    destroyProcValueSlot(indexSlot);
                    destroyProcValueSlot(valueSlot);
                }
                break;
            case SJIT_STMT_LIST_REPLACE:
                if (fastVariableAccess && statement->index && statement->index->opcode == SJIT_EXPR_VARIABLE &&
                    statement->value && statement->value->opcode == SJIT_EXPR_LITERAL) {
                    procBuilder.CreateCall(
                        listReplaceLiteralAtVariable,
                        {
                            procRuntime,
                            procScriptData,
                            statementPtr,
                            emitProcScalarVariable(statement->index),
                        });
                } else if (fastVariableAccess && statement->index &&
                    statement->index->opcode == SJIT_EXPR_VARIABLE &&
                    statement->value && statement->value->opcode == SJIT_EXPR_LIST_ITEM &&
                    statement->value->left &&
                    statement->value->left->opcode == SJIT_EXPR_VARIABLE) {
                    procBuilder.CreateCall(
                        directProcedureListReplace ?
                            fastListReplaceListItemAtVariables : listVariableReplaceListItemAtVariables,
                        {
                            procRuntime,
                            emitProcStatementListVariable(statement),
                            emitProcScalarVariable(statement->index),
                            emitProcListVariable(statement->value),
                            emitProcScalarVariable(statement->value->left),
                        });
                } else if (fastVariableAccess && statement->index &&
                    statement->index->opcode == SJIT_EXPR_VARIABLE &&
                    statement->value && statement->value->opcode == SJIT_EXPR_VARIABLE) {
                    procBuilder.CreateCall(
                        directProcedureListReplace ?
                            fastListReplaceFromVariables : listVariableReplaceFromVariables,
                        {
                            procRuntime,
                            emitProcStatementListVariable(statement),
                            emitProcScalarVariable(statement->index),
                            emitProcScalarVariable(statement->value),
                        });
                } else if (fastVariableAccess && statement->index &&
                    statement->index->opcode == SJIT_EXPR_VARIABLE &&
                    statement->value && statement->value->opcode == SJIT_EXPR_LIST_ITEM &&
                    procedureExprHasNumberResult(statement->value->left)) {
                    procBuilder.CreateCall(
                        listReplaceListItemAtVariableNumber,
                        {
                            procRuntime,
                            procScriptData,
                            statementPtr,
                            emitProcScalarVariable(statement->index),
                            constantPointer(statement->value),
                            emitProcNumberExpr(statement->value->left),
                        });
                } else if (fastVariableAccess && statement->index &&
                    statement->index->opcode == SJIT_EXPR_VARIABLE &&
                    procedureExprHasNumberResult(statement->value)) {
                    procBuilder.CreateCall(
                        speculativeNumericListIndices ?
                            fastListReplaceNumberAtVariable : listVariableReplaceNumberAtVariable,
                        {
                            procRuntime,
                            emitProcStatementListVariable(statement),
                            emitProcScalarVariable(statement->index),
                            emitProcNumberSlot(
                                statement,
                                SJIT_STMT_EXPR_VALUE,
                                "procedure_list_replace_value"),
                        });
                } else if (procedureExprHasNumberResult(statement->index) &&
                    procedureCanEvaluateNumberExpr(procedure, statement->index) &&
                    procedureExprHasNumberResult(statement->value)) {
                    procBuilder.CreateCall(
                        fastListReplaceNumberAt,
                        {
                            procRuntime,
                            emitProcStatementListVariable(statement),
                            emitProcNumberSlot(statement, SJIT_STMT_EXPR_INDEX, "procedure_list_replace_index"),
                            emitProcNumberSlot(statement, SJIT_STMT_EXPR_VALUE, "procedure_list_replace_value"),
                        });
                } else {
                    llvm::AllocaInst *indexSlot = emitProcValueSlot(
                        statement,
                        SJIT_STMT_EXPR_INDEX,
                        "procedure_list_replace_index_slot");
                    llvm::AllocaInst *valueSlot = emitProcValueSlot(
                        statement,
                        SJIT_STMT_EXPR_VALUE,
                        "procedure_list_replace_value_slot");
                    procBuilder.CreateCall(
                        statementListReplaceValuePtr,
                        {procRuntime, procScriptData, statementPtr, indexSlot, valueSlot});
                    destroyProcValueSlot(indexSlot);
                    destroyProcValueSlot(valueSlot);
                }
                break;
            case SJIT_STMT_PROCEDURE_CALL:
                emitProcStatusCheck(emitProcProcedureCall(statement));
                break;
            case SJIT_STMT_LOOKS_SWITCH_BACKDROP:
            {
                llvm::AllocaInst *valueSlot = emitProcValueSlot(
                    statement,
                    SJIT_STMT_EXPR_VALUE,
                    "procedure_backdrop_value_slot");
                procBuilder.CreateCall(
                    looksSwitchBackdropValuePtr,
                    {procRuntime, valueSlot});
                destroyProcValueSlot(valueSlot);
                break;
            }
            case SJIT_STMT_IF: {
                auto *thenBlock = llvm::BasicBlock::Create(*context, "procedure_if_body", procedureFn);
                auto *afterBlock = llvm::BasicBlock::Create(*context, "procedure_if_after", procedureFn);
                procBuilder.CreateCondBr(
                    emitProcBoolSlot(statement, SJIT_STMT_EXPR_CONDITION, "procedure_if_condition"),
                    thenBlock,
                    afterBlock);

                procBuilder.SetInsertPoint(thenBlock);
                emitProcStatements(statement->substack, statement->substack_count);
                if (!procBuilder.GetInsertBlock()->getTerminator()) {
                    procBuilder.CreateBr(afterBlock);
                }

                procBuilder.SetInsertPoint(afterBlock);
                break;
            }
            case SJIT_STMT_IF_ELSE: {
                auto *thenBlock = llvm::BasicBlock::Create(*context, "procedure_if_else_body", procedureFn);
                auto *elseBlock = llvm::BasicBlock::Create(*context, "procedure_if_else_else", procedureFn);
                auto *afterBlock = llvm::BasicBlock::Create(*context, "procedure_if_else_after", procedureFn);
                procBuilder.CreateCondBr(
                    emitProcBoolSlot(statement, SJIT_STMT_EXPR_CONDITION, "procedure_if_else_condition"),
                    thenBlock,
                    elseBlock);

                procBuilder.SetInsertPoint(thenBlock);
                emitProcStatements(statement->substack, statement->substack_count);
                if (!procBuilder.GetInsertBlock()->getTerminator()) {
                    procBuilder.CreateBr(afterBlock);
                }

                procBuilder.SetInsertPoint(elseBlock);
                emitProcStatements(statement->substack2, statement->substack2_count);
                if (!procBuilder.GetInsertBlock()->getTerminator()) {
                    procBuilder.CreateBr(afterBlock);
                }

                procBuilder.SetInsertPoint(afterBlock);
                break;
            }
            case SJIT_STMT_REPEAT: {
                if (!procedure.warp_mode) {
                    if (!canInlineNonWarpLoopSubstack(statement)) {
                        procBuilder.CreateRet(llvm::ConstantInt::get(i32, SJIT_STATUS_ERROR));
                        break;
                    }
                    llvm::AllocaInst *branchCountSlot = createProcAlloca(i32, nullptr, "procedure_repeat_branch_count");
                    procBuilder.CreateStore(llvm::ConstantInt::get(i32, 0), branchCountSlot);
                    auto *conditionBlock = llvm::BasicBlock::Create(*context, "procedure_repeat_nonwarp_condition", procedureFn);
                    auto *bodyBlock = llvm::BasicBlock::Create(*context, "procedure_repeat_nonwarp_body", procedureFn);
                    auto *afterBlock = llvm::BasicBlock::Create(*context, "procedure_repeat_nonwarp_after", procedureFn);
                    procBuilder.CreateBr(conditionBlock);

                    procBuilder.SetInsertPoint(conditionBlock);
                    llvm::Value *enter = procBuilder.CreateCall(
                        procedureRepeatShouldEnterNumber,
                        {
                            procFrame,
                            statementPtr,
                            procDepth,
                            emitProcNumberSlot(statement, SJIT_STMT_EXPR_TIMES, "procedure_repeat_times"),
                        },
                        "procedure_repeat_enter");
                    procBuilder.CreateCondBr(
                        procBuilder.CreateICmpNE(enter, llvm::ConstantInt::get(i32, 0)),
                        bodyBlock,
                        afterBlock);

                    procBuilder.SetInsertPoint(bodyBlock);
                    llvm::Value *state = procBuilder.CreateCall(
                        procedureControlLoopState,
                        {procFrame, statementPtr, procDepth, llvm::ConstantInt::get(i32, 0)},
                        "procedure_repeat_state");
                    procSetLoopBranchActive(state);
                    emitProcStatefulStatements(statement->substack, statement->substack_count, state, "procedure_repeat_nonwarp_sub");
                    if (!procBuilder.GetInsertBlock()->getTerminator()) {
                        llvm::Value *branchCount = procBuilder.CreateLoad(i32, branchCountSlot, "procedure_repeat_branch_count_value");
                        llvm::Value *nextBranchCount = procBuilder.CreateAdd(
                            branchCount,
                            llvm::ConstantInt::get(i32, 1),
                            "procedure_repeat_next_branch_count");
                        procBuilder.CreateStore(nextBranchCount, branchCountSlot);
                        llvm::Value *status = procFinishLoopBranch(
                            state,
                            nextBranchCount,
                            true,
                            "procedure_repeat_status");
                        auto *returnStatus = llvm::BasicBlock::Create(*context, "procedure_repeat_nonwarp_return", procedureFn);
                        auto *continueLoop = llvm::BasicBlock::Create(*context, "procedure_repeat_nonwarp_continue", procedureFn);
                        procBuilder.CreateCondBr(
                            procBuilder.CreateICmpNE(status, llvm::ConstantInt::get(i32, SJIT_STATUS_OK)),
                            returnStatus,
                            continueLoop);

                        procBuilder.SetInsertPoint(returnStatus);
                        procBuilder.CreateRet(status);

                        procBuilder.SetInsertPoint(continueLoop);
                        procBuilder.CreateBr(conditionBlock);
                    }

                    procBuilder.SetInsertPoint(afterBlock);
                    procBuilder.CreateCall(procedureControlLoopReset, {procFrame, statementPtr, procDepth});
                    break;
                }
                llvm::AllocaInst *counterSlot = createProcAlloca(f64, nullptr, "procedure_repeat_counter");
                llvm::Value *repeatTimes = emitProcNumberSlot(
                    statement,
                    SJIT_STMT_EXPR_TIMES,
                    "procedure_repeat_times");
                procBuilder.CreateStore(
                    procBuilder.CreateCall(
                        roundRepeatCount,
                        {repeatTimes},
                        "procedure_repeat_count"),
                    counterSlot);
                auto *conditionBlock = llvm::BasicBlock::Create(*context, "procedure_repeat_condition", procedureFn);
                auto *bodyBlock = llvm::BasicBlock::Create(*context, "procedure_repeat_body", procedureFn);
                auto *afterBlock = llvm::BasicBlock::Create(*context, "procedure_repeat_after", procedureFn);
                NativePenRowPattern nativePenRow;
                const bool hasNativePenRow = fastVariableAccess &&
                    matchNativePenRowPattern(statement, nativePenRow) &&
                    procedureCanEvaluateNumberExpr(procedure, nativePenRow.xStepExpr) &&
                    exprIsGuardReplaySafe(nativePenRow.xStepExpr);
                const SStatement *repeatedAdd = statement->substack_count == 1 && statement->substack &&
                    statement->substack[0].opcode == SJIT_STMT_LIST_ADD &&
                    statement->substack[0].value &&
                    statement->substack[0].value->opcode == SJIT_EXPR_LITERAL ?
                    &statement->substack[0] : nullptr;
                if (hasNativePenRow) {
                    llvm::Value *handled = procBuilder.CreateCall(
                        penRenderRowFromVariables,
                        {
                            procRuntime,
                            procTarget,
                            emitProcListVariable(nativePenRow.colorExpr),
                            emitProcListVariable(nativePenRow.brightnessExpr),
                            emitProcScalarVariable(nativePenRow.rowExpr),
                            emitProcScalarVariable(nativePenRow.columnExpr),
                            emitProcScalarVariable(nativePenRow.indexExpr),
                            repeatTimes,
                            emitProcNumberExpr(nativePenRow.xStepExpr),
                            constantPointer(nativePenRow.replacementLiteral),
                            llvm::ConstantInt::get(i32, nativePenRow.paramId),
                        },
                        "procedure_native_pen_row_handled");
                    procBuilder.CreateCondBr(
                        procBuilder.CreateICmpNE(
                            handled,
                            llvm::ConstantInt::get(i32, 0)),
                        afterBlock,
                        conditionBlock);
                } else if (repeatedAdd) {
                    llvm::Value *count = procBuilder.CreateLoad(f64, counterSlot, "procedure_repeat_bulk_count");
                    llvm::Value *handled = procBuilder.CreateCall(
                        statementListAddLiteralRepeated,
                        {procRuntime, procScriptData, constantPointer(repeatedAdd), count},
                        "procedure_repeat_bulk_handled");
                    procBuilder.CreateCondBr(
                        procBuilder.CreateICmpNE(handled, llvm::ConstantInt::get(i32, 0)),
                        afterBlock,
                        conditionBlock);
                } else {
                    procBuilder.CreateBr(conditionBlock);
                }

                procBuilder.SetInsertPoint(conditionBlock);
                llvm::Value *counter = procBuilder.CreateLoad(f64, counterSlot, "procedure_repeat_remaining");
                procBuilder.CreateCondBr(
                    procBuilder.CreateFCmpOGT(counter, llvm::ConstantFP::get(f64, 0.0)),
                    bodyBlock,
                    afterBlock);

                procBuilder.SetInsertPoint(bodyBlock);
                procBuilder.CreateStore(
                    procBuilder.CreateFSub(counter, llvm::ConstantFP::get(f64, 1.0), "procedure_repeat_next"),
                    counterSlot);
                emitProcStatements(statement->substack, statement->substack_count);
                if (!procBuilder.GetInsertBlock()->getTerminator()) {
                    procBuilder.CreateBr(conditionBlock);
                }

                procBuilder.SetInsertPoint(afterBlock);
                break;
            }
            case SJIT_STMT_REPEAT_UNTIL:
            case SJIT_STMT_WHILE: {
                if (!procedure.warp_mode) {
                    if (!canInlineNonWarpLoopSubstack(statement)) {
                        procBuilder.CreateRet(llvm::ConstantInt::get(i32, SJIT_STATUS_ERROR));
                        break;
                    }
                    llvm::AllocaInst *guardSlot = createProcAlloca(i32, nullptr, "procedure_loop_nonwarp_guard");
                    procBuilder.CreateStore(llvm::ConstantInt::get(i32, 0), guardSlot);
                    auto *conditionBlock = llvm::BasicBlock::Create(*context, "procedure_loop_nonwarp_condition", procedureFn);
                    auto *haveStateBlock = llvm::BasicBlock::Create(*context, "procedure_loop_nonwarp_state", procedureFn);
                    auto *checkConditionBlock = llvm::BasicBlock::Create(*context, "procedure_loop_nonwarp_check", procedureFn);
                    auto *enterBlock = llvm::BasicBlock::Create(*context, "procedure_loop_nonwarp_enter", procedureFn);
                    auto *bodyBlock = llvm::BasicBlock::Create(*context, "procedure_loop_nonwarp_body", procedureFn);
                    auto *guardReturnBlock = llvm::BasicBlock::Create(*context, "procedure_loop_nonwarp_guard_return", procedureFn);
                    auto *afterBlock = llvm::BasicBlock::Create(*context, "procedure_loop_nonwarp_after", procedureFn);
                    procBuilder.CreateBr(conditionBlock);

                    procBuilder.SetInsertPoint(conditionBlock);
                    llvm::Value *guard = procBuilder.CreateLoad(i32, guardSlot, "procedure_loop_guard_value");
                    procBuilder.CreateCondBr(
                        procBuilder.CreateICmpSGE(guard, llvm::ConstantInt::get(i32, 1000000)),
                        guardReturnBlock,
                        haveStateBlock);

                    procBuilder.SetInsertPoint(guardReturnBlock);
                    procBuilder.CreateRet(llvm::ConstantInt::get(i32, SJIT_STATUS_YIELDED));

                    procBuilder.SetInsertPoint(haveStateBlock);
                    llvm::Value *state = procBuilder.CreateCall(
                        procedureControlLoopState,
                        {procFrame, statementPtr, procDepth, llvm::ConstantInt::get(i32, 1)},
                        "procedure_loop_state");
                    auto *stateNullBlock = llvm::BasicBlock::Create(*context, "procedure_loop_nonwarp_state_null", procedureFn);
                    auto *stateActiveBlock = llvm::BasicBlock::Create(*context, "procedure_loop_nonwarp_state_active", procedureFn);
                    procBuilder.CreateCondBr(isNull(procBuilder, state), stateNullBlock, stateActiveBlock);

                    procBuilder.SetInsertPoint(stateNullBlock);
                    procBuilder.CreateBr(afterBlock);

                    procBuilder.SetInsertPoint(stateActiveBlock);
                    llvm::Value *active = procBuilder.CreateLoad(i32, procLoopStateBranchActivePtr(state), "procedure_loop_active");
                    procBuilder.CreateCondBr(
                        procBuilder.CreateICmpNE(active, llvm::ConstantInt::get(i32, 0)),
                        bodyBlock,
                        checkConditionBlock);

                    procBuilder.SetInsertPoint(checkConditionBlock);
                    llvm::Value *truthy = emitProcBoolSlot(
                        statement,
                        SJIT_STMT_EXPR_CONDITION,
                        "procedure_loop_condition_value");
                    if (statement->opcode == SJIT_STMT_REPEAT_UNTIL) {
                        procBuilder.CreateCondBr(truthy, afterBlock, enterBlock);
                    } else {
                        procBuilder.CreateCondBr(truthy, enterBlock, afterBlock);
                    }

                    procBuilder.SetInsertPoint(enterBlock);
                    procSetLoopBranchActive(state);
                    procBuilder.CreateBr(bodyBlock);

                    procBuilder.SetInsertPoint(bodyBlock);
                    llvm::Value *nextGuard = procBuilder.CreateAdd(
                        guard,
                        llvm::ConstantInt::get(i32, 1),
                        "procedure_loop_next_guard");
                    procBuilder.CreateStore(nextGuard, guardSlot);
                    emitProcStatefulStatements(statement->substack, statement->substack_count, state, "procedure_loop_nonwarp_sub");
                    if (!procBuilder.GetInsertBlock()->getTerminator()) {
                        llvm::Value *status = procFinishLoopBranch(
                            state,
                            nextGuard,
                            statement->opcode != SJIT_STMT_REPEAT_UNTIL,
                            "procedure_loop_status");
                        auto *returnStatus = llvm::BasicBlock::Create(*context, "procedure_loop_nonwarp_return", procedureFn);
                        auto *continueLoop = llvm::BasicBlock::Create(*context, "procedure_loop_nonwarp_continue", procedureFn);
                        procBuilder.CreateCondBr(
                            procBuilder.CreateICmpNE(status, llvm::ConstantInt::get(i32, SJIT_STATUS_OK)),
                            returnStatus,
                            continueLoop);

                        procBuilder.SetInsertPoint(returnStatus);
                        procBuilder.CreateRet(status);

                        procBuilder.SetInsertPoint(continueLoop);
                        procBuilder.CreateBr(conditionBlock);
                    }

                    procBuilder.SetInsertPoint(afterBlock);
                    procBuilder.CreateCall(procedureControlLoopReset, {procFrame, statementPtr, procDepth});
                    break;
                }
                llvm::AllocaInst *guardSlot = createProcAlloca(i32, nullptr, "procedure_loop_guard");
                procBuilder.CreateStore(llvm::ConstantInt::get(i32, 0), guardSlot);
                auto *conditionBlock = llvm::BasicBlock::Create(*context, "procedure_loop_condition", procedureFn);
                auto *bodyBlock = llvm::BasicBlock::Create(*context, "procedure_loop_body", procedureFn);
                auto *guardReturnBlock = llvm::BasicBlock::Create(*context, "procedure_loop_guard_return", procedureFn);
                auto *afterBlock = llvm::BasicBlock::Create(*context, "procedure_loop_after", procedureFn);
                procBuilder.CreateBr(conditionBlock);

                procBuilder.SetInsertPoint(conditionBlock);
                llvm::Value *guard = procBuilder.CreateLoad(i32, guardSlot, "procedure_loop_guard_value");
                procBuilder.CreateCondBr(
                    procBuilder.CreateICmpSGE(guard, llvm::ConstantInt::get(i32, 1000000)),
                    guardReturnBlock,
                    bodyBlock);

                procBuilder.SetInsertPoint(guardReturnBlock);
                procBuilder.CreateRet(llvm::ConstantInt::get(i32, SJIT_STATUS_YIELDED));

                procBuilder.SetInsertPoint(bodyBlock);
                llvm::Value *truthy = emitProcBoolSlot(
                    statement,
                    SJIT_STMT_EXPR_CONDITION,
                    "procedure_loop_condition_value");
                auto *enterBlock = llvm::BasicBlock::Create(*context, "procedure_loop_enter", procedureFn);
                if (statement->opcode == SJIT_STMT_REPEAT_UNTIL) {
                    procBuilder.CreateCondBr(truthy, afterBlock, enterBlock);
                } else {
                    procBuilder.CreateCondBr(truthy, enterBlock, afterBlock);
                }

                procBuilder.SetInsertPoint(enterBlock);
                procBuilder.CreateStore(
                    procBuilder.CreateAdd(guard, llvm::ConstantInt::get(i32, 1), "procedure_loop_next_guard"),
                    guardSlot);
                emitProcStatements(statement->substack, statement->substack_count);
                if (!procBuilder.GetInsertBlock()->getTerminator()) {
                    procBuilder.CreateBr(conditionBlock);
                }

                procBuilder.SetInsertPoint(afterBlock);
                break;
            }
            case SJIT_STMT_FOREVER: {
                if (!procedure.warp_mode) {
                    if (!canInlineNonWarpLoopSubstack(statement)) {
                        procBuilder.CreateRet(llvm::ConstantInt::get(i32, SJIT_STATUS_ERROR));
                        break;
                    }
                    llvm::AllocaInst *guardSlot = createProcAlloca(i32, nullptr, "procedure_forever_nonwarp_guard");
                    procBuilder.CreateStore(llvm::ConstantInt::get(i32, 0), guardSlot);
                    auto *conditionBlock = llvm::BasicBlock::Create(*context, "procedure_forever_nonwarp_condition", procedureFn);
                    auto *haveStateBlock = llvm::BasicBlock::Create(*context, "procedure_forever_nonwarp_state", procedureFn);
                    auto *activateBlock = llvm::BasicBlock::Create(*context, "procedure_forever_nonwarp_activate", procedureFn);
                    auto *bodyBlock = llvm::BasicBlock::Create(*context, "procedure_forever_nonwarp_body", procedureFn);
                    auto *guardReturnBlock = llvm::BasicBlock::Create(*context, "procedure_forever_nonwarp_guard_return", procedureFn);
                    procBuilder.CreateBr(conditionBlock);

                    procBuilder.SetInsertPoint(conditionBlock);
                    llvm::Value *guard = procBuilder.CreateLoad(i32, guardSlot, "procedure_forever_guard_value");
                    procBuilder.CreateCondBr(
                        procBuilder.CreateICmpSGE(guard, llvm::ConstantInt::get(i32, 1000000)),
                        guardReturnBlock,
                        haveStateBlock);

                    procBuilder.SetInsertPoint(guardReturnBlock);
                    procBuilder.CreateRet(llvm::ConstantInt::get(i32, SJIT_STATUS_YIELDED));

                    procBuilder.SetInsertPoint(haveStateBlock);
                    llvm::Value *state = procBuilder.CreateCall(
                        procedureControlLoopState,
                        {procFrame, statementPtr, procDepth, llvm::ConstantInt::get(i32, 1)},
                        "procedure_forever_state");
                    auto *stateNullBlock = llvm::BasicBlock::Create(*context, "procedure_forever_nonwarp_state_null", procedureFn);
                    auto *stateActiveBlock = llvm::BasicBlock::Create(*context, "procedure_forever_nonwarp_state_active", procedureFn);
                    procBuilder.CreateCondBr(isNull(procBuilder, state), stateNullBlock, stateActiveBlock);

                    procBuilder.SetInsertPoint(stateNullBlock);
                    procBuilder.CreateRet(llvm::ConstantInt::get(i32, SJIT_STATUS_YIELDED));

                    procBuilder.SetInsertPoint(stateActiveBlock);
                    llvm::Value *active = procBuilder.CreateLoad(i32, procLoopStateBranchActivePtr(state), "procedure_forever_active");
                    procBuilder.CreateCondBr(
                        procBuilder.CreateICmpNE(active, llvm::ConstantInt::get(i32, 0)),
                        bodyBlock,
                        activateBlock);

                    procBuilder.SetInsertPoint(activateBlock);
                    procSetLoopBranchActive(state);
                    procBuilder.CreateBr(bodyBlock);

                    procBuilder.SetInsertPoint(bodyBlock);
                    llvm::Value *nextGuard = procBuilder.CreateAdd(
                        guard,
                        llvm::ConstantInt::get(i32, 1),
                        "procedure_forever_next_guard");
                    procBuilder.CreateStore(nextGuard, guardSlot);
                    emitProcStatefulStatements(statement->substack, statement->substack_count, state, "procedure_forever_nonwarp_sub");
                    if (!procBuilder.GetInsertBlock()->getTerminator()) {
                        llvm::Value *status = procFinishLoopBranch(
                            state,
                            nextGuard,
                            false,
                            "procedure_forever_status");
                        auto *returnStatus = llvm::BasicBlock::Create(*context, "procedure_forever_nonwarp_return", procedureFn);
                        auto *continueLoop = llvm::BasicBlock::Create(*context, "procedure_forever_nonwarp_continue", procedureFn);
                        procBuilder.CreateCondBr(
                            procBuilder.CreateICmpNE(status, llvm::ConstantInt::get(i32, SJIT_STATUS_OK)),
                            returnStatus,
                            continueLoop);

                        procBuilder.SetInsertPoint(returnStatus);
                        procBuilder.CreateRet(status);

                        procBuilder.SetInsertPoint(continueLoop);
                        procBuilder.CreateBr(conditionBlock);
                    }
                    break;
                }
                llvm::AllocaInst *guardSlot = createProcAlloca(i32, nullptr, "procedure_forever_guard");
                procBuilder.CreateStore(llvm::ConstantInt::get(i32, 0), guardSlot);
                auto *conditionBlock = llvm::BasicBlock::Create(*context, "procedure_forever_condition", procedureFn);
                auto *bodyBlock = llvm::BasicBlock::Create(*context, "procedure_forever_body", procedureFn);
                auto *guardReturnBlock = llvm::BasicBlock::Create(*context, "procedure_forever_guard_return", procedureFn);
                procBuilder.CreateBr(conditionBlock);

                procBuilder.SetInsertPoint(conditionBlock);
                llvm::Value *guard = procBuilder.CreateLoad(i32, guardSlot, "procedure_forever_guard_value");
                procBuilder.CreateCondBr(
                    procBuilder.CreateICmpSGE(guard, llvm::ConstantInt::get(i32, 1000000)),
                    guardReturnBlock,
                    bodyBlock);

                procBuilder.SetInsertPoint(guardReturnBlock);
                procBuilder.CreateRet(llvm::ConstantInt::get(i32, SJIT_STATUS_YIELDED));

                procBuilder.SetInsertPoint(bodyBlock);
                procBuilder.CreateStore(
                    procBuilder.CreateAdd(guard, llvm::ConstantInt::get(i32, 1), "procedure_forever_next_guard"),
                    guardSlot);
                emitProcStatements(statement->substack, statement->substack_count);
                if (!procBuilder.GetInsertBlock()->getTerminator()) {
                    procBuilder.CreateBr(conditionBlock);
                }
                break;
            }
            case SJIT_STMT_FOR_EACH: {
                if (!procedure.warp_mode) {
                    if (!canInlineNonWarpLoopSubstack(statement)) {
                        procBuilder.CreateRet(llvm::ConstantInt::get(i32, SJIT_STATUS_ERROR));
                        break;
                    }
                    auto *conditionBlock = llvm::BasicBlock::Create(*context, "procedure_for_each_nonwarp_condition", procedureFn);
                    auto *prepareBlock = llvm::BasicBlock::Create(*context, "procedure_for_each_nonwarp_prepare", procedureFn);
                    auto *enterBlock = llvm::BasicBlock::Create(*context, "procedure_for_each_nonwarp_enter", procedureFn);
                    auto *bodyBlock = llvm::BasicBlock::Create(*context, "procedure_for_each_nonwarp_body", procedureFn);
                    auto *afterBlock = llvm::BasicBlock::Create(*context, "procedure_for_each_nonwarp_after", procedureFn);
                    procBuilder.CreateBr(conditionBlock);

                    procBuilder.SetInsertPoint(conditionBlock);
                    llvm::Value *state = procBuilder.CreateCall(
                        procedureControlLoopState,
                        {procFrame, statementPtr, procDepth, llvm::ConstantInt::get(i32, 1)},
                        "procedure_for_each_state");
                    auto *stateNullBlock = llvm::BasicBlock::Create(*context, "procedure_for_each_nonwarp_state_null", procedureFn);
                    auto *stateActiveBlock = llvm::BasicBlock::Create(*context, "procedure_for_each_nonwarp_state_active", procedureFn);
                    procBuilder.CreateCondBr(isNull(procBuilder, state), stateNullBlock, stateActiveBlock);

                    procBuilder.SetInsertPoint(stateNullBlock);
                    procBuilder.CreateBr(afterBlock);

                    procBuilder.SetInsertPoint(stateActiveBlock);
                    llvm::Value *active = procBuilder.CreateLoad(i32, procLoopStateBranchActivePtr(state), "procedure_for_each_active");
                    procBuilder.CreateCondBr(
                        procBuilder.CreateICmpNE(active, llvm::ConstantInt::get(i32, 0)),
                        bodyBlock,
                        prepareBlock);

                    procBuilder.SetInsertPoint(prepareBlock);
                    llvm::Value *limit = emitProcNumberSlot(statement, SJIT_STMT_EXPR_TIMES, "procedure_for_each_limit_value");
                    llvm::Value *counterPtr = procLoopStateCounterPtr(state);
                    llvm::Value *counter = procBuilder.CreateLoad(f64, counterPtr, "procedure_for_each_counter");
                    auto *doneBlock = llvm::BasicBlock::Create(*context, "procedure_for_each_nonwarp_done", procedureFn);
                    procBuilder.CreateCondBr(
                        procBuilder.CreateFCmpOGE(counter, limit, "procedure_for_each_done"),
                        doneBlock,
                        enterBlock);

                    procBuilder.SetInsertPoint(enterBlock);
                    llvm::Value *nextCounter = procBuilder.CreateFAdd(
                        counter,
                        llvm::ConstantFP::get(f64, 1.0),
                        "procedure_for_each_next_counter");
                    procBuilder.CreateStore(nextCounter, counterPtr);
                    procBuilder.CreateCall(statementSetVariableNumber, {procRuntime, procScriptData, statementPtr, nextCounter});
                    procSetLoopBranchActive(state);
                    procBuilder.CreateBr(bodyBlock);

                    procBuilder.SetInsertPoint(bodyBlock);
                    emitProcStatefulStatements(statement->substack, statement->substack_count, state, "procedure_for_each_nonwarp_sub");
                    if (!procBuilder.GetInsertBlock()->getTerminator()) {
                        llvm::Value *status = procFinishLoopBranch(
                            state,
                            llvm::ConstantInt::get(i32, 0),
                            false,
                            "procedure_for_each_status");
                        auto *returnStatus = llvm::BasicBlock::Create(*context, "procedure_for_each_nonwarp_return", procedureFn);
                        auto *continueLoop = llvm::BasicBlock::Create(*context, "procedure_for_each_nonwarp_continue", procedureFn);
                        procBuilder.CreateCondBr(
                            procBuilder.CreateICmpNE(status, llvm::ConstantInt::get(i32, SJIT_STATUS_OK)),
                            returnStatus,
                            continueLoop);

                        procBuilder.SetInsertPoint(returnStatus);
                        procBuilder.CreateRet(status);

                        procBuilder.SetInsertPoint(continueLoop);
                        procBuilder.CreateBr(conditionBlock);
                    }

                    procBuilder.SetInsertPoint(doneBlock);
                    procBuilder.CreateCall(procedureControlLoopReset, {procFrame, statementPtr, procDepth});
                    procBuilder.CreateBr(afterBlock);

                    procBuilder.SetInsertPoint(afterBlock);
                    break;
                }
                llvm::AllocaInst *counterSlot = createProcAlloca(f64, nullptr, "procedure_for_each_counter");
                llvm::AllocaInst *limitSlot = createProcAlloca(f64, nullptr, "procedure_for_each_limit");
                llvm::AllocaInst *guardSlot = createProcAlloca(i32, nullptr, "procedure_for_each_guard");
                procBuilder.CreateStore(llvm::ConstantFP::get(f64, 0.0), counterSlot);
                procBuilder.CreateStore(
                    emitProcNumberSlot(statement, SJIT_STMT_EXPR_TIMES, "procedure_for_each_limit_value"),
                    limitSlot);
                procBuilder.CreateStore(llvm::ConstantInt::get(i32, 0), guardSlot);

                auto *conditionBlock = llvm::BasicBlock::Create(*context, "procedure_for_each_condition", procedureFn);
                auto *bodyBlock = llvm::BasicBlock::Create(*context, "procedure_for_each_body", procedureFn);
                auto *guardReturnBlock = llvm::BasicBlock::Create(*context, "procedure_for_each_guard_return", procedureFn);
                auto *afterBlock = llvm::BasicBlock::Create(*context, "procedure_for_each_after", procedureFn);
                procBuilder.CreateBr(conditionBlock);

                procBuilder.SetInsertPoint(conditionBlock);
                llvm::Value *guard = procBuilder.CreateLoad(i32, guardSlot, "procedure_for_each_guard_value");
                auto *limitCheckBlock = llvm::BasicBlock::Create(*context, "procedure_for_each_limit_check", procedureFn);
                procBuilder.CreateCondBr(
                    procBuilder.CreateICmpSGE(guard, llvm::ConstantInt::get(i32, 1000000)),
                    guardReturnBlock,
                    limitCheckBlock);

                procBuilder.SetInsertPoint(guardReturnBlock);
                procBuilder.CreateRet(llvm::ConstantInt::get(i32, SJIT_STATUS_YIELDED));

                procBuilder.SetInsertPoint(limitCheckBlock);
                llvm::Value *counter = procBuilder.CreateLoad(f64, counterSlot, "procedure_for_each_counter_value");
                llvm::Value *limit = procBuilder.CreateLoad(f64, limitSlot, "procedure_for_each_limit");
                procBuilder.CreateCondBr(
                    procBuilder.CreateFCmpOLT(counter, limit, "procedure_for_each_has_next"),
                    bodyBlock,
                    afterBlock);

                procBuilder.SetInsertPoint(bodyBlock);
                llvm::Value *nextCounter = procBuilder.CreateFAdd(
                    counter,
                    llvm::ConstantFP::get(f64, 1.0),
                    "procedure_for_each_next_counter");
                procBuilder.CreateStore(nextCounter, counterSlot);
                procBuilder.CreateStore(
                    procBuilder.CreateAdd(guard, llvm::ConstantInt::get(i32, 1), "procedure_for_each_next_guard"),
                    guardSlot);
                procBuilder.CreateCall(statementSetVariableNumber, {procRuntime, procScriptData, statementPtr, nextCounter});
                emitProcStatements(statement->substack, statement->substack_count);
                if (!procBuilder.GetInsertBlock()->getTerminator()) {
                    procBuilder.CreateBr(conditionBlock);
                }

                procBuilder.SetInsertPoint(afterBlock);
                break;
            }
            case SJIT_STMT_PEN_CLEAR:
                procBuilder.CreateCall(penClear, {procRuntime});
                break;
            case SJIT_STMT_PEN_DOWN:
            case SJIT_STMT_PEN_UP:
            case SJIT_STMT_LOOKS_SHOW:
            case SJIT_STMT_LOOKS_HIDE: {
                if (statement->opcode == SJIT_STMT_PEN_DOWN) {
                    procBuilder.CreateCall(penDown, {procRuntime, procTarget});
                } else if (statement->opcode == SJIT_STMT_PEN_UP) {
                    procBuilder.CreateCall(penUp, {procRuntime, procTarget});
                } else if (statement->opcode == SJIT_STMT_LOOKS_SHOW) {
                    procBuilder.CreateCall(looksShow, {procRuntime, procTarget});
                } else {
                    procBuilder.CreateCall(looksHide, {procRuntime, procTarget});
                }
                break;
            }
            case SJIT_STMT_MONITOR_SHOW:
            case SJIT_STMT_MONITOR_HIDE:
                procBuilder.CreateCall(
                    statementMonitorVisibility,
                    {procRuntime, procScriptData, statementPtr});
                break;
            case SJIT_STMT_PEN_SET_SIZE: {
                llvm::Value *size = emitProcNumberSlot(statement, SJIT_STMT_EXPR_VALUE, "procedure_pen_size");
                procBuilder.CreateCall(penSetSizeNumber, {procRuntime, procTarget, size});
                break;
            }
            case SJIT_STMT_MOTION_GOTO_XY: {
                llvm::Value *x = emitProcNumberSlot(statement, SJIT_STMT_EXPR_VALUE, "procedure_motion_x");
                llvm::Value *y = emitProcNumberSlot(statement, SJIT_STMT_EXPR_INDEX, "procedure_motion_y");
                emitProcSetXy(x, y);
                break;
            }
            case SJIT_STMT_MOTION_SET_X: {
                llvm::Value *x = emitProcNumberSlot(statement, SJIT_STMT_EXPR_VALUE, "procedure_motion_x");
                llvm::Value *y = emitProcSpriteCoordinate(4, spriteY, "procedure_current_y");
                emitProcSetXy(x, y);
                break;
            }
            case SJIT_STMT_MOTION_SET_Y: {
                llvm::Value *x = emitProcSpriteCoordinate(3, spriteX, "procedure_current_x");
                llvm::Value *y = emitProcNumberSlot(statement, SJIT_STMT_EXPR_VALUE, "procedure_motion_y");
                emitProcSetXy(x, y);
                break;
            }
            case SJIT_STMT_MOTION_CHANGE_X: {
                llvm::Value *currentX = emitProcSpriteCoordinate(3, spriteX, "procedure_current_x");
                llvm::Value *dx = emitProcNumberSlot(statement, SJIT_STMT_EXPR_VALUE, "procedure_motion_dx");
                llvm::Value *x = procBuilder.CreateFAdd(currentX, dx, "procedure_motion_next_x");
                llvm::Value *y = emitProcSpriteCoordinate(4, spriteY, "procedure_current_y");
                emitProcSetXy(x, y);
                break;
            }
            case SJIT_STMT_MOTION_CHANGE_Y: {
                llvm::Value *x = emitProcSpriteCoordinate(3, spriteX, "procedure_current_x");
                llvm::Value *currentY = emitProcSpriteCoordinate(4, spriteY, "procedure_current_y");
                llvm::Value *dy = emitProcNumberSlot(statement, SJIT_STMT_EXPR_VALUE, "procedure_motion_dy");
                llvm::Value *y = procBuilder.CreateFAdd(currentY, dy, "procedure_motion_next_y");
                emitProcSetXy(x, y);
                break;
            }
            case SJIT_STMT_LOOKS_SET_SIZE: {
                llvm::Value *size = emitProcNumberSlot(statement, SJIT_STMT_EXPR_VALUE, "procedure_looks_size");
                procBuilder.CreateCall(spriteSetSize, {procRuntime, procTarget, size});
                break;
            }
            case SJIT_STMT_LOOKS_SET_EFFECT:
            case SJIT_STMT_LOOKS_CHANGE_EFFECT: {
                llvm::Value *value = emitProcNumberSlot(
                    statement,
                    SJIT_STMT_EXPR_VALUE,
                    "procedure_looks_effect_value");
                procBuilder.CreateCall(
                    statement->opcode == SJIT_STMT_LOOKS_SET_EFFECT ?
                        looksSetEffectNumber : looksChangeEffectNumber,
                    {
                        procRuntime,
                        procTarget,
                        llvm::ConstantInt::get(
                            i32,
                            statement->looks_effect_cache_valid ?
                                statement->looks_effect : -1),
                        value,
                    });
                break;
            }
            case SJIT_STMT_LOOKS_CLEAR_EFFECTS:
                procBuilder.CreateCall(looksClearEffects, {procRuntime, procTarget});
                break;
            case SJIT_STMT_SENSING_SET_DRAG_MODE:
                procBuilder.CreateCall(
                    spriteSetDraggable,
                    {procTarget, llvm::ConstantInt::get(i32, statement->drag_mode ? 1 : 0)});
                break;
            case SJIT_STMT_SAY: {
                llvm::AllocaInst *valueSlot = emitProcValueSlot(
                    statement,
                    SJIT_STMT_EXPR_VALUE,
                    "procedure_say_value_slot");
                procBuilder.CreateCall(statementSayValuePtr, {procRuntime, valueSlot});
                destroyProcValueSlot(valueSlot);
                break;
            }
            case SJIT_STMT_PEN_SET_COLOR: {
                llvm::AllocaInst *valueSlot = emitProcValueSlot(
                    statement,
                    SJIT_STMT_EXPR_VALUE,
                    "procedure_pen_color_value_slot");
                procBuilder.CreateCall(statementPenSetColorValuePtr, {procRuntime, procScriptData, valueSlot});
                destroyProcValueSlot(valueSlot);
                break;
            }
            case SJIT_STMT_PEN_CHANGE_COLOR_PARAM: {
                llvm::AllocaInst *paramSlot = emitProcValueSlot(
                    statement,
                    SJIT_STMT_EXPR_INDEX,
                    "procedure_pen_color_param_slot");
                llvm::AllocaInst *valueSlot = emitProcValueSlot(
                    statement,
                    SJIT_STMT_EXPR_VALUE,
                    "procedure_pen_color_delta_slot");
                procBuilder.CreateCall(
                    statementPenChangeColorParamValuePtr,
                    {procRuntime, procScriptData, paramSlot, valueSlot});
                destroyProcValueSlot(paramSlot);
                destroyProcValueSlot(valueSlot);
                break;
            }
            default:
                procBuilder.CreateRet(llvm::ConstantInt::get(i32, SJIT_STATUS_ERROR));
                break;
            }
        };
        emitProcStatements = [&](const SStatement *statements, int count) {
            for (int i = 0; i < count; ++i) {
                if (procBuilder.GetInsertBlock()->getTerminator()) {
                    return;
                }
                if (fastVariableAccess && i + 4 < count &&
                    statements[i].opcode == SJIT_STMT_PEN_SET_COLOR &&
                    statements[i + 1].opcode == SJIT_STMT_PEN_CHANGE_COLOR_PARAM &&
                    statements[i + 2].opcode == SJIT_STMT_LIST_REPLACE &&
                    statements[i + 3].opcode == SJIT_STMT_PEN_DOWN &&
                    statements[i + 4].opcode == SJIT_STMT_PEN_UP) {
                    const SExpr *colorExpr = statements[i].value;
                    const SExpr *brightnessExpr = statements[i + 1].value;
                    const SExpr *colorIndex = colorExpr && colorExpr->opcode == SJIT_EXPR_LIST_ITEM ?
                        colorExpr->left : nullptr;
                    const SExpr *brightnessIndex = brightnessExpr && brightnessExpr->opcode == SJIT_EXPR_LIST_ITEM ?
                        brightnessExpr->left : nullptr;
                    const SExpr *replaceIndex = statements[i + 2].index;
                    const bool sameIndexVariable =
                        colorIndex && colorIndex->opcode == SJIT_EXPR_VARIABLE &&
                        colorIndex->literal.tag == SJIT_VALUE_STRING &&
                        brightnessIndex && brightnessIndex->opcode == SJIT_EXPR_VARIABLE &&
                        brightnessIndex->literal.tag == SJIT_VALUE_STRING &&
                        replaceIndex && replaceIndex->opcode == SJIT_EXPR_VARIABLE &&
                        replaceIndex->literal.tag == SJIT_VALUE_STRING &&
                        stringEquals(
                            static_cast<const SString *>(colorIndex->literal.ptr),
                            static_cast<const SString *>(brightnessIndex->literal.ptr)) &&
                        stringEquals(
                            static_cast<const SString *>(colorIndex->literal.ptr),
                            static_cast<const SString *>(replaceIndex->literal.ptr));
                    const bool replacesColorList =
                        colorExpr && colorExpr->literal.tag == SJIT_VALUE_STRING &&
                        statements[i + 2].variable_name &&
                        stringEquals(
                            static_cast<const SString *>(colorExpr->literal.ptr),
                            statements[i + 2].variable_name);
                    const bool literalReplacement =
                        statements[i + 2].value &&
                        statements[i + 2].value->opcode == SJIT_EXPR_LITERAL;
                    const SExpr *paramExpr = statements[i + 1].index;
                    const int paramId = paramExpr && paramExpr->opcode == SJIT_EXPR_LITERAL &&
                        paramExpr->literal.tag == SJIT_VALUE_STRING ?
                        sjit_pen_color_param_id(sjit_string_cstr(
                            static_cast<const SString *>(paramExpr->literal.ptr))) : 0;
                    if (sameIndexVariable && replacesColorList && literalReplacement && paramId != 0) {
                        procBuilder.CreateCall(
                            penRenderListPixelFromVariables,
                            {
                                procRuntime,
                                procTarget,
                                emitProcListVariable(colorExpr),
                                emitProcListVariable(brightnessExpr),
                                emitProcScalarVariable(colorIndex),
                                constantPointer(statements[i + 2].value),
                                llvm::ConstantInt::get(i32, paramId),
                            });
                        i += 4;
                        continue;
                    }
                }
                if (i + 1 < count &&
                    statements[i].opcode == SJIT_STMT_PEN_DOWN &&
                    statements[i + 1].opcode == SJIT_STMT_PEN_UP) {
                    procBuilder.CreateCall(penStamp, {procRuntime, procTarget});
                    ++i;
                    continue;
                }
                if (i + 1 < count &&
                    statements[i].opcode == SJIT_STMT_PEN_SET_COLOR &&
                    statements[i + 1].opcode == SJIT_STMT_PEN_CHANGE_COLOR_PARAM &&
                    statements[i + 1].index &&
                    statements[i + 1].index->opcode == SJIT_EXPR_LITERAL &&
                    statements[i + 1].index->literal.tag == SJIT_VALUE_STRING &&
                    procedureSupportsNumberExpr(procedure, statements[i + 1].value)) {
                    const int paramId = sjit_pen_color_param_id(sjit_string_cstr(
                        static_cast<const SString *>(statements[i + 1].index->literal.ptr)));
                    if (paramId != 0) {
                        llvm::Value *delta = emitProcNumberSlot(
                            &statements[i + 1],
                            SJIT_STMT_EXPR_VALUE,
                            "procedure_pen_color_pair_delta");
                        const SExpr *colorExpr = statements[i].value;
                        if (fastVariableAccess && colorExpr && colorExpr->opcode == SJIT_EXPR_LIST_ITEM &&
                            colorExpr->left && colorExpr->left->opcode == SJIT_EXPR_VARIABLE) {
                            procBuilder.CreateCall(
                                penColorListChangeAtVariable,
                                {
                                    procRuntime,
                                    procScriptData,
                                    constantPointer(colorExpr),
                                    emitProcScalarVariable(colorExpr->left),
                                    llvm::ConstantInt::get(i32, paramId),
                                    delta,
                                });
                        } else {
                            llvm::AllocaInst *colorSlot = emitProcValueSlot(
                                &statements[i],
                                SJIT_STMT_EXPR_VALUE,
                                "procedure_pen_color_pair_value_slot");
                            procBuilder.CreateCall(
                                penColorChangePair,
                                {
                                    procRuntime,
                                    procScriptData,
                                    colorSlot,
                                    llvm::ConstantInt::get(i32, paramId),
                                    delta,
                                });
                            destroyProcValueSlot(colorSlot);
                        }
                        ++i;
                        continue;
                    }
                }
                emitProcStatement(&statements[i]);
            }
        };
        emitProcStatefulStatements = [&](const SStatement *statements, int count, llvm::Value *state, const std::string &name) {
            if (!statements || count <= 0 || procBuilder.GetInsertBlock()->getTerminator()) {
                return;
            }
            llvm::Value *subPcPtr = procLoopStateSubPcPtr(state);
            std::vector<llvm::BasicBlock *> checkBlocks;
            std::vector<llvm::BasicBlock *> executeBlocks;
            checkBlocks.reserve(static_cast<size_t>(count));
            executeBlocks.reserve(static_cast<size_t>(count));
            for (int i = 0; i < count; ++i) {
                checkBlocks.push_back(llvm::BasicBlock::Create(
                    *context,
                    name + "_" + std::to_string(i) + "_check",
                    procedureFn));
                executeBlocks.push_back(llvm::BasicBlock::Create(
                    *context,
                    name + "_" + std::to_string(i),
                    procedureFn));
            }
            auto *afterBlock = llvm::BasicBlock::Create(*context, name + "_after", procedureFn);
            procBuilder.CreateBr(checkBlocks[0]);
            for (int i = 0; i < count; ++i) {
                llvm::BasicBlock *nextBlock = i + 1 < count ? checkBlocks[static_cast<size_t>(i + 1)] : afterBlock;
                procBuilder.SetInsertPoint(checkBlocks[static_cast<size_t>(i)]);
                llvm::Value *resumePc = procBuilder.CreateLoad(i32, subPcPtr, name + "_resume_pc");
                procBuilder.CreateCondBr(
                    procBuilder.CreateICmpSGT(resumePc, llvm::ConstantInt::get(i32, i), name + "_statement_done"),
                    nextBlock,
                    executeBlocks[static_cast<size_t>(i)]);

                procBuilder.SetInsertPoint(executeBlocks[static_cast<size_t>(i)]);
                procBuilder.CreateStore(llvm::ConstantInt::get(i32, i), subPcPtr);
                emitProcStatement(&statements[i]);
                if (!procBuilder.GetInsertBlock()->getTerminator()) {
                    procBuilder.CreateBr(nextBlock);
                }
            }
            procBuilder.SetInsertPoint(afterBlock);
        };

        auto emitProcTopLevelStatements = [&]() {
            llvm::Value *activationState = procBuilder.CreateCall(
                procedureActivationState,
                {procFrame, procedurePtr, procDepth, llvm::ConstantInt::get(i32, 1)},
                "procedure_activation_state");
            auto *activationError = llvm::BasicBlock::Create(*context, "procedure_activation_error", procedureFn);
            auto *afterStatements = llvm::BasicBlock::Create(*context, "procedure_after_statements", procedureFn);
            std::vector<llvm::BasicBlock *> checkBlocks;
            std::vector<llvm::BasicBlock *> executeBlocks;
            checkBlocks.reserve(static_cast<size_t>(procedure.statement_count));
            executeBlocks.reserve(static_cast<size_t>(procedure.statement_count));
            for (int i = 0; i < procedure.statement_count; ++i) {
                checkBlocks.push_back(llvm::BasicBlock::Create(
                    *context,
                    "procedure_statement_" + std::to_string(i) + "_check",
                    procedureFn));
                executeBlocks.push_back(llvm::BasicBlock::Create(
                    *context,
                    "procedure_statement_" + std::to_string(i),
                    procedureFn));
            }

            llvm::BasicBlock *firstBlock = procedure.statement_count > 0 ? checkBlocks[0] : afterStatements;
            llvm::Value *activationPcPtr = procLoopStateSubPcPtr(activationState);
            procBuilder.CreateCondBr(isNull(procBuilder, activationState), activationError, firstBlock);

            procBuilder.SetInsertPoint(activationError);
            procBuilder.CreateRet(llvm::ConstantInt::get(i32, SJIT_STATUS_ERROR));

            for (int i = 0; i < procedure.statement_count; ++i) {
                llvm::BasicBlock *nextBlock = i + 1 < procedure.statement_count ? checkBlocks[static_cast<size_t>(i + 1)] : afterStatements;
                procBuilder.SetInsertPoint(checkBlocks[static_cast<size_t>(i)]);
                llvm::Value *resumePc = procBuilder.CreateLoad(i32, activationPcPtr, "procedure_resume_pc");
                procBuilder.CreateCondBr(
                    procBuilder.CreateICmpSGT(resumePc, llvm::ConstantInt::get(i32, i), "procedure_statement_done"),
                    nextBlock,
                    executeBlocks[static_cast<size_t>(i)]);

                procBuilder.SetInsertPoint(executeBlocks[static_cast<size_t>(i)]);
                procBuilder.CreateStore(llvm::ConstantInt::get(i32, i), activationPcPtr);
                emitProcStatement(&procedure.statements[i]);
                if (!procBuilder.GetInsertBlock()->getTerminator()) {
                    procBuilder.CreateBr(nextBlock);
                }
            }

            procBuilder.SetInsertPoint(afterStatements);
            procBuilder.CreateCall(procedureActivationReset, {procFrame, procedurePtr, procDepth});
        };

        if (statelessWarpProcedure) {
            emitProcStatements(procedure.statements, procedure.statement_count);
        } else {
            emitProcTopLevelStatements();
        }
        if (!procBuilder.GetInsertBlock()->getTerminator()) {
            procBuilder.CreateRet(llvm::ConstantInt::get(i32, SJIT_STATUS_OK));
        }
    };
    struct NumberLowering {
        llvm::Value *value;
        llvm::Value *ok;
        bool supported;
    };
    struct BoolLowering {
        llvm::Value *value;
        llvm::Value *ok;
        bool supported;
    };
    auto supportedNumber = [&](llvm::Value *value, llvm::Value *ok) -> NumberLowering {
        return {value, ok, true};
    };
    auto unsupportedNumber = [&]() -> NumberLowering {
        return {nullptr, nullptr, false};
    };
    auto supportedBool = [&](llvm::Value *value, llvm::Value *ok) -> BoolLowering {
        return {value, ok, true};
    };
    auto unsupportedBool = [&]() -> BoolLowering {
        return {nullptr, nullptr, false};
    };
    std::function<NumberLowering(const SExpr *)> emitNumberExpr;
    std::function<BoolLowering(const SExpr *)> emitBoolExpr;
    std::function<void(const SExpr *, llvm::Value *)> emitValueInto;
    auto storeScalarValue = [&](llvm::Value *valueSlot, int tag, llvm::Value *number) {
        builder.CreateStore(
            llvm::ConstantInt::get(i32, tag),
            builder.CreateStructGEP(svalueTy, valueSlot, 0));
        builder.CreateStore(
            number,
            builder.CreateStructGEP(svalueTy, valueSlot, 1));
        builder.CreateStore(
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr)),
            builder.CreateStructGEP(svalueTy, valueSlot, 2));
    };
    auto destroyValueSlot = [&](llvm::Value *valueSlot) {
        llvm::Value *tag = builder.CreateLoad(
            i32,
            builder.CreateStructGEP(svalueTy, valueSlot, 0, "destroy_value_tag_ptr"),
            "destroy_value_tag");
        llvm::Value *ownsStorage = builder.CreateOr(
            builder.CreateICmpEQ(tag, llvm::ConstantInt::get(i32, SJIT_VALUE_STRING)),
            builder.CreateICmpEQ(tag, llvm::ConstantInt::get(i32, SJIT_VALUE_LIST)),
            "destroy_value_owned");
        auto *destroyBlock = llvm::BasicBlock::Create(*context, "destroy_value", fn);
        auto *continueBlock = llvm::BasicBlock::Create(*context, "destroy_value_done", fn);
        builder.CreateCondBr(ownsStorage, destroyBlock, continueBlock);

        builder.SetInsertPoint(destroyBlock);
        builder.CreateCall(valueDestroyPtr, {valueSlot});
        builder.CreateBr(continueBlock);

        builder.SetInsertPoint(continueBlock);
    };
    auto emitValueExprToNumber = [&](const SExpr *expr, const llvm::Twine &name) -> llvm::Value * {
        llvm::AllocaInst *valueSlot = entryBuilder.CreateAlloca(svalueTy, nullptr, name + "_value_slot");
        emitValueInto(expr, valueSlot);
        llvm::Value *number = builder.CreateCall(valuePtrToNumber, {runtime, valueSlot}, name);
        destroyValueSlot(valueSlot);
        return number;
    };
    auto emitValueExprTruthy = [&](const SExpr *expr, const llvm::Twine &name) -> llvm::Value * {
        llvm::AllocaInst *valueSlot = entryBuilder.CreateAlloca(svalueTy, nullptr, name + "_value_slot");
        emitValueInto(expr, valueSlot);
        llvm::Value *truthy = builder.CreateCall(valueTruthy, {runtime, valueSlot}, name + "_raw");
        destroyValueSlot(valueSlot);
        return builder.CreateICmpNE(truthy, llvm::ConstantInt::get(i32, 0), name);
    };
    auto runtimeDoubleField = [&](unsigned fieldIndex, const char *name) -> llvm::Value * {
        llvm::Value *field = builder.CreateStructGEP(runtimeTy, runtime, fieldIndex, std::string(name) + "_ptr");
        return builder.CreateLoad(f64, field, name);
    };
    auto runtimeIntField = [&](unsigned fieldIndex, const char *name) -> llvm::Value * {
        llvm::Value *field = builder.CreateStructGEP(runtimeTy, runtime, fieldIndex, std::string(name) + "_ptr");
        return builder.CreateLoad(i32, field, name);
    };
    auto runtimeInputDoubleField = [&](unsigned fieldIndex, const char *name) -> llvm::Value * {
        llvm::Value *input = builder.CreateStructGEP(runtimeTy, runtime, 0, "input_ptr");
        llvm::Value *field = builder.CreateStructGEP(inputTy, input, fieldIndex, std::string(name) + "_ptr");
        return builder.CreateLoad(f64, field, name);
    };
    auto runtimeInputIntField = [&](unsigned fieldIndex, const char *name) -> llvm::Value * {
        llvm::Value *input = builder.CreateStructGEP(runtimeTy, runtime, 0, "input_ptr");
        llvm::Value *field = builder.CreateStructGEP(inputTy, input, fieldIndex, std::string(name) + "_ptr");
        return builder.CreateLoad(i32, field, name);
    };
    auto variableNumberField = [&](llvm::Value *variable, const char *name) -> llvm::Value * {
        llvm::Value *valuePtr = builder.CreateStructGEP(variableTy, variable, 5, std::string(name) + "_value_ptr");
        llvm::Value *numberPtr = builder.CreateStructGEP(svalueTy, valuePtr, 1, std::string(name) + "_number_ptr");
        llvm::Value *raw = builder.CreateLoad(f64, numberPtr, std::string(name) + "_raw");
        llvm::Value *isNan = builder.CreateFCmpUNO(raw, raw, std::string(name) + "_is_nan");
        return builder.CreateSelect(isNan, llvm::ConstantFP::get(f64, 0.0), raw, name);
    };
    auto variableRawNumberField = [&](llvm::Value *variable, const char *name) -> llvm::Value * {
        llvm::Value *valuePtr = builder.CreateStructGEP(variableTy, variable, 5, std::string(name) + "_value_ptr");
        llvm::Value *numberPtr = builder.CreateStructGEP(svalueTy, valuePtr, 1, std::string(name) + "_number_ptr");
        return builder.CreateLoad(f64, numberPtr, name);
    };
    auto variableScalarKindField = [&](llvm::Value *variable, const char *name) -> llvm::Value * {
        llvm::Value *scalarKindPtr = builder.CreateStructGEP(variableTy, variable, 4, std::string(name) + "_ptr");
        return builder.CreateLoad(i32, scalarKindPtr, name);
    };
    auto variableValueTagField = [&](llvm::Value *variable, const char *name) -> llvm::Value * {
        llvm::Value *valuePtr = builder.CreateStructGEP(variableTy, variable, 5, std::string(name) + "_value_ptr");
        llvm::Value *tagPtr = builder.CreateStructGEP(svalueTy, valuePtr, 0, std::string(name) + "_tag_ptr");
        return builder.CreateLoad(i32, tagPtr, name);
    };
    auto variableValueNumberPtr = [&](llvm::Value *variable, const char *name) -> llvm::Value * {
        llvm::Value *valuePtr = builder.CreateStructGEP(variableTy, variable, 5, std::string(name) + "_value_ptr");
        return builder.CreateStructGEP(svalueTy, valuePtr, 1, std::string(name) + "_number_ptr");
    };
    auto variableValuePtrPtr = [&](llvm::Value *variable, const char *name) -> llvm::Value * {
        llvm::Value *valuePtr = builder.CreateStructGEP(variableTy, variable, 5, std::string(name) + "_value_ptr");
        return builder.CreateStructGEP(svalueTy, valuePtr, 2, std::string(name) + "_ptr_ptr");
    };
    auto spriteBasePtr = [&](llvm::Value *sprite, const char *name) -> llvm::Value * {
        return builder.CreateStructGEP(spriteTy, sprite, 0, std::string(name) + "_base_ptr");
    };
    auto targetVariablesPtr = [&](llvm::Value *target, const char *name) -> llvm::Value * {
        llvm::Value *variablesPtr = builder.CreateStructGEP(targetTy, target, 4, std::string(name) + "_variables_ptr");
        return builder.CreateLoad(ptr, variablesPtr, std::string(name) + "_variables");
    };
    auto targetVariableCount = [&](llvm::Value *target, const char *name) -> llvm::Value * {
        llvm::Value *countPtr = builder.CreateStructGEP(targetTy, target, 5, std::string(name) + "_variable_count_ptr");
        return builder.CreateLoad(i32, countPtr, std::string(name) + "_variable_count");
    };
    std::unordered_map<std::string, llvm::Value *> entryHoistedVariables;
    std::unordered_map<const SSprite *, llvm::Value *> entryOwnerVariableBases;
    auto emitEntryStableVariable = [&](
        int variableType,
        const char *variableName,
        const SSprite *stableOwner,
        int variableIndex,
        const std::string &name) -> llvm::Value * {
        (void)variableName;
        const std::string key = "stable:" + std::to_string(variableType) + ":" +
            std::to_string(reinterpret_cast<std::uintptr_t>(stableOwner)) + ":" +
            std::to_string(variableIndex);
        auto found = entryHoistedVariables.find(key);
        if (found != entryHoistedVariables.end()) {
            return found->second;
        }
        llvm::IRBuilder<> hoistBuilder(nativeEntryPrologue->getTerminator());
        llvm::Value *variables = nullptr;
        auto owner = entryOwnerVariableBases.find(stableOwner);
        if (owner != entryOwnerVariableBases.end()) {
            variables = owner->second;
        } else {
            llvm::Value *sprite = constantPointer(stableOwner);
            llvm::Value *target = hoistBuilder.CreateStructGEP(
                spriteTy, sprite, 0, name + "_owner_base");
            variables = hoistBuilder.CreateLoad(
                ptr,
                hoistBuilder.CreateStructGEP(
                    targetTy, target, 4, name + "_owner_variables_ptr"),
                name + "_owner_variables");
            entryOwnerVariableBases.emplace(stableOwner, variables);
        }
        llvm::Value *variable = hoistBuilder.CreateInBoundsGEP(
            variableTy,
            variables,
            llvm::ConstantInt::get(i32, variableIndex),
            name + "_stable_handle");
        entryHoistedVariables.emplace(key, variable);
        return variable;
    };
    auto emitEntryHoistedVariable = [&](
        int variableType,
        const char *variableName,
        llvm::Function *resolver,
        const std::vector<llvm::Value *> &resolverArgs,
        const std::string &name) -> llvm::Value * {
        if (script.jit_runtime_instance_id == 0) {
            return builder.CreateCall(resolver, resolverArgs, name + "_handle");
        }
        const std::string key = std::to_string(variableType) + ":" +
            (variableName ? variableName : "");
        auto found = entryHoistedVariables.find(key);
        if (found != entryHoistedVariables.end()) {
            return found->second;
        }
        llvm::IRBuilder<> hoistBuilder(nativeEntryPrologue->getTerminator());
        llvm::Value *variable = hoistBuilder.CreateCall(
            resolver,
            resolverArgs,
            name + "_entry_handle");
        entryHoistedVariables.emplace(key, variable);
        return variable;
    };
    auto emitCachedVariableLookup = [&](
        llvm::Value *owner,
        llvm::Value *cacheTargetId,
        llvm::Value *cacheIndex,
        llvm::Value *cacheType,
        int targetId,
        int variableType,
        llvm::Function *fallback,
        const std::vector<llvm::Value *> &fallbackArgs,
        const std::string &name) -> llvm::Value * {
        llvm::AllocaInst *slot = entryBuilder.CreateAlloca(ptr, nullptr, name + "_slot");
        auto *checkBlock = llvm::BasicBlock::Create(*context, name + "_cache_check", fn);
        auto *hitBlock = llvm::BasicBlock::Create(*context, name + "_cache_hit", fn);
        auto *fallbackBlock = llvm::BasicBlock::Create(*context, name + "_cache_fallback", fn);
        auto *continueBlock = llvm::BasicBlock::Create(*context, name + "_cache_continue", fn);

        llvm::Value *targetOk = builder.CreateICmpEQ(
            cacheTargetId,
            llvm::ConstantInt::get(i32, targetId),
            name + "_cache_target_ok");
        llvm::Value *typeOk = builder.CreateICmpEQ(
            cacheType,
            llvm::ConstantInt::get(i32, variableType),
            name + "_cache_type_ok");
        llvm::Value *indexOk = builder.CreateICmpSGE(
            cacheIndex,
            llvm::ConstantInt::get(i32, 0),
            name + "_cache_index_ok");
        llvm::Value *ownerOk = builder.CreateNot(isNull(builder, owner), name + "_cache_owner_ok");
        llvm::Value *cacheHeaderOk = builder.CreateAnd(targetOk, typeOk, name + "_cache_header_kind_ok");
        cacheHeaderOk = builder.CreateAnd(cacheHeaderOk, indexOk, name + "_cache_header_index_ok");
        cacheHeaderOk = builder.CreateAnd(cacheHeaderOk, ownerOk, name + "_cache_header_ok");
        builder.CreateCondBr(cacheHeaderOk, checkBlock, fallbackBlock);

        builder.SetInsertPoint(checkBlock);
        llvm::Value *target = spriteBasePtr(owner, name.c_str());
        llvm::Value *count = targetVariableCount(target, name.c_str());
        llvm::Value *inRange = builder.CreateICmpSLT(cacheIndex, count, name + "_cache_in_range");
        builder.CreateCondBr(inRange, hitBlock, fallbackBlock);

        builder.SetInsertPoint(hitBlock);
        llvm::Value *variables = targetVariablesPtr(target, name.c_str());
        llvm::Value *variable = builder.CreateInBoundsGEP(variableTy, variables, cacheIndex, name + "_ptr");
        llvm::Value *variableTypePtr = builder.CreateStructGEP(variableTy, variable, 2, name + "_type_ptr");
        llvm::Value *loadedType = builder.CreateLoad(i32, variableTypePtr, name + "_type");
        llvm::Value *loadedTypeOk = builder.CreateICmpEQ(
            loadedType,
            llvm::ConstantInt::get(i32, variableType),
            name + "_type_ok");
        auto *storeHitBlock = llvm::BasicBlock::Create(*context, name + "_store_hit", fn);
        builder.CreateCondBr(loadedTypeOk, storeHitBlock, fallbackBlock);

        builder.SetInsertPoint(storeHitBlock);
        builder.CreateStore(variable, slot);
        builder.CreateBr(continueBlock);

        builder.SetInsertPoint(fallbackBlock);
        llvm::Value *fallbackVariable = builder.CreateCall(fallback, fallbackArgs, name + "_fallback");
        builder.CreateStore(fallbackVariable, slot);
        builder.CreateBr(continueBlock);

        builder.SetInsertPoint(continueBlock);
        return builder.CreateLoad(ptr, slot, name.c_str());
    };
    auto emitCachedExprScalarVariable = [&](const SExpr *expr, const std::string &name) -> llvm::Value * {
        if (!unsafeRawRuntimeObjectConstantsEnabled()) {
            const std::string variableKey = compiledVariableKey(
                SJIT_VAR_SCALAR,
                expr ? expr->variable_id : nullptr,
                expr && expr->literal.tag == SJIT_VALUE_STRING ?
                    static_cast<const SString *>(expr->literal.ptr) : nullptr);
            if (script.jit_runtime_instance_id != 0 && expr &&
                expr->variable_cache_owner &&
                expr->variable_cache_owner_is_original &&
                expr->variable_cache_runtime_instance_id == script.jit_runtime_instance_id &&
                expr->variable_cache_owner_target_id > 0 &&
                expr->variable_cache_index >= 0 &&
                expr->variable_cache_type == SJIT_VAR_SCALAR) {
                return emitEntryStableVariable(
                    SJIT_VAR_SCALAR,
                    variableKey.c_str(),
                    expr->variable_cache_owner,
                    expr->variable_cache_index,
                    name);
            }
            return emitEntryHoistedVariable(
                SJIT_VAR_SCALAR,
                variableKey.c_str(),
                exprScalarVariable,
                {runtime, llvm::ConstantInt::get(i32, script.target_id), constantPointer(expr)},
                name);
        }
        if (SVariable *variable = preboundExprVariable(expr, SJIT_VAR_SCALAR)) {
            return constantPointer(variable);
        }
        llvm::Value *exprPtr = constantPointer(expr);
        llvm::Value *owner = builder.CreateLoad(
            ptr,
            builder.CreateStructGEP(exprTy, exprPtr, 6, name + "_cache_owner_ptr"),
            name + "_cache_owner");
        llvm::Value *cacheTargetId = builder.CreateLoad(
            i32,
            builder.CreateStructGEP(exprTy, exprPtr, 7, name + "_cache_target_ptr"),
            name + "_cache_target");
        llvm::Value *cacheIndex = builder.CreateLoad(
            i32,
            builder.CreateStructGEP(exprTy, exprPtr, 9, name + "_cache_index_ptr"),
            name + "_cache_index");
        llvm::Value *cacheType = builder.CreateLoad(
            i32,
            builder.CreateStructGEP(exprTy, exprPtr, 10, name + "_cache_type_ptr"),
            name + "_cache_type");
        return emitCachedVariableLookup(
            owner,
            cacheTargetId,
            cacheIndex,
            cacheType,
            script.target_id,
            SJIT_VAR_SCALAR,
            exprScalarVariable,
            {runtime, llvm::ConstantInt::get(i32, script.target_id), exprPtr},
            name);
    };
    auto emitCachedExprListVariable = [&](const SExpr *expr, const std::string &name) -> llvm::Value * {
        if (!unsafeRawRuntimeObjectConstantsEnabled()) {
            const std::string variableKey = compiledVariableKey(
                SJIT_VAR_LIST,
                expr ? expr->variable_id : nullptr,
                expr && expr->literal.tag == SJIT_VALUE_STRING ?
                    static_cast<const SString *>(expr->literal.ptr) : nullptr);
            if (script.jit_runtime_instance_id != 0 && expr &&
                expr->variable_cache_owner &&
                expr->variable_cache_owner_is_original &&
                expr->variable_cache_runtime_instance_id == script.jit_runtime_instance_id &&
                expr->variable_cache_owner_target_id > 0 &&
                expr->variable_cache_index >= 0 &&
                expr->variable_cache_type == SJIT_VAR_LIST) {
                return emitEntryStableVariable(
                    SJIT_VAR_LIST,
                    variableKey.c_str(),
                    expr->variable_cache_owner,
                    expr->variable_cache_index,
                    name);
            }
            return emitEntryHoistedVariable(
                SJIT_VAR_LIST,
                variableKey.c_str(),
                exprListVariable,
                {runtime, llvm::ConstantInt::get(i32, script.target_id), constantPointer(expr)},
                name);
        }
        if (SVariable *variable = preboundExprVariable(expr, SJIT_VAR_LIST)) {
            return constantPointer(variable);
        }
        llvm::Value *exprPtr = constantPointer(expr);
        llvm::Value *owner = builder.CreateLoad(
            ptr,
            builder.CreateStructGEP(exprTy, exprPtr, 6, name + "_cache_owner_ptr"),
            name + "_cache_owner");
        llvm::Value *cacheTargetId = builder.CreateLoad(
            i32,
            builder.CreateStructGEP(exprTy, exprPtr, 7, name + "_cache_target_ptr"),
            name + "_cache_target");
        llvm::Value *cacheIndex = builder.CreateLoad(
            i32,
            builder.CreateStructGEP(exprTy, exprPtr, 9, name + "_cache_index_ptr"),
            name + "_cache_index");
        llvm::Value *cacheType = builder.CreateLoad(
            i32,
            builder.CreateStructGEP(exprTy, exprPtr, 10, name + "_cache_type_ptr"),
            name + "_cache_type");
        return emitCachedVariableLookup(
            owner,
            cacheTargetId,
            cacheIndex,
            cacheType,
            script.target_id,
            SJIT_VAR_LIST,
            exprListVariable,
            {runtime, llvm::ConstantInt::get(i32, script.target_id), exprPtr},
            name);
    };
    struct ListStorageLowering {
        llvm::Value *storage;
        llvm::Value *ok;
    };
    auto emitListStorageFromVariable = [&](llvm::Value *variable, const std::string &name) -> ListStorageLowering {
        llvm::AllocaInst *storageSlot = entryBuilder.CreateAlloca(ptr, nullptr, name + "_storage_slot");
        llvm::AllocaInst *okSlot = entryBuilder.CreateAlloca(i1, nullptr, name + "_storage_ok_slot");
        auto *haveListBlock = llvm::BasicBlock::Create(*context, name + "_have_list", fn);
        auto *noListBlock = llvm::BasicBlock::Create(*context, name + "_no_list", fn);
        auto *continueBlock = llvm::BasicBlock::Create(*context, name + "_list_continue", fn);

        const std::string valueTagName = name + "_value_tag";
        const std::string listName = name + "_list";
        llvm::Value *valueTag = variableValueTagField(variable, valueTagName.c_str());
        llvm::Value *valueIsList = builder.CreateICmpEQ(
            valueTag,
            llvm::ConstantInt::get(i32, SJIT_VALUE_LIST),
            name + "_value_is_list");
        llvm::Value *listPtr = builder.CreateLoad(ptr, variableValuePtrPtr(variable, listName.c_str()), listName);
        llvm::Value *listNotNull = builder.CreateNot(isNull(builder, listPtr), name + "_list_not_null");
        builder.CreateCondBr(builder.CreateAnd(valueIsList, listNotNull, name + "_list_header_ok"), haveListBlock, noListBlock);

        builder.SetInsertPoint(haveListBlock);
        llvm::Value *storagePtr = builder.CreateLoad(
            ptr,
            builder.CreateStructGEP(listTy, listPtr, 0, name + "_storage_ptr"),
            name + "_storage");
        llvm::Value *storageOk = builder.CreateNot(isNull(builder, storagePtr), name + "_storage_not_null");
        builder.CreateStore(storagePtr, storageSlot);
        builder.CreateStore(storageOk, okSlot);
        builder.CreateBr(continueBlock);

        builder.SetInsertPoint(noListBlock);
        builder.CreateStore(llvm::ConstantPointerNull::get(ptr), storageSlot);
        builder.CreateStore(llvm::ConstantInt::getFalse(*context), okSlot);
        builder.CreateBr(continueBlock);

        builder.SetInsertPoint(continueBlock);
        return {
            builder.CreateLoad(ptr, storageSlot, name + "_storage"),
            builder.CreateLoad(i1, okSlot, name + "_storage_ok"),
        };
    };
    auto emitListLengthNumber = [&](const SExpr *expr, const std::string &name) -> NumberLowering {
        llvm::Value *variable = emitCachedExprListVariable(expr, name + "_variable");
        ListStorageLowering list = emitListStorageFromVariable(variable, name);
        llvm::AllocaInst *valueSlot = entryBuilder.CreateAlloca(f64, nullptr, name + "_value_slot");
        auto *directBlock = llvm::BasicBlock::Create(*context, name + "_direct", fn);
        auto *emptyBlock = llvm::BasicBlock::Create(*context, name + "_empty", fn);
        auto *continueBlock = llvm::BasicBlock::Create(*context, name + "_continue", fn);
        builder.CreateCondBr(list.ok, directBlock, emptyBlock);

        builder.SetInsertPoint(directBlock);
        llvm::Value *length = builder.CreateLoad(
            i32,
            builder.CreateStructGEP(listStorageTy, list.storage, 1, name + "_length_ptr"),
            name + "_length");
        builder.CreateStore(builder.CreateSIToFP(length, f64, name + "_length_number"), valueSlot);
        builder.CreateBr(continueBlock);

        builder.SetInsertPoint(emptyBlock);
        builder.CreateStore(llvm::ConstantFP::get(f64, 0.0), valueSlot);
        builder.CreateBr(continueBlock);

        builder.SetInsertPoint(continueBlock);
        return supportedNumber(builder.CreateLoad(f64, valueSlot, name + "_value"), llvm::ConstantInt::getTrue(*context));
    };
    auto emitListItemNumber = [&](const SExpr *expr, const std::string &name) -> NumberLowering {
        NumberLowering index = emitNumberExpr(expr->left);
        if (!index.supported) {
            return unsupportedNumber();
        }
        llvm::Value *variable = emitCachedExprListVariable(expr, name + "_variable");
        ListStorageLowering list = emitListStorageFromVariable(variable, name);
        llvm::AllocaInst *valueSlot = entryBuilder.CreateAlloca(f64, nullptr, name + "_value_slot");
        llvm::AllocaInst *okSlot = entryBuilder.CreateAlloca(i1, nullptr, name + "_ok_slot");
        auto *haveStorageBlock = llvm::BasicBlock::Create(*context, name + "_have_storage", fn);
        auto *indexBlock = llvm::BasicBlock::Create(*context, name + "_index", fn);
        auto *cacheBlock = llvm::BasicBlock::Create(*context, name + "_have_number_cache", fn);
        auto *checkItemsBlock = llvm::BasicBlock::Create(*context, name + "_check_items", fn);
        auto *haveItemsBlock = llvm::BasicBlock::Create(*context, name + "_have_items", fn);
        auto *noFastBlock = llvm::BasicBlock::Create(*context, name + "_no_fast", fn);
        auto *continueBlock = llvm::BasicBlock::Create(*context, name + "_continue", fn);
        builder.CreateCondBr(builder.CreateAnd(index.ok, list.ok, name + "_header_ok"), haveStorageBlock, noFastBlock);

        builder.SetInsertPoint(haveStorageBlock);
        llvm::Value *length = builder.CreateLoad(
            i32,
            builder.CreateStructGEP(listStorageTy, list.storage, 1, name + "_length_ptr"),
            name + "_length");
        llvm::Value *lengthNumber = builder.CreateSIToFP(length, f64, name + "_length_number");
        llvm::Value *geOne = builder.CreateFCmpOGE(index.value, llvm::ConstantFP::get(f64, 1.0), name + "_index_ge_one");
        llvm::Value *leLength = builder.CreateFCmpOLE(index.value, lengthNumber, name + "_index_le_length");
        builder.CreateCondBr(builder.CreateAnd(geOne, leLength, name + "_index_in_range"), indexBlock, noFastBlock);

        builder.SetInsertPoint(indexBlock);
        llvm::Value *zeroBased = builder.CreateSub(
            builder.CreateFPToSI(index.value, i32, name + "_one_based_index"),
            llvm::ConstantInt::get(i32, 1),
            name + "_zero_based_index");
        llvm::Value *numbers = builder.CreateLoad(
            ptr,
            builder.CreateStructGEP(listStorageTy, list.storage, 4, name + "_numbers_ptr"),
            name + "_numbers");
        llvm::Value *numbersValid = builder.CreateICmpNE(
            builder.CreateLoad(
                i32,
                builder.CreateStructGEP(listStorageTy, list.storage, 5, name + "_numbers_valid_ptr"),
                name + "_numbers_valid_raw"),
            llvm::ConstantInt::get(i32, 0),
            name + "_numbers_valid");
        builder.CreateCondBr(
            builder.CreateAnd(
                numbersValid,
                builder.CreateNot(isNull(builder, numbers)),
                name + "_number_cache_ok"),
            cacheBlock,
            checkItemsBlock);

        builder.SetInsertPoint(cacheBlock);
        llvm::Value *cached = builder.CreateLoad(
            f64,
            builder.CreateInBoundsGEP(f64, numbers, zeroBased, name + "_cached_number_ptr"),
            name + "_cached_number_raw");
        builder.CreateStore(
            builder.CreateSelect(
                builder.CreateFCmpUNO(cached, cached),
                llvm::ConstantFP::get(f64, 0.0),
                cached,
                name + "_cached_number"),
            valueSlot);
        builder.CreateStore(llvm::ConstantInt::getTrue(*context), okSlot);
        builder.CreateBr(continueBlock);

        builder.SetInsertPoint(checkItemsBlock);
        llvm::Value *items = builder.CreateLoad(
            ptr,
            builder.CreateStructGEP(listStorageTy, list.storage, 0, name + "_items_ptr"),
            name + "_items");
        builder.CreateCondBr(builder.CreateNot(isNull(builder, items), name + "_items_not_null"), haveItemsBlock, noFastBlock);

        builder.SetInsertPoint(haveItemsBlock);
        llvm::Value *itemPtr = builder.CreateInBoundsGEP(svalueTy, items, zeroBased, name + "_item_ptr");
        llvm::Value *tag = builder.CreateLoad(
            i32,
            builder.CreateStructGEP(svalueTy, itemPtr, 0, name + "_item_tag_ptr"),
            name + "_item_tag");
        llvm::Value *raw = builder.CreateLoad(
            f64,
            builder.CreateStructGEP(svalueTy, itemPtr, 1, name + "_item_number_ptr"),
            name + "_item_raw");
        llvm::Value *isNumber = builder.CreateICmpEQ(
            tag,
            llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER),
            name + "_item_is_number");
        llvm::Value *isBool = builder.CreateICmpEQ(
            tag,
            llvm::ConstantInt::get(i32, SJIT_VALUE_BOOL),
            name + "_item_is_bool");
        llvm::Value *isNan = builder.CreateFCmpUNO(raw, raw, name + "_item_is_nan");
        llvm::Value *cleanNumber = builder.CreateSelect(isNan, llvm::ConstantFP::get(f64, 0.0), raw, name + "_item_number");
        builder.CreateStore(builder.CreateSelect(isNumber, cleanNumber, raw, name + "_item_value"), valueSlot);
        builder.CreateStore(builder.CreateOr(isNumber, isBool, name + "_item_numeric"), okSlot);
        builder.CreateBr(continueBlock);

        builder.SetInsertPoint(noFastBlock);
        builder.CreateStore(llvm::ConstantFP::get(f64, 0.0), valueSlot);
        builder.CreateStore(llvm::ConstantInt::getFalse(*context), okSlot);
        builder.CreateBr(continueBlock);

        builder.SetInsertPoint(continueBlock);
        return supportedNumber(
            builder.CreateLoad(f64, valueSlot, name + "_value"),
            builder.CreateLoad(i1, okSlot, name + "_ok"));
    };
    emitNumberExpr = [&](const SExpr *expr) -> NumberLowering {
        if (!expr) {
            return unsupportedNumber();
        }
        double literal = 0.0;
        if (literalNumber(expr, literal)) {
            return supportedNumber(
                llvm::ConstantFP::get(f64, literal),
                llvm::ConstantInt::getTrue(*context));
        }
        switch (expr->opcode) {
        case SJIT_EXPR_TIMER: {
            llvm::Value *now = runtimeDoubleField(1, "runtime_now_ms");
            llvm::Value *timerStart = runtimeDoubleField(18, "timer_start_ms");
            llvm::Value *elapsed = builder.CreateFSub(now, timerStart, "timer_elapsed_ms");
            llvm::Value *seconds = builder.CreateFDiv(elapsed, llvm::ConstantFP::get(f64, 1000.0), "timer_seconds");
            return supportedNumber(seconds, llvm::ConstantInt::getTrue(*context));
        }
        case SJIT_EXPR_DAYS_SINCE_2000:
            return supportedNumber(
                builder.CreateCall(daysSince2000, {}, "days_since_2000"),
                llvm::ConstantInt::getTrue(*context));
        case SJIT_EXPR_MOUSE_X:
            return supportedNumber(runtimeInputDoubleField(2, "mouse_x"), llvm::ConstantInt::getTrue(*context));
        case SJIT_EXPR_MOUSE_Y:
            return supportedNumber(runtimeInputDoubleField(3, "mouse_y"), llvm::ConstantInt::getTrue(*context));
        case SJIT_EXPR_X_POSITION:
            return supportedNumber(
                builder.CreateCall(spriteX, {builder.CreateCall(runtimeGetSprite, {runtime, llvm::ConstantInt::get(i32, script.target_id)}, "x_position_target")}, "x_position"),
                llvm::ConstantInt::getTrue(*context));
        case SJIT_EXPR_Y_POSITION:
            return supportedNumber(
                builder.CreateCall(spriteY, {builder.CreateCall(runtimeGetSprite, {runtime, llvm::ConstantInt::get(i32, script.target_id)}, "y_position_target")}, "y_position"),
                llvm::ConstantInt::getTrue(*context));
        case SJIT_EXPR_VARIABLE: {
            llvm::Value *variable = emitCachedExprScalarVariable(expr, "expr_variable");
            if (SVariable *bound = preboundExprVariable(expr, SJIT_VAR_SCALAR);
                bound && bound->scalar_kind == SJIT_SCALAR_NUMBER) {
                return supportedNumber(
                    variableNumberField(variable, "expr_variable_number"),
                    llvm::ConstantInt::getTrue(*context));
            }
            llvm::Value *isNumber = builder.CreateCall(variableIsNumber, {variable}, "expr_variable_is_number");
            llvm::Value *numberKind = builder.CreateICmpNE(
                isNumber,
                llvm::ConstantInt::get(i32, 0),
                "expr_variable_number_kind");
            llvm::Value *valueTag = variableValueTagField(variable, "expr_variable_value_tag");
            llvm::Value *valueIsNumber = builder.CreateICmpEQ(
                valueTag,
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER),
                "expr_variable_value_is_number");
            llvm::Value *ok = builder.CreateAnd(numberKind, valueIsNumber, "expr_variable_ok");
            llvm::Value *number = variableNumberField(variable, "expr_variable_number");
            return supportedNumber(number, ok);
        }
        case SJIT_EXPR_LIST_LENGTH:
            return emitListLengthNumber(expr, "list_length");
        case SJIT_EXPR_LIST_ITEM:
            return emitListItemNumber(expr, "list_item_number");
        case SJIT_EXPR_LIST_ITEM_NUMBER: {
            llvm::AllocaInst *itemSlot = entryBuilder.CreateAlloca(svalueTy, nullptr, "list_item_number_item");
            emitValueInto(expr->left, itemSlot);
            llvm::Value *number = builder.CreateCall(
                exprListItemNumberValue,
                {runtime, llvm::ConstantInt::get(i32, script.target_id), constantPointer(expr), itemSlot},
                "list_item_number");
            destroyValueSlot(itemSlot);
            return supportedNumber(number, llvm::ConstantInt::getTrue(*context));
        }
        case SJIT_EXPR_LENGTH: {
            int length = 0;
            if (literalStringLength(expr->left, length)) {
                return supportedNumber(
                    llvm::ConstantFP::get(f64, (double)length),
                    llvm::ConstantInt::getTrue(*context));
            }
            llvm::AllocaInst *valueSlot = entryBuilder.CreateAlloca(svalueTy, nullptr, "length_value");
            emitValueInto(expr->left, valueSlot);
            llvm::Value *number = builder.CreateCall(valueLengthNumber, {runtime, valueSlot}, "length_number");
            destroyValueSlot(valueSlot);
            return supportedNumber(number, llvm::ConstantInt::getTrue(*context));
        }
        case SJIT_EXPR_JOIN:
        case SJIT_EXPR_LETTER_OF:
            return supportedNumber(
                emitValueExprToNumber(expr, "value_number"),
                llvm::ConstantInt::getTrue(*context));
        case SJIT_EXPR_MATHOP: {
            NumberLowering value = emitNumberExpr(expr->left);
            if (!value.supported) {
                return unsupportedNumber();
            }
            if (llvm::Value *direct = emitDirectSimpleMathop(
                    builder,
                    expr,
                    value.value,
                    "mathop_direct")) {
                return supportedNumber(direct, value.ok);
            }
            return supportedNumber(
                builder.CreateCall(mathopNumber, {runtime, constantPointer(expr), value.value}, "mathop_number"),
                value.ok);
        }
        case SJIT_EXPR_ROUND: {
            NumberLowering value = emitNumberExpr(expr->left);
            if (!value.supported) {
                return unsupportedNumber();
            }
            return supportedNumber(builder.CreateCall(roundNumber, {value.value}, "round_number"), value.ok);
        }
        case SJIT_EXPR_ADD:
        case SJIT_EXPR_SUB:
        case SJIT_EXPR_MUL:
        case SJIT_EXPR_DIV:
        case SJIT_EXPR_MOD:
        case SJIT_EXPR_RANDOM: {
            NumberLowering left = emitNumberExpr(expr->left);
            NumberLowering right = emitNumberExpr(expr->right);
            if (!left.supported || !right.supported) {
                return unsupportedNumber();
            }
            llvm::Value *ok = builder.CreateAnd(left.ok, right.ok, "number_expr_ok");
            llvm::Value *value = nullptr;
            if (expr->opcode == SJIT_EXPR_ADD) {
                value = builder.CreateFAdd(left.value, right.value, "number_add");
            } else if (expr->opcode == SJIT_EXPR_SUB) {
                value = builder.CreateFSub(left.value, right.value, "number_sub");
            } else if (expr->opcode == SJIT_EXPR_MUL) {
                value = builder.CreateFMul(left.value, right.value, "number_mul");
            } else if (expr->opcode == SJIT_EXPR_DIV) {
                value = builder.CreateFDiv(left.value, right.value, "number_div");
            } else if (expr->opcode == SJIT_EXPR_MOD) {
                llvm::Value *remainder = builder.CreateFRem(left.value, right.value, "number_mod");
                llvm::Value *quotient = builder.CreateFDiv(remainder, right.value, "number_mod_sign");
                llvm::Value *negative = builder.CreateFCmpOLT(quotient, llvm::ConstantFP::get(f64, 0.0), "number_mod_negative");
                llvm::Value *adjusted = builder.CreateFAdd(remainder, right.value, "number_mod_adjusted");
                value = builder.CreateSelect(negative, adjusted, remainder, "number_mod_result");
            } else {
                value = builder.CreateCall(randomNumber, {left.value, right.value}, "random_number");
            }
            return supportedNumber(value, ok);
        }
        default:
            return unsupportedNumber();
        }
    };
    emitBoolExpr = [&](const SExpr *expr) -> BoolLowering {
        if (!expr) {
            return unsupportedBool();
        }
        bool literal = false;
        if (literalBool(expr, literal)) {
            return supportedBool(
                llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), literal),
                llvm::ConstantInt::getTrue(*context));
        }
        switch (expr->opcode) {
        case SJIT_EXPR_MOUSE_DOWN: {
            llvm::Value *down = builder.CreateOr(
                runtimeInputIntField(4, "mouse_down"),
                runtimeInputIntField(9, "mouse_pressed_edge"),
                "mouse_down_or_edge");
            return supportedBool(
                builder.CreateICmpNE(down, llvm::ConstantInt::get(i32, 0), "mouse_down_bool"),
                llvm::ConstantInt::getTrue(*context));
        }
        case SJIT_EXPR_KEY_PRESSED: {
            const int keyIndex = literalKeyIndex(expr->left);
            if (keyIndex < 0 || keyIndex >= 256) {
                if (!scriptSupportsValueExpr(expr->left)) {
                    return unsupportedBool();
                }
                llvm::AllocaInst *keySlot = entryBuilder.CreateAlloca(svalueTy, nullptr, "key_pressed_value_slot");
                emitValueInto(expr->left, keySlot);
                llvm::Value *pressed = builder.CreateCall(keyPressedValue, {runtime, keySlot}, "key_pressed_value");
                destroyValueSlot(keySlot);
                return supportedBool(
                    builder.CreateICmpNE(pressed, llvm::ConstantInt::get(i32, 0), "key_pressed_bool"),
                    llvm::ConstantInt::getTrue(*context));
            }
            llvm::Value *input = builder.CreateStructGEP(runtimeTy, runtime, 0, "input_ptr");
            llvm::Value *keyDown = builder.CreateStructGEP(inputTy, input, 5, "key_down_ptr");
            llvm::Value *keyPtr = builder.CreateInBoundsGEP(
                keyStateTy,
                keyDown,
                {llvm::ConstantInt::get(i32, 0), llvm::ConstantInt::get(i32, keyIndex)},
                "key_down_slot");
            llvm::Value *pressed = builder.CreateLoad(i32, keyPtr, "key_down");
            return supportedBool(
                builder.CreateICmpNE(pressed, llvm::ConstantInt::get(i32, 0), "key_pressed_bool"),
                llvm::ConstantInt::getTrue(*context));
        }
        case SJIT_EXPR_LIST_CONTAINS: {
            if (!scriptSupportsValueExpr(expr->left)) {
                return unsupportedBool();
            }
            llvm::AllocaInst *itemSlot = entryBuilder.CreateAlloca(svalueTy, nullptr, "list_contains_item");
            emitValueInto(expr->left, itemSlot);
            llvm::Value *contains = builder.CreateCall(
                exprListContainsValue,
                {runtime, llvm::ConstantInt::get(i32, script.target_id), constantPointer(expr), itemSlot},
                "list_contains");
            destroyValueSlot(itemSlot);
            return supportedBool(
                builder.CreateICmpNE(contains, llvm::ConstantInt::get(i32, 0), "list_contains_bool"),
                llvm::ConstantInt::getTrue(*context));
        }
        case SJIT_EXPR_CONTAINS: {
            if (!scriptSupportsValueExpr(expr->left) ||
                !scriptSupportsValueExpr(expr->right)) {
                return unsupportedBool();
            }
            llvm::AllocaInst *textSlot = entryBuilder.CreateAlloca(
                svalueTy,
                nullptr,
                "contains_text_slot");
            llvm::AllocaInst *substringSlot = entryBuilder.CreateAlloca(
                svalueTy,
                nullptr,
                "contains_substring_slot");
            emitValueInto(expr->left, textSlot);
            emitValueInto(expr->right, substringSlot);
            llvm::Value *contains = builder.CreateCall(
                valueContains,
                {runtime, textSlot, substringSlot},
                "contains");
            destroyValueSlot(textSlot);
            destroyValueSlot(substringSlot);
            return supportedBool(
                builder.CreateICmpNE(
                    contains,
                    llvm::ConstantInt::get(i32, 0),
                    "contains_bool"),
                llvm::ConstantInt::getTrue(*context));
        }
        case SJIT_EXPR_VARIABLE: {
            llvm::Value *variable = emitCachedExprScalarVariable(expr, "bool_expr_variable");
            llvm::Value *scalarKind = variableScalarKindField(variable, "bool_expr_variable_scalar_kind");
            llvm::Value *valueTag = variableValueTagField(variable, "bool_expr_variable_value_tag");
            llvm::Value *scalarIsBool = builder.CreateICmpEQ(
                scalarKind,
                llvm::ConstantInt::get(i32, SJIT_SCALAR_BOOL),
                "bool_expr_variable_kind_is_bool");
            llvm::Value *valueIsBool = builder.CreateICmpEQ(
                valueTag,
                llvm::ConstantInt::get(i32, SJIT_VALUE_BOOL),
                "bool_expr_variable_value_is_bool");
            llvm::Value *boolOk = builder.CreateAnd(scalarIsBool, valueIsBool, "bool_expr_variable_bool_ok");
            llvm::Value *scalarIsNumber = builder.CreateICmpEQ(
                scalarKind,
                llvm::ConstantInt::get(i32, SJIT_SCALAR_NUMBER),
                "bool_expr_variable_kind_is_number");
            llvm::Value *valueIsNumber = builder.CreateICmpEQ(
                valueTag,
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER),
                "bool_expr_variable_value_is_number");
            llvm::Value *numberOk = builder.CreateAnd(scalarIsNumber, valueIsNumber, "bool_expr_variable_number_ok");
            llvm::Value *raw = variableRawNumberField(variable, "bool_expr_variable_raw");
            llvm::Value *number = variableNumberField(variable, "bool_expr_variable_number");
            llvm::Value *boolTruthy = builder.CreateFCmpUNE(
                raw,
                llvm::ConstantFP::get(f64, 0.0),
                "bool_expr_variable_bool_truthy");
            llvm::Value *numberTruthy = builder.CreateFCmpONE(
                number,
                llvm::ConstantFP::get(f64, 0.0),
                "bool_expr_variable_number_truthy");
            llvm::Value *value = builder.CreateSelect(boolOk, boolTruthy, numberTruthy, "bool_expr_variable_truthy");
            return supportedBool(value, builder.CreateOr(boolOk, numberOk, "bool_expr_variable_ok"));
        }
        case SJIT_EXPR_LT:
        case SJIT_EXPR_EQ:
        case SJIT_EXPR_GT: {
            if ((!scriptSupportsNumberExpr(expr->left) || !scriptSupportsNumberExpr(expr->right)) &&
                scriptSupportsValueExpr(expr->left) && scriptSupportsValueExpr(expr->right)) {
                llvm::AllocaInst *leftSlot = entryBuilder.CreateAlloca(svalueTy, nullptr, "compare_left_value");
                llvm::AllocaInst *rightSlot = entryBuilder.CreateAlloca(svalueTy, nullptr, "compare_right_value");
                emitValueInto(expr->left, leftSlot);
                emitValueInto(expr->right, rightSlot);
                llvm::Value *compared = builder.CreateCall(
                    directCachedComparisons ? fastCachedValueCompare : valueCompare,
                    {runtime, leftSlot, rightSlot, llvm::ConstantInt::get(i32, expr->opcode)},
                    "value_compare");
                destroyValueSlot(leftSlot);
                destroyValueSlot(rightSlot);
                return supportedBool(
                    builder.CreateICmpNE(compared, llvm::ConstantInt::get(i32, 0), "value_compare_bool"),
                    llvm::ConstantInt::getTrue(*context));
            }
            NumberLowering left = emitNumberExpr(expr->left);
            NumberLowering right = emitNumberExpr(expr->right);
            if (!left.supported || !right.supported) {
                return unsupportedBool();
            }
            llvm::Value *ok = builder.CreateAnd(left.ok, right.ok, "compare_number_ok");
            llvm::Value *value = nullptr;
            if (expr->opcode == SJIT_EXPR_LT) {
                value = builder.CreateFCmpOLT(left.value, right.value, "number_lt");
            } else if (expr->opcode == SJIT_EXPR_EQ) {
                value = builder.CreateFCmpOEQ(left.value, right.value, "number_eq");
            } else {
                value = builder.CreateFCmpOGT(left.value, right.value, "number_gt");
            }
            return supportedBool(value, ok);
        }
        case SJIT_EXPR_AND:
        case SJIT_EXPR_OR: {
            BoolLowering left = emitBoolExpr(expr->left);
            BoolLowering right = emitBoolExpr(expr->right);
            if (!left.supported || !right.supported) {
                return unsupportedBool();
            }
            llvm::Value *ok = builder.CreateAnd(left.ok, right.ok, "logic_ok");
            llvm::Value *value = expr->opcode == SJIT_EXPR_AND ?
                builder.CreateAnd(left.value, right.value, "logic_and") :
                builder.CreateOr(left.value, right.value, "logic_or");
            return supportedBool(value, ok);
        }
        case SJIT_EXPR_NOT: {
            BoolLowering operand = emitBoolExpr(expr->left);
            if (!operand.supported) {
                return unsupportedBool();
            }
            return supportedBool(builder.CreateNot(operand.value, "logic_not"), operand.ok);
        }
        default: {
            if (procedureExprHasNumberResult(expr) && scriptSupportsNumberExpr(expr)) {
                NumberLowering number = emitNumberExpr(expr);
                if (!number.supported) {
                    return unsupportedBool();
                }
                return supportedBool(
                    builder.CreateFCmpONE(number.value, llvm::ConstantFP::get(f64, 0.0), "number_truthy"),
                    number.ok);
            }
            if (scriptSupportsValueExpr(expr)) {
                return supportedBool(
                    emitValueExprTruthy(expr, "value_truthy"),
                    llvm::ConstantInt::getTrue(*context));
            }
            return unsupportedBool();
        }
        }
    };

    emitValueInto = [&](const SExpr *expr, llvm::Value *valueSlot) {
        if (!valueSlot) {
            return;
        }
        if (!expr) {
            storeScalarValue(
                valueSlot,
                SJIT_VALUE_NUMBER,
                llvm::ConstantFP::get(f64, 0.0));
            return;
        }
        switch (expr->opcode) {
        case SJIT_EXPR_LITERAL:
            if (expr->literal.tag == SJIT_VALUE_NUMBER ||
                expr->literal.tag == SJIT_VALUE_BOOL ||
                expr->literal.tag == SJIT_VALUE_NULL) {
                const int tag = expr->literal.tag;
                const double number = tag == SJIT_VALUE_BOOL ?
                    (expr->literal.number != 0.0 ? 1.0 : 0.0) :
                    (tag == SJIT_VALUE_NUMBER && !std::isnan(expr->literal.number) ?
                        expr->literal.number : 0.0);
                storeScalarValue(valueSlot, tag, llvm::ConstantFP::get(f64, number));
                return;
            }
            builder.CreateCall(exprLiteralValue, {constantPointer(expr), valueSlot});
            return;
        case SJIT_EXPR_VARIABLE:
            if (SVariable *bound = preboundExprVariable(expr, SJIT_VAR_SCALAR);
                bound && bound->scalar_kind == SJIT_SCALAR_NUMBER) {
                storeScalarValue(
                    valueSlot,
                    SJIT_VALUE_NUMBER,
                    variableNumberField(emitCachedExprScalarVariable(expr, "value_variable"), "value_variable_number"));
                return;
            }
            builder.CreateCall(
                exprVariableValue,
                {runtime, llvm::ConstantInt::get(i32, script.target_id), constantPointer(expr), valueSlot});
            return;
        case SJIT_EXPR_LIST_ITEM: {
            NumberLowering numericIndex = directProcedureListValues &&
                    scriptSupportsNumberExpr(expr->left) ?
                emitNumberExpr(expr->left) : unsupportedNumber();
            if (numericIndex.supported) {
                SList *knownList = preboundListPointers ? preboundExprList(expr) : nullptr;
                builder.CreateCall(
                    fastListValueAtNumber,
                    {
                        runtime,
                        emitCachedExprListVariable(expr, "value_list_variable"),
                        knownList ? constantPointer(knownList) :
                            llvm::ConstantPointerNull::get(ptr),
                        numericIndex.value,
                        valueSlot,
                    });
                return;
            }
            llvm::AllocaInst *indexSlot = entryBuilder.CreateAlloca(svalueTy, nullptr, "list_item_index");
            emitValueInto(expr->left, indexSlot);
            builder.CreateCall(
                exprListItemValue,
                {runtime, llvm::ConstantInt::get(i32, script.target_id), constantPointer(expr), indexSlot, valueSlot});
            destroyValueSlot(indexSlot);
            return;
        }
        case SJIT_EXPR_JOIN: {
            llvm::AllocaInst *leftSlot = entryBuilder.CreateAlloca(svalueTy, nullptr, "join_left");
            llvm::AllocaInst *rightSlot = entryBuilder.CreateAlloca(svalueTy, nullptr, "join_right");
            emitValueInto(expr->left, leftSlot);
            emitValueInto(expr->right, rightSlot);
            builder.CreateCall(valueJoinPtr, {runtime, leftSlot, rightSlot, valueSlot});
            destroyValueSlot(leftSlot);
            destroyValueSlot(rightSlot);
            return;
        }
        case SJIT_EXPR_LETTER_OF: {
            llvm::AllocaInst *indexSlot = entryBuilder.CreateAlloca(svalueTy, nullptr, "letter_index");
            llvm::AllocaInst *textSlot = entryBuilder.CreateAlloca(svalueTy, nullptr, "letter_text");
            emitValueInto(expr->left, indexSlot);
            emitValueInto(expr->right, textSlot);
            builder.CreateCall(valueLetterOf, {runtime, indexSlot, textSlot, valueSlot});
            destroyValueSlot(indexSlot);
            destroyValueSlot(textSlot);
            return;
        }
        default:
            break;
        }
        if (procedureExprHasNumberResult(expr) && scriptSupportsNumberExpr(expr)) {
            NumberLowering number = emitNumberExpr(expr);
            if (number.supported) {
                storeScalarValue(valueSlot, SJIT_VALUE_NUMBER, number.value);
                return;
            }
        }
        if (scriptSupportsBoolExpr(expr)) {
            BoolLowering truthy = emitBoolExpr(expr);
            if (truthy.supported) {
                storeScalarValue(
                    valueSlot,
                    SJIT_VALUE_BOOL,
                    builder.CreateUIToFP(truthy.value, f64, "bool_value_number"));
                return;
            }
        }
        storeScalarValue(
            valueSlot,
            SJIT_VALUE_NUMBER,
            llvm::ConstantFP::get(f64, 0.0));
    };

    const int donePc = static_cast<int>(nodes.size());
    auto pcConstant = [&](int pcValue) -> llvm::ConstantInt * {
        return llvm::ConstantInt::get(i32, pcValue < 0 ? donePc : pcValue);
    };
    auto emitJumpToPc = [&](int nextPc) {
        llvm::Value *next = pcConstant(nextPc);
        /* pcSlot is promoted to SSA by the optimization pipeline.  The frame
           is the deoptimization/resume record, not the intra-region branch
           variable, so write it only at safepoints and region exits. */
        builder.CreateStore(next, pcSlot);
        builder.CreateBr(nextPc < 0 ? done : dispatch);
    };
    auto emitStatusContinuation = [&](int labelPc, llvm::Value *status, int resumePc, int nextPc) {
        auto *statusYield = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(labelPc) + "_yield", fn);
        auto *statusNotOk = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(labelPc) + "_status", fn);
        auto *returnStatus = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(labelPc) + "_return", fn);
        auto *statusOk = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(labelPc) + "_ok", fn);
        llvm::Value *isYield = builder.CreateICmpEQ(status, llvm::ConstantInt::get(i32, SJIT_STATUS_YIELDED));
        llvm::Value *isYieldTick = builder.CreateICmpEQ(status, llvm::ConstantInt::get(i32, SJIT_STATUS_YIELD_TICK));
        llvm::Value *isWaiting = builder.CreateICmpEQ(status, llvm::ConstantInt::get(i32, SJIT_STATUS_WAITING));
        llvm::Value *shouldPause = builder.CreateOr(builder.CreateOr(isYield, isYieldTick), isWaiting);
        builder.CreateCondBr(shouldPause, statusYield, statusNotOk);

        builder.SetInsertPoint(statusYield);
        storeFramePc(pcConstant(resumePc));
        builder.CreateRet(status);

        builder.SetInsertPoint(statusNotOk);
        llvm::Value *notOk = builder.CreateICmpNE(status, llvm::ConstantInt::get(i32, SJIT_STATUS_OK));
        builder.CreateCondBr(notOk, returnStatus, statusOk);

        builder.SetInsertPoint(returnStatus);
        builder.CreateRet(status);

        builder.SetInsertPoint(statusOk);
        emitJumpToPc(nextPc);
    };
    auto loopStateCounterPtr = [&](llvm::Value *state) -> llvm::Value * {
        return builder.CreateStructGEP(loopStateTy, state, 2, "loop_counter_ptr");
    };
    auto loopStateBranchActivePtr = [&](llvm::Value *state) -> llvm::Value * {
        return builder.CreateStructGEP(loopStateTy, state, 3, "loop_branch_active_ptr");
    };
    auto loopStateSubPcPtr = [&](llvm::Value *state) -> llvm::Value * {
        return builder.CreateStructGEP(loopStateTy, state, 4, "loop_sub_pc_ptr");
    };
    auto setLoopBranchActive = [&](llvm::Value *state) {
        llvm::Value *activePtr = loopStateBranchActivePtr(state);
        llvm::Value *subPcPtr = loopStateSubPcPtr(state);
        llvm::Value *wasActive = builder.CreateLoad(i32, activePtr, "loop_was_active");
        llvm::Value *currentSubPc = builder.CreateLoad(i32, subPcPtr, "loop_current_sub_pc");
        llvm::Value *nextSubPc = builder.CreateSelect(
            builder.CreateICmpNE(wasActive, llvm::ConstantInt::get(i32, 0)),
            currentSubPc,
            llvm::ConstantInt::get(i32, 0),
            "loop_next_sub_pc");
        builder.CreateStore(llvm::ConstantInt::get(i32, 1), activePtr);
        builder.CreateStore(nextSubPc, subPcPtr);
    };
    auto statementSprite = [&]() -> llvm::Value * {
        if (unsafeRawRuntimeObjectConstantsEnabled() && script.bound_target) {
            return constantPointer(script.bound_target);
        }
        llvm::Value *targetId = llvm::ConstantInt::get(i32, script.target_id);
        return builder.CreateCall(runtimeGetSprite, {runtime, targetId}, "target");
    };
    auto frameWarpModePtr = [&]() -> llvm::Value * {
        return builder.CreateStructGEP(frameTy, frame, 3, "frame_warp_mode_ptr");
    };
    auto emitDirectProcedureStatementCall = [&](const SStatement *callStatement, llvm::Value *statementPtr) -> llvm::Value * {
        (void)statementPtr;
        const int target = findProcedureIndex(script, callStatement ? callStatement->procedure_name : nullptr);
        if (target < 0) {
            return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
        }
        if (!callStatement ||
            (callStatement->argument_count > 0 && !callStatement->arguments)) {
            return llvm::ConstantInt::get(i32, SJIT_STATUS_ERROR);
        }
        if (!directProcedures[static_cast<size_t>(target)] ||
            !procedureFunctions[static_cast<size_t>(target)]) {
            return builder.CreateCall(
                scriptExecuteProcedureStatement,
                {runtime, thread, frame, scriptData, constantPointer(callStatement)},
                "procedure_fallback_status");
        }
        if (callStatement->argument_count != script.procedures[target].argument_count) {
            return builder.CreateCall(
                scriptExecuteProcedureStatement,
                {runtime, thread, frame, scriptData, constantPointer(callStatement)},
                "procedure_arity_fallback_status");
        }
        for (int i = 0; i < callStatement->argument_count; ++i) {
            if (!scriptSupportsValueExpr(callStatement->arguments[i].value)) {
                return builder.CreateCall(
                    scriptExecuteProcedureStatement,
                    {runtime, thread, frame, scriptData, constantPointer(callStatement)},
                    "procedure_fallback_status");
            }
        }

        auto emitDirectCall = [&](llvm::Value *numericArguments,
                                  llvm::Value *valueArguments,
                                  const std::string &callName) -> llvm::Value * {
            llvm::Value *previousWarp =
                builder.CreateLoad(i32, frameWarpModePtr(), callName + "_previous_warp");
            if (script.procedures[target].warp_mode) {
                builder.CreateStore(llvm::ConstantInt::get(i32, 1), frameWarpModePtr());
            }
            llvm::Value *status = builder.CreateCall(
                procedureFunctions[static_cast<size_t>(target)],
                {
                    runtime,
                    thread,
                    frame,
                    scriptData,
                    llvm::ConstantInt::get(i32, 1),
                    numericArguments,
                    valueArguments,
                },
                callName + "_status");
            if (script.procedures[target].warp_mode) {
                builder.CreateStore(previousWarp, frameWarpModePtr());
            }
            return builder.CreateSelect(
                builder.CreateICmpEQ(status, llvm::ConstantInt::get(i32, SJIT_STATUS_DONE)),
                llvm::ConstantInt::get(i32, SJIT_STATUS_OK),
                status,
                callName + "_normalized_status");
        };

        bool numericOnlyCall = !statementsMaySuspendOrCallUnknown(
            script.procedures[target].statements,
            script.procedures[target].statement_count);
        for (int i = 0; numericOnlyCall && i < callStatement->argument_count; ++i) {
            const bool calleeNeedsValue =
                target >= static_cast<int>(procedureArgumentNeedsValue.size()) ||
                i >= static_cast<int>(procedureArgumentNeedsValue[static_cast<size_t>(target)].size()) ||
                procedureArgumentNeedsValue[static_cast<size_t>(target)][static_cast<size_t>(i)];
            numericOnlyCall = !calleeNeedsValue &&
                scriptSupportsNumberExpr(callStatement->arguments[i].value) &&
                exprIsGuardReplaySafe(callStatement->arguments[i].value);
        }
        if (numericOnlyCall) {
            llvm::Value *argArray = entryBuilder.CreateAlloca(
                f64,
                llvm::ConstantInt::get(
                    i32,
                    callStatement->argument_count > 0 ? callStatement->argument_count : 1),
                "procedure_numeric_args");
            llvm::Value *allGuards = llvm::ConstantInt::getTrue(*context);
            for (int i = 0; i < callStatement->argument_count; ++i) {
                NumberLowering argument = emitNumberExpr(callStatement->arguments[i].value);
                if (!argument.supported) {
                    numericOnlyCall = false;
                    break;
                }
                builder.CreateStore(
                    argument.value,
                    builder.CreateInBoundsGEP(
                        f64,
                        argArray,
                        llvm::ConstantInt::get(i32, i),
                        "procedure_numeric_arg_ptr"));
                allGuards = builder.CreateAnd(allGuards, argument.ok, "procedure_numeric_guards");
            }
            if (numericOnlyCall) {
                llvm::Value *nullValueArgs = llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(ptr));
                if (auto *constantGuards = llvm::dyn_cast<llvm::ConstantInt>(allGuards);
                    constantGuards && constantGuards->isOne()) {
                    return emitDirectCall(argArray, nullValueArgs, "procedure_numeric_direct");
                }

                llvm::AllocaInst *statusSlot =
                    entryBuilder.CreateAlloca(i32, nullptr, "procedure_numeric_status_slot");
                auto *directBlock = llvm::BasicBlock::Create(
                    *context, "procedure_numeric_guard_ok", fn);
                auto *fallbackBlock = llvm::BasicBlock::Create(
                    *context, "procedure_numeric_guard_fallback", fn);
                auto *continueBlock = llvm::BasicBlock::Create(
                    *context, "procedure_numeric_guard_continue", fn);
                builder.CreateCondBr(allGuards, directBlock, fallbackBlock);

                builder.SetInsertPoint(directBlock);
                builder.CreateStore(
                    emitDirectCall(argArray, nullValueArgs, "procedure_numeric_guarded"),
                    statusSlot);
                builder.CreateBr(continueBlock);

                builder.SetInsertPoint(fallbackBlock);
                builder.CreateStore(
                    builder.CreateCall(
                        scriptExecuteProcedureStatement,
                        {runtime, thread, frame, scriptData, constantPointer(callStatement)},
                        "procedure_numeric_guard_fallback_status"),
                    statusSlot);
                builder.CreateBr(continueBlock);

                builder.SetInsertPoint(continueBlock);
                return builder.CreateLoad(i32, statusSlot, "procedure_numeric_guard_status");
            }
        }

        llvm::Value *argArray = entryBuilder.CreateAlloca(
            f64,
            llvm::ConstantInt::get(i32, callStatement->argument_count > 0 ? callStatement->argument_count : 1),
            "procedure_call_args");
        llvm::Value *valueArgArray = entryBuilder.CreateAlloca(
            svalueTy,
            llvm::ConstantInt::get(i32, callStatement->argument_count > 0 ? callStatement->argument_count : 1),
            "procedure_call_value_args");
        for (int i = 0; i < callStatement->argument_count; ++i) {
            llvm::Value *slot = builder.CreateInBoundsGEP(
                f64,
                argArray,
                llvm::ConstantInt::get(i32, i),
                "procedure_call_arg_ptr");
            llvm::Value *valueSlot = builder.CreateInBoundsGEP(
                svalueTy,
                valueArgArray,
                llvm::ConstantInt::get(i32, i),
                "procedure_call_value_arg_ptr");
            emitValueInto(callStatement->arguments[i].value, valueSlot);
            llvm::Value *number = builder.CreateCall(
                valuePtrToNumber,
                {runtime, valueSlot},
                "procedure_call_arg_number");
            builder.CreateStore(number, slot);
        }

        llvm::Value *status = emitDirectCall(
            argArray,
            valueArgArray,
            "procedure_direct");
        for (int i = 0; i < callStatement->argument_count; ++i) {
            llvm::Value *valueSlot = builder.CreateInBoundsGEP(
                svalueTy,
                valueArgArray,
                llvm::ConstantInt::get(i32, i),
                "procedure_call_value_arg_destroy_ptr");
            destroyValueSlot(valueSlot);
        }
        return status;
    };

    for (const JitStatementNode &node : nodes) {
        builder.SetInsertPoint(statementBlocks[static_cast<size_t>(node.pc)]);
        const SStatement *statement = node.statement;
        llvm::Value *statementPtr = constantPointer(statement);
        auto emitCachedStatementVariable = [&](int variableType, llvm::Function *fallback, const std::string &name) -> llvm::Value * {
            if (!unsafeRawRuntimeObjectConstantsEnabled()) {
                const std::string variableKey = compiledVariableKey(
                    variableType,
                    statement ? statement->variable_id : nullptr,
                    statement ? statement->variable_name : nullptr);
                if (script.jit_runtime_instance_id != 0 && statement &&
                    statement->variable_cache_owner &&
                    statement->variable_cache_owner_is_original &&
                    statement->variable_cache_runtime_instance_id ==
                        script.jit_runtime_instance_id &&
                    statement->variable_cache_owner_target_id > 0 &&
                    statement->variable_cache_index >= 0 &&
                    statement->variable_cache_type == variableType) {
                    return emitEntryStableVariable(
                        variableType,
                        variableKey.c_str(),
                        statement->variable_cache_owner,
                        statement->variable_cache_index,
                        name);
                }
                return emitEntryHoistedVariable(
                    variableType,
                    variableKey.c_str(),
                    fallback,
                    {runtime, scriptData, statementPtr},
                    name);
            }
            if (SVariable *variable = preboundStatementVariable(statement, variableType)) {
                return constantPointer(variable);
            }
            llvm::Value *owner = builder.CreateLoad(
                ptr,
                builder.CreateStructGEP(statementTy, statementPtr, 13, name + "_cache_owner_ptr"),
                name + "_cache_owner");
            llvm::Value *cacheTargetId = builder.CreateLoad(
                i32,
                builder.CreateStructGEP(statementTy, statementPtr, 14, name + "_cache_target_ptr"),
                name + "_cache_target");
            llvm::Value *cacheIndex = builder.CreateLoad(
                i32,
                builder.CreateStructGEP(statementTy, statementPtr, 16, name + "_cache_index_ptr"),
                name + "_cache_index");
            llvm::Value *cacheType = builder.CreateLoad(
                i32,
                builder.CreateStructGEP(statementTy, statementPtr, 17, name + "_cache_type_ptr"),
                name + "_cache_type");
            return emitCachedVariableLookup(
                owner,
                cacheTargetId,
                cacheIndex,
                cacheType,
                script.target_id,
                variableType,
                fallback,
                {runtime, scriptData, statementPtr},
                name);
        };
        auto emitCachedStatementScalarVariable = [&](const std::string &name) -> llvm::Value * {
            return emitCachedStatementVariable(SJIT_VAR_SCALAR, statementScalarVariablePtr, name);
        };
        auto emitCachedStatementListVariable = [&](const std::string &name) -> llvm::Value * {
            return emitCachedStatementVariable(SJIT_VAR_LIST, statementListVariablePtr, name);
        };
        auto evalStatementNumberSlot = [&](int slot, const char *valueName) -> llvm::Value * {
            const SExpr *expr = statement ? statementExprForSlot(*statement, slot) : nullptr;
            double constant = 0.0;
            if (literalNumber(expr, constant)) {
                return llvm::ConstantFP::get(f64, constant);
            }
            NumberLowering lowered = emitNumberExpr(expr);
            if (lowered.supported) {
                if (auto *constantOk = llvm::dyn_cast<llvm::ConstantInt>(lowered.ok);
                    constantOk && constantOk->isOne()) {
                    return lowered.value;
                }
                llvm::AllocaInst *slotValue = entryBuilder.CreateAlloca(
                    f64,
                    nullptr,
                    std::string(valueName) + "_fast_value");
                auto *directBlock = llvm::BasicBlock::Create(
                    *context,
                    "pc_" + std::to_string(node.pc) + "_" + valueName + "_number",
                    fn);
                auto *fallbackBlock = llvm::BasicBlock::Create(
                    *context,
                    "pc_" + std::to_string(node.pc) + "_" + valueName + "_number_fallback",
                    fn);
                auto *continueBlock = llvm::BasicBlock::Create(
                    *context,
                    "pc_" + std::to_string(node.pc) + "_" + valueName + "_number_continue",
                    fn);
                builder.CreateCondBr(lowered.ok, directBlock, fallbackBlock);

                builder.SetInsertPoint(directBlock);
                builder.CreateStore(lowered.value, slotValue);
                builder.CreateBr(continueBlock);

                builder.SetInsertPoint(fallbackBlock);
                llvm::Value *fallback = builder.CreateCall(
                    statementNumber,
                    {runtime, scriptData, statementPtr, llvm::ConstantInt::get(i32, slot)},
                    std::string(valueName) + "_fallback");
                builder.CreateStore(fallback, slotValue);
                builder.CreateBr(continueBlock);

                builder.SetInsertPoint(continueBlock);
                return builder.CreateLoad(f64, slotValue, valueName);
            }
            return builder.CreateCall(
                statementNumber,
                {runtime, scriptData, statementPtr, llvm::ConstantInt::get(i32, slot)},
                valueName);
        };
        auto evalStatementBoolSlot = [&](int slot, const char *valueName) -> llvm::Value * {
            const SExpr *expr = statement ? statementExprForSlot(*statement, slot) : nullptr;
            BoolLowering lowered = emitBoolExpr(expr);
            if (lowered.supported) {
                if (auto *constantOk = llvm::dyn_cast<llvm::ConstantInt>(lowered.ok);
                    constantOk && constantOk->isOne()) {
                    return lowered.value;
                }
                llvm::AllocaInst *slotValue = entryBuilder.CreateAlloca(
                    llvm::Type::getInt1Ty(*context),
                    nullptr,
                    std::string(valueName) + "_fast_value");
                auto *directBlock = llvm::BasicBlock::Create(
                    *context,
                    "pc_" + std::to_string(node.pc) + "_" + valueName + "_bool",
                    fn);
                auto *fallbackBlock = llvm::BasicBlock::Create(
                    *context,
                    "pc_" + std::to_string(node.pc) + "_" + valueName + "_bool_fallback",
                    fn);
                auto *continueBlock = llvm::BasicBlock::Create(
                    *context,
                    "pc_" + std::to_string(node.pc) + "_" + valueName + "_bool_continue",
                    fn);
                builder.CreateCondBr(lowered.ok, directBlock, fallbackBlock);

                builder.SetInsertPoint(directBlock);
                builder.CreateStore(lowered.value, slotValue);
                builder.CreateBr(continueBlock);

                builder.SetInsertPoint(fallbackBlock);
                llvm::Value *truthy = builder.CreateCall(
                    statementBool,
                    {runtime, scriptData, statementPtr, llvm::ConstantInt::get(i32, slot)},
                    std::string(valueName) + "_fallback");
                builder.CreateStore(
                    builder.CreateICmpNE(truthy, llvm::ConstantInt::get(i32, 0), valueName),
                    slotValue);
                builder.CreateBr(continueBlock);

                builder.SetInsertPoint(continueBlock);
                return builder.CreateLoad(llvm::Type::getInt1Ty(*context), slotValue, valueName);
            }
            llvm::Value *truthy = builder.CreateCall(
                statementBool,
                {runtime, scriptData, statementPtr, llvm::ConstantInt::get(i32, slot)},
                valueName);
            return builder.CreateICmpNE(truthy, llvm::ConstantInt::get(i32, 0), valueName);
        };
        auto emitNumericVariableFastPath = [&](bool change, llvm::Function *fallback) -> bool {
            if (!statement) {
                return false;
            }
            if (!change && statement->value && statement->value->opcode == SJIT_EXPR_LITERAL &&
                (statement->value->literal.tag == SJIT_VALUE_STRING ||
                 statement->value->literal.tag == SJIT_VALUE_LIST)) {
                return false;
            }
            NumberLowering number = emitNumberExpr(statement->value);
            if (!number.supported) {
                return false;
            }

            auto *directBlock = llvm::BasicBlock::Create(
                *context,
                "pc_" + std::to_string(node.pc) + (change ? "_change_number" : "_set_number"),
                fn);
            auto *fallbackBlock = llvm::BasicBlock::Create(
                *context,
                "pc_" + std::to_string(node.pc) + (change ? "_change_fallback" : "_set_fallback"),
                fn);
            auto *continueBlock = llvm::BasicBlock::Create(
                *context,
                "pc_" + std::to_string(node.pc) + (change ? "_change_continue" : "_set_continue"),
                fn);
            llvm::Value *variable = emitCachedStatementScalarVariable(change ? "change_variable" : "set_variable");
            llvm::Value *isNumber = builder.CreateCall(variableIsNumber, {variable}, "target_variable_is_number");
            llvm::Value *variableOk = builder.CreateICmpNE(
                isNumber,
                llvm::ConstantInt::get(i32, 0),
                "target_variable_ok");
            llvm::Value *valueTag = variableValueTagField(variable, "target_variable_value_tag");
            llvm::Value *valueIsNumber = builder.CreateICmpEQ(
                valueTag,
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER),
                "target_variable_value_is_number");
            llvm::Value *canUseFastPath = builder.CreateAnd(variableOk, valueIsNumber, "numeric_variable_and_value_ok");
            canUseFastPath = builder.CreateAnd(canUseFastPath, number.ok, "numeric_variable_fast_path");
            builder.CreateCondBr(canUseFastPath, directBlock, fallbackBlock);

            builder.SetInsertPoint(directBlock);
            if (change) {
                llvm::Value *numberPtr = variableValueNumberPtr(variable, "change_variable");
                llvm::Value *current = builder.CreateLoad(f64, numberPtr, "change_variable_current");
                llvm::Value *next = builder.CreateFAdd(current, number.value, "change_variable_next");
                builder.CreateStore(next, numberPtr);
            } else {
                builder.CreateStore(number.value, variableValueNumberPtr(variable, "set_variable"));
            }
            builder.CreateStore(
                llvm::ConstantPointerNull::get(ptr),
                variableValuePtrPtr(variable, change ? "change_variable" : "set_variable"));
            builder.CreateBr(continueBlock);

            builder.SetInsertPoint(fallbackBlock);
            builder.CreateCall(fallback, {runtime, scriptData, statementPtr});
            builder.CreateBr(continueBlock);

            builder.SetInsertPoint(continueBlock);
            return true;
        };
        auto emitBoolVariableFastPath = [&](llvm::Function *fallback) -> bool {
            if (!statement) {
                return false;
            }
            BoolLowering truthy = emitBoolExpr(statement->value);
            if (!truthy.supported) {
                return false;
            }

            auto *directBlock = llvm::BasicBlock::Create(
                *context,
                "pc_" + std::to_string(node.pc) + "_set_bool",
                fn);
            auto *fallbackBlock = llvm::BasicBlock::Create(
                *context,
                "pc_" + std::to_string(node.pc) + "_set_bool_fallback",
                fn);
            auto *continueBlock = llvm::BasicBlock::Create(
                *context,
                "pc_" + std::to_string(node.pc) + "_set_bool_continue",
                fn);
            llvm::Value *variable = emitCachedStatementScalarVariable("set_bool_variable");
            llvm::Value *scalarKind = variableScalarKindField(variable, "set_bool_variable_scalar_kind");
            llvm::Value *variableOk = builder.CreateICmpEQ(
                scalarKind,
                llvm::ConstantInt::get(i32, SJIT_SCALAR_BOOL),
                "set_bool_variable_ok");
            llvm::Value *valueTag = variableValueTagField(variable, "set_bool_variable_value_tag");
            llvm::Value *valueIsBool = builder.CreateICmpEQ(
                valueTag,
                llvm::ConstantInt::get(i32, SJIT_VALUE_BOOL),
                "set_bool_variable_value_is_bool");
            llvm::Value *canUseFastPath = builder.CreateAnd(variableOk, valueIsBool, "set_bool_variable_and_value_ok");
            canUseFastPath = builder.CreateAnd(canUseFastPath, truthy.ok, "set_bool_variable_fast_path");
            builder.CreateCondBr(canUseFastPath, directBlock, fallbackBlock);

            builder.SetInsertPoint(directBlock);
            llvm::Value *asNumber = builder.CreateUIToFP(truthy.value, f64, "set_bool_variable_number");
            builder.CreateStore(asNumber, variableValueNumberPtr(variable, "set_bool_variable"));
            builder.CreateStore(llvm::ConstantPointerNull::get(ptr), variableValuePtrPtr(variable, "set_bool_variable"));
            builder.CreateBr(continueBlock);

            builder.SetInsertPoint(fallbackBlock);
            builder.CreateCall(fallback, {runtime, scriptData, statementPtr});
            builder.CreateBr(continueBlock);

            builder.SetInsertPoint(continueBlock);
            return true;
        };
        auto expressionAllowsNumericListWrite = [](const SExpr *expr) -> bool {
            return !expr ||
                expr->opcode != SJIT_EXPR_LITERAL ||
                (expr->literal.tag != SJIT_VALUE_STRING && expr->literal.tag != SJIT_VALUE_LIST);
        };
        auto emitListAddNumberFastPath = [&]() -> bool {
            if (!statement || !expressionAllowsNumericListWrite(statement->value)) {
                return false;
            }
            NumberLowering value = emitNumberExpr(statement->value);
            if (!value.supported) {
                return false;
            }

            llvm::Value *variable = emitCachedStatementListVariable("list_add_variable");
            ListStorageLowering list = emitListStorageFromVariable(variable, "list_add");
            auto *checkBlock = llvm::BasicBlock::Create(
                *context,
                "pc_" + std::to_string(node.pc) + "_list_add_check",
                fn);
            auto *directBlock = llvm::BasicBlock::Create(
                *context,
                "pc_" + std::to_string(node.pc) + "_list_add_number",
                fn);
            auto *cacheBlock = llvm::BasicBlock::Create(
                *context,
                "pc_" + std::to_string(node.pc) + "_list_add_cache",
                fn);
            auto *finishBlock = llvm::BasicBlock::Create(
                *context,
                "pc_" + std::to_string(node.pc) + "_list_add_finish",
                fn);
            auto *fallbackBlock = llvm::BasicBlock::Create(
                *context,
                "pc_" + std::to_string(node.pc) + "_list_add_fallback",
                fn);
            auto *continueBlock = llvm::BasicBlock::Create(
                *context,
                "pc_" + std::to_string(node.pc) + "_list_add_continue",
                fn);
            builder.CreateCondBr(builder.CreateAnd(value.ok, list.ok, "list_add_header_ok"), checkBlock, fallbackBlock);

            builder.SetInsertPoint(checkBlock);
            llvm::Value *lengthPtr = builder.CreateStructGEP(listStorageTy, list.storage, 1, "list_add_length_ptr");
            llvm::Value *length = builder.CreateLoad(i32, lengthPtr, "list_add_length");
            llvm::Value *capacity = builder.CreateLoad(
                i32,
                builder.CreateStructGEP(listStorageTy, list.storage, 2, "list_add_capacity_ptr"),
                "list_add_capacity");
            llvm::Value *refCount = builder.CreateLoad(
                i32,
                builder.CreateStructGEP(listStorageTy, list.storage, 3, "list_add_ref_count_ptr"),
                "list_add_ref_count");
            llvm::Value *items = builder.CreateLoad(
                ptr,
                builder.CreateStructGEP(listStorageTy, list.storage, 0, "list_add_items_ptr"),
                "list_add_items");
            llvm::Value *numbers = builder.CreateLoad(
                ptr,
                builder.CreateStructGEP(listStorageTy, list.storage, 4, "list_add_numbers_ptr"),
                "list_add_numbers");
            llvm::Value *numbersValid = builder.CreateICmpNE(
                builder.CreateLoad(
                    i32,
                    builder.CreateStructGEP(listStorageTy, list.storage, 5, "list_add_numbers_valid_ptr"),
                    "list_add_numbers_valid_raw"),
                llvm::ConstantInt::get(i32, 0),
                "list_add_numbers_valid");
            llvm::Value *unique = builder.CreateICmpEQ(refCount, llvm::ConstantInt::get(i32, 1), "list_add_unique");
            llvm::Value *hasCapacity = builder.CreateICmpSLT(length, capacity, "list_add_has_capacity");
            llvm::Value *underLimit = builder.CreateICmpSLT(
                length,
                llvm::ConstantInt::get(i32, SJIT_LIST_ITEM_LIMIT),
                "list_add_under_limit");
            llvm::Value *itemsOk = builder.CreateNot(isNull(builder, items), "list_add_items_ok");
            llvm::Value *cacheReady = builder.CreateOr(
                builder.CreateNot(numbersValid),
                builder.CreateNot(isNull(builder, numbers)),
                "list_add_cache_ready");
            llvm::Value *canUseFastPath = builder.CreateAnd(unique, hasCapacity, "list_add_unique_capacity_ok");
            canUseFastPath = builder.CreateAnd(canUseFastPath, underLimit, "list_add_under_limit_ok");
            canUseFastPath = builder.CreateAnd(canUseFastPath, itemsOk, "list_add_fast_path");
            canUseFastPath = builder.CreateAnd(canUseFastPath, cacheReady, "list_add_cache_fast_path");
            builder.CreateCondBr(canUseFastPath, directBlock, fallbackBlock);

            builder.SetInsertPoint(directBlock);
            llvm::Value *itemPtr = builder.CreateInBoundsGEP(svalueTy, items, length, "list_add_item_ptr");
            builder.CreateStore(
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER),
                builder.CreateStructGEP(svalueTy, itemPtr, 0, "list_add_item_tag_ptr"));
            builder.CreateStore(value.value, builder.CreateStructGEP(svalueTy, itemPtr, 1, "list_add_item_number_ptr"));
            builder.CreateStore(
                llvm::ConstantPointerNull::get(ptr),
                builder.CreateStructGEP(svalueTy, itemPtr, 2, "list_add_item_ptr_ptr"));
            builder.CreateCondBr(numbersValid, cacheBlock, finishBlock);

            builder.SetInsertPoint(cacheBlock);
            builder.CreateStore(
                value.value,
                builder.CreateInBoundsGEP(f64, numbers, length, "list_add_number_cache_ptr"));
            builder.CreateBr(finishBlock);

            builder.SetInsertPoint(finishBlock);
            builder.CreateStore(builder.CreateAdd(length, llvm::ConstantInt::get(i32, 1), "list_add_next_length"), lengthPtr);
            builder.CreateBr(continueBlock);

            builder.SetInsertPoint(fallbackBlock);
            builder.CreateCall(statementListAdd, {runtime, scriptData, statementPtr});
            builder.CreateBr(continueBlock);

            builder.SetInsertPoint(continueBlock);
            return true;
        };
        auto emitListReplaceNumberFastPath = [&]() -> bool {
            if (!statement || !expressionAllowsNumericListWrite(statement->value)) {
                return false;
            }
            NumberLowering index = emitNumberExpr(statement->index);
            NumberLowering value = emitNumberExpr(statement->value);
            if (!index.supported || !value.supported) {
                return false;
            }

            llvm::Value *variable = emitCachedStatementListVariable("list_replace_variable");
            ListStorageLowering list = emitListStorageFromVariable(variable, "list_replace");
            auto *haveStorageBlock = llvm::BasicBlock::Create(
                *context,
                "pc_" + std::to_string(node.pc) + "_list_replace_storage",
                fn);
            auto *indexBlock = llvm::BasicBlock::Create(
                *context,
                "pc_" + std::to_string(node.pc) + "_list_replace_index",
                fn);
            auto *haveItemsBlock = llvm::BasicBlock::Create(
                *context,
                "pc_" + std::to_string(node.pc) + "_list_replace_items",
                fn);
            auto *directBlock = llvm::BasicBlock::Create(
                *context,
                "pc_" + std::to_string(node.pc) + "_list_replace_number",
                fn);
            auto *cacheBlock = llvm::BasicBlock::Create(
                *context,
                "pc_" + std::to_string(node.pc) + "_list_replace_cache",
                fn);
            auto *fallbackBlock = llvm::BasicBlock::Create(
                *context,
                "pc_" + std::to_string(node.pc) + "_list_replace_fallback",
                fn);
            auto *continueBlock = llvm::BasicBlock::Create(
                *context,
                "pc_" + std::to_string(node.pc) + "_list_replace_continue",
                fn);
            llvm::Value *headerOk = builder.CreateAnd(index.ok, value.ok, "list_replace_expr_ok");
            headerOk = builder.CreateAnd(headerOk, list.ok, "list_replace_header_ok");
            builder.CreateCondBr(headerOk, haveStorageBlock, fallbackBlock);

            builder.SetInsertPoint(haveStorageBlock);
            llvm::Value *length = builder.CreateLoad(
                i32,
                builder.CreateStructGEP(listStorageTy, list.storage, 1, "list_replace_length_ptr"),
                "list_replace_length");
            llvm::Value *refCount = builder.CreateLoad(
                i32,
                builder.CreateStructGEP(listStorageTy, list.storage, 3, "list_replace_ref_count_ptr"),
                "list_replace_ref_count");
            llvm::Value *numbers = builder.CreateLoad(
                ptr,
                builder.CreateStructGEP(listStorageTy, list.storage, 4, "list_replace_numbers_ptr"),
                "list_replace_numbers");
            llvm::Value *numbersValid = builder.CreateICmpNE(
                builder.CreateLoad(
                    i32,
                    builder.CreateStructGEP(listStorageTy, list.storage, 5, "list_replace_numbers_valid_ptr"),
                    "list_replace_numbers_valid_raw"),
                llvm::ConstantInt::get(i32, 0),
                "list_replace_numbers_valid");
            llvm::Value *lengthNumber = builder.CreateSIToFP(length, f64, "list_replace_length_number");
            llvm::Value *geOne = builder.CreateFCmpOGE(index.value, llvm::ConstantFP::get(f64, 1.0), "list_replace_index_ge_one");
            llvm::Value *leLength = builder.CreateFCmpOLE(index.value, lengthNumber, "list_replace_index_le_length");
            llvm::Value *unique = builder.CreateICmpEQ(refCount, llvm::ConstantInt::get(i32, 1), "list_replace_unique");
            llvm::Value *cacheReady = builder.CreateOr(
                builder.CreateNot(numbersValid),
                builder.CreateNot(isNull(builder, numbers)),
                "list_replace_cache_ready");
            llvm::Value *canCheckIndex = builder.CreateAnd(geOne, leLength, "list_replace_index_range");
            canCheckIndex = builder.CreateAnd(canCheckIndex, unique, "list_replace_unique_range");
            canCheckIndex = builder.CreateAnd(canCheckIndex, cacheReady, "list_replace_cache_range");
            builder.CreateCondBr(canCheckIndex, indexBlock, fallbackBlock);

            builder.SetInsertPoint(indexBlock);
            llvm::Value *zeroBased = builder.CreateSub(
                builder.CreateFPToSI(index.value, i32, "list_replace_one_based_index"),
                llvm::ConstantInt::get(i32, 1),
                "list_replace_zero_based_index");
            llvm::Value *items = builder.CreateLoad(
                ptr,
                builder.CreateStructGEP(listStorageTy, list.storage, 0, "list_replace_items_ptr"),
                "list_replace_items");
            builder.CreateCondBr(builder.CreateNot(isNull(builder, items), "list_replace_items_ok"), haveItemsBlock, fallbackBlock);

            builder.SetInsertPoint(haveItemsBlock);
            llvm::Value *itemPtr = builder.CreateInBoundsGEP(svalueTy, items, zeroBased, "list_replace_item_ptr");
            llvm::Value *oldTag = builder.CreateLoad(
                i32,
                builder.CreateStructGEP(svalueTy, itemPtr, 0, "list_replace_old_tag_ptr"),
                "list_replace_old_tag");
            llvm::Value *oldIsNumber = builder.CreateICmpEQ(
                oldTag,
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER),
                "list_replace_old_is_number");
            builder.CreateCondBr(oldIsNumber, directBlock, fallbackBlock);

            builder.SetInsertPoint(directBlock);
            builder.CreateStore(
                llvm::ConstantInt::get(i32, SJIT_VALUE_NUMBER),
                builder.CreateStructGEP(svalueTy, itemPtr, 0, "list_replace_item_tag_ptr"));
            builder.CreateStore(value.value, builder.CreateStructGEP(svalueTy, itemPtr, 1, "list_replace_item_number_ptr"));
            builder.CreateStore(
                llvm::ConstantPointerNull::get(ptr),
                builder.CreateStructGEP(svalueTy, itemPtr, 2, "list_replace_item_ptr_ptr"));
            builder.CreateCondBr(numbersValid, cacheBlock, continueBlock);

            builder.SetInsertPoint(cacheBlock);
            builder.CreateStore(
                value.value,
                builder.CreateInBoundsGEP(f64, numbers, zeroBased, "list_replace_number_cache_ptr"));
            builder.CreateBr(continueBlock);

            builder.SetInsertPoint(fallbackBlock);
            builder.CreateCall(statementListReplace, {runtime, scriptData, statementPtr});
            builder.CreateBr(continueBlock);

            builder.SetInsertPoint(continueBlock);
            return true;
        };
        auto emitSimpleStatement = [&]() -> llvm::Value * {
            if (!statement) {
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            }
            switch (statement->opcode) {
            case SJIT_STMT_NOOP:
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            case SJIT_STMT_RESET_TIMER:
                builder.CreateCall(resetTimer, {runtime});
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            case SJIT_STMT_STOP_THIS_SCRIPT:
                return llvm::ConstantInt::get(i32, SJIT_STATUS_DONE);
            case SJIT_STMT_BROADCAST:
            case SJIT_STMT_WAIT:
            case SJIT_STMT_WAIT_UNTIL:
            case SJIT_STMT_STOP_OTHER_SCRIPTS:
            case SJIT_STMT_STOP_ALL:
            case SJIT_STMT_LOOKS_SAY_FOR_SECS:
            case SJIT_STMT_LOOKS_SWITCH_BACKDROP:
                return builder.CreateCall(genericStatement, {runtime, thread, frame, scriptData, statementPtr});
            case SJIT_STMT_SET_VARIABLE:
                if (!emitNumericVariableFastPath(false, statementSetVariableAction) &&
                    !emitBoolVariableFastPath(statementSetVariableAction)) {
                    builder.CreateCall(statementSetVariableAction, {runtime, scriptData, statementPtr});
                }
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            case SJIT_STMT_CHANGE_VARIABLE:
                if (!emitNumericVariableFastPath(true, statementChangeVariableAction)) {
                    builder.CreateCall(statementChangeVariableAction, {runtime, scriptData, statementPtr});
                }
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            case SJIT_STMT_LIST_ADD:
                if (!emitListAddNumberFastPath()) {
                    builder.CreateCall(statementListAdd, {runtime, scriptData, statementPtr});
                }
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            case SJIT_STMT_LIST_DELETE:
                builder.CreateCall(statementListDelete, {runtime, scriptData, statementPtr});
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            case SJIT_STMT_LIST_DELETE_ALL:
                builder.CreateCall(statementListDeleteAll, {runtime, scriptData, statementPtr});
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            case SJIT_STMT_LIST_INSERT:
                builder.CreateCall(statementListInsert, {runtime, scriptData, statementPtr});
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            case SJIT_STMT_LIST_REPLACE:
                if (!emitListReplaceNumberFastPath()) {
                    builder.CreateCall(statementListReplace, {runtime, scriptData, statementPtr});
                }
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            case SJIT_STMT_SAY:
                builder.CreateCall(statementSay, {runtime, scriptData, statementPtr});
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            case SJIT_STMT_PEN_CLEAR:
                builder.CreateCall(penClear, {runtime});
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            case SJIT_STMT_PEN_DOWN:
            case SJIT_STMT_PEN_UP:
            case SJIT_STMT_LOOKS_SHOW:
            case SJIT_STMT_LOOKS_HIDE: {
                llvm::Value *sprite = statementSprite();
                if (statement->opcode == SJIT_STMT_PEN_DOWN) {
                    builder.CreateCall(penDown, {runtime, sprite});
                } else if (statement->opcode == SJIT_STMT_PEN_UP) {
                    builder.CreateCall(penUp, {runtime, sprite});
                } else if (statement->opcode == SJIT_STMT_LOOKS_SHOW) {
                    builder.CreateCall(looksShow, {runtime, sprite});
                } else {
                    builder.CreateCall(looksHide, {runtime, sprite});
                }
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            }
            case SJIT_STMT_MONITOR_SHOW:
            case SJIT_STMT_MONITOR_HIDE:
                builder.CreateCall(
                    statementMonitorVisibility,
                    {runtime, scriptData, statementPtr});
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            case SJIT_STMT_PEN_SET_SIZE: {
                llvm::Value *sprite = statementSprite();
                llvm::Value *size = evalStatementNumberSlot(SJIT_STMT_EXPR_VALUE, "pen_size");
                builder.CreateCall(penSetSizeNumber, {runtime, sprite, size});
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            }
            case SJIT_STMT_PEN_SET_COLOR:
                builder.CreateCall(statementPenSetColor, {runtime, scriptData, statementPtr});
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            case SJIT_STMT_PEN_CHANGE_COLOR_PARAM:
                builder.CreateCall(statementPenChangeColorParam, {runtime, scriptData, statementPtr});
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            case SJIT_STMT_MOTION_GOTO_XY: {
                llvm::Value *sprite = statementSprite();
                llvm::Value *x = evalStatementNumberSlot(SJIT_STMT_EXPR_VALUE, "x");
                llvm::Value *y = evalStatementNumberSlot(SJIT_STMT_EXPR_INDEX, "y");
                builder.CreateCall(spriteSetXy, {runtime, sprite, x, y, llvm::ConstantInt::get(i32, 0)});
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            }
            case SJIT_STMT_MOTION_SET_X: {
                llvm::Value *sprite = statementSprite();
                llvm::Value *x = evalStatementNumberSlot(SJIT_STMT_EXPR_VALUE, "x");
                llvm::Value *y = builder.CreateCall(spriteY, {sprite}, "current_y");
                builder.CreateCall(spriteSetXy, {runtime, sprite, x, y, llvm::ConstantInt::get(i32, 0)});
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            }
            case SJIT_STMT_MOTION_SET_Y: {
                llvm::Value *sprite = statementSprite();
                llvm::Value *x = builder.CreateCall(spriteX, {sprite}, "current_x");
                llvm::Value *y = evalStatementNumberSlot(SJIT_STMT_EXPR_VALUE, "y");
                builder.CreateCall(spriteSetXy, {runtime, sprite, x, y, llvm::ConstantInt::get(i32, 0)});
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            }
            case SJIT_STMT_MOTION_CHANGE_X: {
                llvm::Value *sprite = statementSprite();
                llvm::Value *currentX = builder.CreateCall(spriteX, {sprite}, "current_x");
                llvm::Value *dx = evalStatementNumberSlot(SJIT_STMT_EXPR_VALUE, "dx");
                llvm::Value *x = builder.CreateFAdd(currentX, dx, "x");
                llvm::Value *y = builder.CreateCall(spriteY, {sprite}, "current_y");
                builder.CreateCall(spriteSetXy, {runtime, sprite, x, y, llvm::ConstantInt::get(i32, 0)});
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            }
            case SJIT_STMT_MOTION_CHANGE_Y: {
                llvm::Value *sprite = statementSprite();
                llvm::Value *x = builder.CreateCall(spriteX, {sprite}, "current_x");
                llvm::Value *currentY = builder.CreateCall(spriteY, {sprite}, "current_y");
                llvm::Value *dy = evalStatementNumberSlot(SJIT_STMT_EXPR_VALUE, "dy");
                llvm::Value *y = builder.CreateFAdd(currentY, dy, "y");
                builder.CreateCall(spriteSetXy, {runtime, sprite, x, y, llvm::ConstantInt::get(i32, 0)});
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            }
            case SJIT_STMT_LOOKS_SET_SIZE: {
                llvm::Value *sprite = statementSprite();
                llvm::Value *size = evalStatementNumberSlot(SJIT_STMT_EXPR_VALUE, "size");
                builder.CreateCall(spriteSetSize, {runtime, sprite, size});
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            }
            case SJIT_STMT_LOOKS_SET_EFFECT:
            case SJIT_STMT_LOOKS_CHANGE_EFFECT: {
                llvm::Value *sprite = statementSprite();
                llvm::Value *value = evalStatementNumberSlot(
                    SJIT_STMT_EXPR_VALUE,
                    "looks_effect_value");
                builder.CreateCall(
                    statement->opcode == SJIT_STMT_LOOKS_SET_EFFECT ?
                        looksSetEffectNumber : looksChangeEffectNumber,
                    {
                        runtime,
                        sprite,
                        llvm::ConstantInt::get(
                            i32,
                            statement->looks_effect_cache_valid ?
                                statement->looks_effect : -1),
                        value,
                    });
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            }
            case SJIT_STMT_LOOKS_CLEAR_EFFECTS: {
                llvm::Value *sprite = statementSprite();
                builder.CreateCall(looksClearEffects, {runtime, sprite});
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            }
            case SJIT_STMT_SENSING_SET_DRAG_MODE: {
                llvm::Value *sprite = statementSprite();
                builder.CreateCall(
                    spriteSetDraggable,
                    {sprite, llvm::ConstantInt::get(i32, statement->drag_mode ? 1 : 0)});
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            }
            case SJIT_STMT_PROCEDURE_CALL:
                return emitDirectProcedureStatementCall(statement, statementPtr);
            default:
                return llvm::ConstantInt::get(i32, SJIT_STATUS_OK);
            }
        };

        switch (node.kind) {
        case JitNodeKind::Statement: {
            if (statement) {
                const OpcodeEffects opcodeEffects =
                    sjit_statement_opcode_effects(statement->opcode);
                bool needsFramePc = opcodeEffects.canYield ||
                    opcodeEffects.canCallUnknown ||
                    opcodeEffects.requiresInterpreter;
                if (statement->opcode == SJIT_STMT_PROCEDURE_CALL) {
                    const int procedureIndex = findProcedureIndex(
                        script, statement->procedure_name);
                    if (procedureIndex >= 0 &&
                        !statementsMaySuspendOrCallUnknown(
                            script.procedures[procedureIndex].statements,
                            script.procedures[procedureIndex].statement_count)) {
                        needsFramePc = false;
                    }
                }
                if (needsFramePc) {
                    /* Runtime helpers such as wait inspect frame->pc while
                       establishing their continuation. */
                    storeFramePc(pcConstant(node.pc));
                }
            }
            llvm::Value *status = emitSimpleStatement();
            emitStatusContinuation(node.pc, status, node.pc, node.nextPc);
            break;
        }
        case JitNodeKind::RepeatEntry:
        case JitNodeKind::WhileEntry:
        case JitNodeKind::ForeverEntry:
            builder.CreateStore(llvm::ConstantInt::get(i32, 0), branchCounterSlots[static_cast<size_t>(node.branchCounter)]);
            emitJumpToPc(node.continuePc);
            break;
        case JitNodeKind::RepeatUntilEntry:
        case JitNodeKind::ForEachEntry:
            emitJumpToPc(node.continuePc);
            break;
        case JitNodeKind::RepeatContinue: {
            llvm::Value *times = evalStatementNumberSlot(SJIT_STMT_EXPR_TIMES, "repeat_times");
            auto emitNormalRepeatContinue = [&]() {
                llvm::Value *enter = builder.CreateCall(repeatShouldEnterNumber, {frame, statementPtr, times}, "repeat_enter");
                auto *repeatBody = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_repeat_body", fn);
                auto *repeatDone = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_repeat_done", fn);
                builder.CreateCondBr(
                    builder.CreateICmpNE(enter, llvm::ConstantInt::get(i32, 0)),
                    repeatBody,
                    repeatDone);

                builder.SetInsertPoint(repeatBody);
                llvm::Value *state = builder.CreateCall(controlLoopState, {frame, statementPtr, llvm::ConstantInt::get(i32, 0)}, "repeat_state");
                setLoopBranchActive(state);
                emitJumpToPc(node.bodyPc);

                builder.SetInsertPoint(repeatDone);
                builder.CreateCall(controlLoopReset, {frame, statementPtr});
                emitJumpToPc(node.nextPc);
            };

            const SStatement *bulkChange = bulkRepeatChangeCandidate(statement);
            if (bulkChange) {
                auto *bulkCheck = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_repeat_bulk_check", fn);
                auto *bulkRemainingCheck = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_repeat_bulk_remaining_check", fn);
                auto *bulkValue = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_repeat_bulk_value", fn);
                auto *bulkConsume = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_repeat_bulk_consume", fn);
                auto *bulkApplyCheck = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_repeat_bulk_apply_check", fn);
                auto *bulkApply = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_repeat_bulk_apply", fn);
                auto *bulkDone = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_repeat_bulk_done", fn);
                auto *normalRepeat = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_repeat_normal", fn);
                llvm::Value *warp = builder.CreateLoad(i32, frameWarpModePtr(), "repeat_frame_warp_mode");
                llvm::Value *warpActive = builder.CreateICmpNE(warp, llvm::ConstantInt::get(i32, 0), "repeat_warp_active");
                llvm::Value *turbo = runtimeIntField(20, "repeat_runtime_turbo_mode");
                llvm::Value *turboActive = builder.CreateICmpNE(turbo, llvm::ConstantInt::get(i32, 0), "repeat_turbo_active");
                llvm::Value *canTryBulk = builder.CreateOr(warpActive, turboActive, "repeat_bulk_mode");
                builder.CreateCondBr(canTryBulk, bulkCheck, normalRepeat);

                builder.SetInsertPoint(bulkCheck);
                llvm::Value *remaining = builder.CreateCall(
                    repeatRemainingNumber,
                    {frame, statementPtr, times},
                    "repeat_bulk_remaining");
                llvm::Value *canBulk = builder.CreateFCmpOGE(
                    remaining,
                    llvm::ConstantFP::get(f64, 0.0),
                    "repeat_bulk_available");
                builder.CreateCondBr(canBulk, bulkRemainingCheck, normalRepeat);

                builder.SetInsertPoint(bulkRemainingCheck);
                llvm::Value *hasIterations = builder.CreateFCmpOGT(
                    remaining,
                    llvm::ConstantFP::get(f64, 0.0),
                    "repeat_bulk_has_iterations");
                builder.CreateCondBr(hasIterations, bulkValue, bulkDone);

                builder.SetInsertPoint(bulkValue);
                NumberLowering delta = emitNumberExpr(bulkChange->value);
                if (!delta.supported) {
                    builder.CreateBr(normalRepeat);
                } else {
                    builder.CreateCondBr(delta.ok, bulkConsume, normalRepeat);
                }

                builder.SetInsertPoint(bulkConsume);
                llvm::Value *taken = builder.CreateCall(
                    repeatTakeAllNumber,
                    {frame, statementPtr, times},
                    "repeat_bulk_taken");
                llvm::Value *takeOk = builder.CreateFCmpOGE(
                    taken,
                    llvm::ConstantFP::get(f64, 0.0),
                    "repeat_bulk_take_ok");
                builder.CreateCondBr(takeOk, bulkApplyCheck, normalRepeat);

                builder.SetInsertPoint(bulkApplyCheck);
                llvm::Value *takenHasIterations = builder.CreateFCmpOGT(
                    taken,
                    llvm::ConstantFP::get(f64, 0.0),
                    "repeat_bulk_taken_has_iterations");
                builder.CreateCondBr(takenHasIterations, bulkApply, bulkDone);

                builder.SetInsertPoint(bulkApply);
                llvm::Value *totalDelta = builder.CreateFMul(taken, delta.value, "repeat_bulk_delta");
                builder.CreateCall(
                    statementChangeVariableNumber,
                    {runtime, scriptData, constantPointer(bulkChange), totalDelta});
                builder.CreateBr(bulkDone);

                builder.SetInsertPoint(bulkDone);
                builder.CreateCall(controlLoopReset, {frame, statementPtr});
                emitJumpToPc(node.nextPc);

                builder.SetInsertPoint(normalRepeat);
                emitNormalRepeatContinue();
                break;
            }

            emitNormalRepeatContinue();
            break;
        }
        case JitNodeKind::RepeatAfter:
        case JitNodeKind::WhileAfter: {
            llvm::AllocaInst *counterSlot = branchCounterSlots[static_cast<size_t>(node.branchCounter)];
            llvm::Value *count = builder.CreateLoad(i32, counterSlot, "branch_count");
            llvm::Value *nextCount = builder.CreateAdd(count, llvm::ConstantInt::get(i32, 1), "next_branch_count");
            builder.CreateStore(nextCount, counterSlot);
            llvm::Value *state = builder.CreateCall(controlLoopState, {frame, statementPtr, llvm::ConstantInt::get(i32, 0)}, "loop_state");
            llvm::Value *status = builder.CreateCall(
                finishBatchedLoopBranch,
                {runtime, frame, state, nextCount},
                "loop_status");
            emitStatusContinuation(node.pc, status, node.controlPc, node.continuePc);
            break;
        }
        case JitNodeKind::RepeatUntilContinue:
        case JitNodeKind::WhileContinue: {
            llvm::Value *state = builder.CreateCall(controlLoopState, {frame, statementPtr, llvm::ConstantInt::get(i32, 1)}, "control_state");
            auto *stateNull = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_state_null", fn);
            auto *haveState = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_state", fn);
            builder.CreateCondBr(isNull(builder, state), stateNull, haveState);

            builder.SetInsertPoint(stateNull);
            emitJumpToPc(node.nextPc);

            builder.SetInsertPoint(haveState);
            llvm::Value *active = builder.CreateLoad(i32, loopStateBranchActivePtr(state), "branch_active");
            auto *branchBody = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_body", fn);
            auto *checkCondition = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_condition", fn);
            builder.CreateCondBr(
                builder.CreateICmpNE(active, llvm::ConstantInt::get(i32, 0)),
                branchBody,
                checkCondition);

            builder.SetInsertPoint(checkCondition);
            llvm::Value *truthy = evalStatementBoolSlot(SJIT_STMT_EXPR_CONDITION, "loop_condition");
            auto *enterBody = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_enter_body", fn);
            auto *loopDone = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_done", fn);
            if (node.kind == JitNodeKind::RepeatUntilContinue) {
                builder.CreateCondBr(truthy, loopDone, enterBody);
            } else {
                builder.CreateCondBr(truthy, enterBody, loopDone);
            }

            builder.SetInsertPoint(enterBody);
            setLoopBranchActive(state);
            builder.CreateBr(branchBody);

            builder.SetInsertPoint(branchBody);
            emitJumpToPc(node.bodyPc);

            builder.SetInsertPoint(loopDone);
            builder.CreateCall(controlLoopReset, {frame, statementPtr});
            emitJumpToPc(node.nextPc);
            break;
        }
        case JitNodeKind::RepeatUntilAfter:
        case JitNodeKind::ForEachAfter: {
            llvm::Value *state = builder.CreateCall(controlLoopState, {frame, statementPtr, llvm::ConstantInt::get(i32, 0)}, "loop_state");
            llvm::Value *status = builder.CreateCall(finishControlBranch, {frame, state}, "loop_status");
            emitStatusContinuation(node.pc, status, node.controlPc, node.continuePc);
            break;
        }
        case JitNodeKind::ForeverContinue: {
            llvm::Value *state = builder.CreateCall(controlLoopState, {frame, statementPtr, llvm::ConstantInt::get(i32, 1)}, "forever_state");
            auto *stateNull = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_state_null", fn);
            auto *haveState = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_state", fn);
            builder.CreateCondBr(isNull(builder, state), stateNull, haveState);

            builder.SetInsertPoint(stateNull);
            emitStatusContinuation(
                node.pc,
                llvm::ConstantInt::get(i32, SJIT_STATUS_YIELDED),
                node.controlPc,
                node.continuePc);

            builder.SetInsertPoint(haveState);
            llvm::Value *active = builder.CreateLoad(i32, loopStateBranchActivePtr(state), "branch_active");
            auto *branchBody = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_body", fn);
            auto *activate = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_activate", fn);
            builder.CreateCondBr(
                builder.CreateICmpNE(active, llvm::ConstantInt::get(i32, 0)),
                branchBody,
                activate);

            builder.SetInsertPoint(activate);
            setLoopBranchActive(state);
            builder.CreateBr(branchBody);

            builder.SetInsertPoint(branchBody);
            emitJumpToPc(node.bodyPc);
            break;
        }
        case JitNodeKind::ForeverAfter: {
            llvm::AllocaInst *counterSlot = branchCounterSlots[static_cast<size_t>(node.branchCounter)];
            llvm::Value *count = builder.CreateLoad(i32, counterSlot, "forever_count");
            llvm::Value *nextCount = builder.CreateAdd(count, llvm::ConstantInt::get(i32, 1), "next_forever_count");
            builder.CreateStore(nextCount, counterSlot);
            llvm::Value *state = builder.CreateCall(controlLoopState, {frame, statementPtr, llvm::ConstantInt::get(i32, 0)}, "forever_state");
            llvm::Value *status = builder.CreateCall(finishControlBranch, {frame, state}, "forever_status");
            auto *statusYield = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_yield", fn);
            auto *statusNotOk = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_status", fn);
            auto *returnStatus = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_return", fn);
            auto *statusOk = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_ok", fn);
            llvm::Value *isYield = builder.CreateICmpEQ(status, llvm::ConstantInt::get(i32, SJIT_STATUS_YIELDED));
            llvm::Value *isYieldTick = builder.CreateICmpEQ(status, llvm::ConstantInt::get(i32, SJIT_STATUS_YIELD_TICK));
            llvm::Value *isWaiting = builder.CreateICmpEQ(status, llvm::ConstantInt::get(i32, SJIT_STATUS_WAITING));
            llvm::Value *shouldPause = builder.CreateOr(builder.CreateOr(isYield, isYieldTick), isWaiting);
            builder.CreateCondBr(shouldPause, statusYield, statusNotOk);

            builder.SetInsertPoint(statusYield);
            storeFramePc(pcConstant(node.controlPc));
            builder.CreateRet(status);

            builder.SetInsertPoint(statusNotOk);
            llvm::Value *notOk = builder.CreateICmpNE(status, llvm::ConstantInt::get(i32, SJIT_STATUS_OK));
            builder.CreateCondBr(notOk, returnStatus, statusOk);

            builder.SetInsertPoint(returnStatus);
            builder.CreateRet(status);

            builder.SetInsertPoint(statusOk);
            auto *guardYield = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_guard_yield", fn);
            auto *continueLoop = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_continue", fn);
            builder.CreateCondBr(
                builder.CreateICmpSGE(nextCount, llvm::ConstantInt::get(i32, 1000000)),
                guardYield,
                continueLoop);

            builder.SetInsertPoint(guardYield);
            storeFramePc(pcConstant(node.controlPc));
            builder.CreateRet(llvm::ConstantInt::get(i32, SJIT_STATUS_YIELDED));

            builder.SetInsertPoint(continueLoop);
            emitJumpToPc(node.continuePc);
            break;
        }
        case JitNodeKind::ForEachContinue: {
            llvm::Value *state = builder.CreateCall(controlLoopState, {frame, statementPtr, llvm::ConstantInt::get(i32, 1)}, "for_each_state");
            auto *stateNull = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_state_null", fn);
            auto *haveState = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_state", fn);
            builder.CreateCondBr(isNull(builder, state), stateNull, haveState);

            builder.SetInsertPoint(stateNull);
            emitJumpToPc(node.nextPc);

            builder.SetInsertPoint(haveState);
            llvm::Value *active = builder.CreateLoad(i32, loopStateBranchActivePtr(state), "branch_active");
            auto *branchBody = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_body", fn);
            auto *prepare = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_prepare", fn);
            builder.CreateCondBr(
                builder.CreateICmpNE(active, llvm::ConstantInt::get(i32, 0)),
                branchBody,
                prepare);

            builder.SetInsertPoint(prepare);
            llvm::Value *limit = evalStatementNumberSlot(SJIT_STMT_EXPR_TIMES, "for_each_limit");
            llvm::Value *counterPtr = loopStateCounterPtr(state);
            llvm::Value *counter = builder.CreateLoad(f64, counterPtr, "for_each_counter");
            auto *loopDone = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_done", fn);
            auto *enterBody = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_enter_body", fn);
            builder.CreateCondBr(builder.CreateFCmpOGE(counter, limit), loopDone, enterBody);

            builder.SetInsertPoint(enterBody);
            llvm::Value *nextCounter = builder.CreateFAdd(counter, llvm::ConstantFP::get(f64, 1.0), "for_each_next");
            builder.CreateStore(nextCounter, counterPtr);
            llvm::Value *variable = emitCachedStatementScalarVariable("for_each_variable");
            builder.CreateCall(variableSetNumber, {variable, nextCounter});
            setLoopBranchActive(state);
            builder.CreateBr(branchBody);

            builder.SetInsertPoint(branchBody);
            emitJumpToPc(node.bodyPc);

            builder.SetInsertPoint(loopDone);
            builder.CreateCall(controlLoopReset, {frame, statementPtr});
            emitJumpToPc(node.nextPc);
            break;
        }
        case JitNodeKind::IfEntry: {
            llvm::Value *truthy = evalStatementBoolSlot(SJIT_STMT_EXPR_CONDITION, "if_condition");
            auto *ifBody = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_body", fn);
            auto *ifDone = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_done", fn);
            builder.CreateCondBr(truthy, ifBody, ifDone);

            builder.SetInsertPoint(ifBody);
            emitJumpToPc(node.bodyPc);

            builder.SetInsertPoint(ifDone);
            emitJumpToPc(node.nextPc);
            break;
        }
        case JitNodeKind::IfAfter:
        case JitNodeKind::IfElseAfter:
            emitJumpToPc(node.nextPc);
            break;
        case JitNodeKind::IfElseEntry: {
            llvm::Value *truthy = evalStatementBoolSlot(SJIT_STMT_EXPR_CONDITION, "if_else_condition");
            auto *ifBody = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_body", fn);
            auto *elseBody = llvm::BasicBlock::Create(*context, "pc_" + std::to_string(node.pc) + "_else", fn);
            builder.CreateCondBr(truthy, ifBody, elseBody);

            builder.SetInsertPoint(ifBody);
            emitJumpToPc(node.bodyPc);

            builder.SetInsertPoint(elseBody);
            emitJumpToPc(node.elsePc);
            break;
        }
        }
    }

    builder.SetInsertPoint(done);
    storeFramePc(pcConstant(-1));
    markFrameFinished();
    builder.CreateRet(llvm::ConstantInt::get(i32, SJIT_STATUS_DONE));

    for (int i = 0; i < script.procedure_count; ++i) {
        emitProcedureFunction(i);
    }

    /* Repeating these comparatively large dispatch helpers at dozens of call
       sites makes O3 and machine-code emission dominate startup for renderers
       such as 3D.sb3.  Outlining is semantics-neutral and keeps the hot leaf
       available without multiplying its control-flow graph.  Small modules
       retain the existing inline policy. */
    constexpr unsigned largeHelperOutlineUseThreshold = 16;
    const bool outlineListNumber =
        std::getenv("SJIT_DISABLE_INLINE_PROCEDURE_LIST_NUMBER") != nullptr ||
        fastListNumber->getNumUses() > largeHelperOutlineUseThreshold;
    fastListNumber->addFnAttr(
        outlineListNumber ? llvm::Attribute::NoInline : llvm::Attribute::InlineHint);

    constexpr unsigned cachedCompareInlineUseThreshold = 8;
    const bool outlineCachedCompare =
        std::getenv("SJIT_DISABLE_CACHED_COMPARE_INLINE") != nullptr ||
        fastCachedValueCompare->getNumUses() > largeHelperOutlineUseThreshold;
    if (outlineCachedCompare) {
        fastCachedValueCompare->addFnAttr(llvm::Attribute::NoInline);
    } else if (fastCachedValueCompare->getNumUses() >= cachedCompareInlineUseThreshold) {
        fastCachedValueCompare->addFnAttr(llvm::Attribute::AlwaysInline);
    }

    /* Helpers are emitted while lowering so hot paths can select them locally.
       A project can contain many small hat-script modules which do not end up
       calling most helpers.  Drop those definitions before ORC runs the O3
       pipeline; otherwise LLVM spends time optimizing code that GlobalDCE
       would only discard near the end of the pipeline. */
    eraseUnusedInternalFunctions(*module);
    linkRuntimeHelpersIntoScript(*module, dataLayout);
    verifyModuleOrThrow(*module, "verify script module");
    return {std::move(context), std::move(module)};
}

} // namespace

namespace sjit {

struct JitEngine::Impl {
    struct ScriptBinding {
        SCompiledScript *script = nullptr;
        SScriptEntryFn scriptEntry = nullptr;
        std::vector<SProcedureEntryFn> procedureEntries;
    };

    std::unique_ptr<llvm::orc::LLJIT> jit;
    bool runtimeBitcodeLoaded = false;
    std::string lastFallbackReason;
    std::vector<ScriptBinding> scriptBindings;

    void rememberEntries(
        SCompiledScript &script,
        SScriptEntryFn scriptEntry) {
        ScriptBinding *binding = nullptr;
        for (ScriptBinding &candidate : scriptBindings) {
            if (candidate.script == &script) {
                binding = &candidate;
                break;
            }
        }
        if (!binding) {
            scriptBindings.push_back({});
            binding = &scriptBindings.back();
            binding->script = &script;
        }
        binding->scriptEntry = scriptEntry;
        binding->procedureEntries.resize(static_cast<size_t>(script.procedure_count));
        for (int i = 0; i < script.procedure_count; ++i) {
            binding->procedureEntries[static_cast<size_t>(i)] =
                script.procedures[i].jit_entry;
        }
    }

    bool hasExactBinding(
        const SCompiledScript &script,
        SScriptEntryFn entry) const {
        for (const ScriptBinding &binding : scriptBindings) {
            if (binding.script != &script ||
                binding.scriptEntry != entry ||
                binding.procedureEntries.size() !=
                    static_cast<size_t>(std::max(0, script.procedure_count))) {
                continue;
            }
            bool proceduresMatch = true;
            for (int i = 0; i < script.procedure_count; ++i) {
                if (!script.procedures ||
                    binding.procedureEntries[static_cast<size_t>(i)] !=
                        script.procedures[i].jit_entry) {
                    proceduresMatch = false;
                    break;
                }
            }
            if (proceduresMatch) {
                return true;
            }
        }
        return false;
    }
};

JitEngine::JitEngine() : impl_(std::make_unique<Impl>()) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    impl_->jit = createOptimizedLlJit("create LLJIT");
    impl_->jit->getIRTransformLayer().setTransform(
        [](llvm::orc::ThreadSafeModule module, llvm::orc::MaterializationResponsibility &) -> llvm::Expected<llvm::orc::ThreadSafeModule> {
            module.withModuleDo([](llvm::Module &llvmModule) {
                optimizeModule(llvmModule);
            });
            return std::move(module);
        });
    auto processSymbols = unwrapExpected(
        llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            impl_->jit->getDataLayout().getGlobalPrefix()),
        "create current process symbol generator");
    impl_->jit->getMainJITDylib().addGenerator(std::move(processSymbols));

    defineHostThreadPoolSymbols(*impl_->jit);
    impl_->runtimeBitcodeLoaded = addRuntimeCodeIfAvailable(*impl_->jit);
    if (impl_->runtimeBitcodeLoaded) {
        return;
    }

    llvm::orc::MangleAndInterner mangle(impl_->jit->getExecutionSession(), impl_->jit->getDataLayout());
    checkError(
        impl_->jit->getMainJITDylib().define(llvm::orc::absoluteSymbols({
            {mangle("sjit_script_interpreter_entry"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_script_interpreter_entry), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_script_execute_statement_with_frame"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_script_execute_statement_with_frame), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_script_execute_procedure_statement"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_script_execute_procedure_statement), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_script_eval_statement_expr"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_script_eval_statement_expr), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_script_eval_statement_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_script_eval_statement_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_script_value_ptr_to_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_script_value_ptr_to_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_script_destroy_value_ptr"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_script_destroy_value_ptr), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_thread_script_data"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_thread_script_data), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_frame_pc"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_frame_pc), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_frame_set_pc"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_frame_set_pc), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_frame_mark_finished"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_frame_mark_finished), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_sprite_x"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_sprite_x), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_sprite_y"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_sprite_y), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_sprite_set_size"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_sprite_set_size), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_reset_timer"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_reset_timer), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_scalar_variable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_scalar_variable), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_scalar_variable_ptr"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_scalar_variable_ptr), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_variable_ptr"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_variable_ptr), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_expr_scalar_variable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_expr_scalar_variable), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_expr_list_variable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_expr_list_variable), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_variable_is_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_variable_is_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_variable_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_variable_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_variable_value"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_variable_value), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_variable_truthy"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_variable_truthy), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_variable_set_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_variable_set_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_variable_set_string_borrowed"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_variable_set_string_borrowed), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_variable_change_by_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_variable_change_by_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_expr_variable_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_expr_variable_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_expr_list_length_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_expr_list_length_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_expr_list_item_number_at"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_expr_list_item_number_at), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_expr_list_contains_literal"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_expr_list_contains_literal), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_list_variable_length_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_list_variable_length_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_list_variable_item_number_at"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_list_variable_item_number_at), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_list_variable_item_number_at_variable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_list_variable_item_number_at_variable), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_list_variable_item_number_at_argument"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_list_variable_item_number_at_argument), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_list_variable_item_value"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_list_variable_item_value), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_list_variable_item_value_at_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_list_variable_item_value_at_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_list_variable_item_number_value"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_list_variable_item_number_value), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_list_variable_contains_value"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_list_variable_contains_value), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_days_since_2000"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_days_since_2000), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_round_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_round_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_random_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_random_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_mathop_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_mathop_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_value_make_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_value_make_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_value_make_bool"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_value_make_bool), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_expr_literal_value"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_expr_literal_value), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_variable_argument_value"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_variable_argument_value), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_procedure_argument_copy"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_procedure_argument_copy), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_expr_list_item_argument_literal"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_expr_list_item_argument_literal), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_expr_list_item_argument_at_variable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_expr_list_item_argument_at_variable), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_procedure_argument_value_at"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_procedure_argument_value_at), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_value_join_ptr"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_value_join_ptr), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_expr_variable_value"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_expr_variable_value), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_expr_list_item_value"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_expr_list_item_value), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_expr_list_item_number_value"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_expr_list_item_number_value), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_expr_list_contains_value"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_expr_list_contains_value), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_value_length_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_value_length_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_value_letter_of"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_value_letter_of), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_key_pressed_value"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_key_pressed_value), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_value_truthy"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_value_truthy), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_value_compare"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_value_compare), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_op_contains_value_ptr"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_op_contains_value_ptr), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_pen_set_color_value_and_change_param_number_ptr"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_pen_set_color_value_and_change_param_number_ptr), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_expr_list_item_number_at_variable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_expr_list_item_number_at_variable), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_expr_list_item_number_at_argument"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_expr_list_item_number_at_argument), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_expr_list_item_compare_literal_at_variable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_expr_list_item_compare_literal_at_variable), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_pen_set_color_list_item_and_change_param_number_at_variable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_pen_set_color_list_item_and_change_param_number_at_variable), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_replace_literal_at_variable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_replace_literal_at_variable), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_replace_number_at_variable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_replace_number_at_variable), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_replace_list_item_at_variables"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_replace_list_item_at_variables), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_replace_list_item_at_variable_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_replace_list_item_at_variable_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_pen_render_list_pixel_at_variable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_pen_render_list_pixel_at_variable), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_pen_render_list_pixel_from_variables"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_pen_render_list_pixel_from_variables), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_pen_render_row_from_variables"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_pen_render_row_from_variables), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_script_set_variable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_script_set_variable), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_script_change_variable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_script_change_variable), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_script_list_add"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_script_list_add), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_script_list_delete"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_script_list_delete), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_script_list_delete_all"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_script_list_delete_all), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_script_list_insert"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_script_list_insert), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_script_list_replace"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_script_list_replace), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_script_say"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_script_say), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_script_pen_set_color"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_script_pen_set_color), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_script_pen_change_color_param"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_script_pen_change_color_param), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_bool"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_bool), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_set_variable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_set_variable), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_change_variable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_change_variable), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_set_monitor_visibility"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_set_monitor_visibility), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_set_variable_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_set_variable_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_variable_set_from_variable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_variable_set_from_variable), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_variable_set_from_argument"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_variable_set_from_argument), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_variable_set_from_literal"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_variable_set_from_literal), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_variable_set_from_list_item_literal"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_variable_set_from_list_item_literal), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_variable_set_from_list_item_at_variable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_variable_set_from_list_item_at_variable), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_variable_set_from_list_item_at_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_variable_set_from_list_item_at_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_variable_set_from_list_item_at_argument"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_variable_set_from_list_item_at_argument), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_change_variable_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_change_variable_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_add"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_add), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_delete"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_delete), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_delete_all"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_delete_all), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_insert"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_insert), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_replace"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_replace), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_add_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_add_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_add_from_variable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_add_from_variable), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_add_from_argument"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_add_from_argument), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_add_from_literal"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_add_from_literal), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_add_list_item_at_variable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_add_list_item_at_variable), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_add_literal_repeated"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_add_literal_repeated), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_delete_at_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_delete_at_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_insert_number_at"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_insert_number_at), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_replace_number_at"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_replace_number_at), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_list_variable_add_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_list_variable_add_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_list_variable_clear"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_list_variable_clear), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_list_variable_replace_number_at"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_list_variable_replace_number_at), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_list_variable_replace_number_at_variable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_list_variable_replace_number_at_variable), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_list_variable_replace_from_variables"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_list_variable_replace_from_variables), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_list_variable_replace_list_item_at_variables"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_list_variable_replace_list_item_at_variables), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_set_variable_value_ptr"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_set_variable_value_ptr), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_add_value_ptr"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_add_value_ptr), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_delete_index_value_ptr"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_delete_index_value_ptr), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_insert_value_ptr"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_insert_value_ptr), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_list_replace_value_ptr"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_list_replace_value_ptr), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_say_value_ptr"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_say_value_ptr), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_pen_set_color_value_ptr"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_pen_set_color_value_ptr), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_pen_change_color_param_value_ptr"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_pen_change_color_param_value_ptr), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_say"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_say), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_pen_set_color"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_pen_set_color), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_statement_pen_change_color_param"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_statement_pen_change_color_param), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_control_loop_state"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_control_loop_state), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_control_loop_reset"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_control_loop_reset), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_procedure_control_loop_state"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_procedure_control_loop_state), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_procedure_control_loop_reset"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_procedure_control_loop_reset), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_procedure_activation_state"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_procedure_activation_state), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_procedure_activation_reset"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_procedure_activation_reset), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_control_loop_reset_from_depth"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_control_loop_reset_from_depth), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_repeat_should_enter_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_repeat_should_enter_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_procedure_repeat_should_enter_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_procedure_repeat_should_enter_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_repeat_remaining_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_repeat_remaining_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_repeat_take_all_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_repeat_take_all_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_round_repeat_count"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_round_repeat_count), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_finish_control_branch"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_finish_control_branch), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_finish_batched_loop_branch"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_finish_batched_loop_branch), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_runtime_get_sprite"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_runtime_get_sprite), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_script_execute_statement_ptr_with_thread"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_script_execute_statement_ptr_with_thread), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_sprite_set_xy"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_sprite_set_xy), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_pen_clear"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_pen_clear), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_pen_down"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_pen_down), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_pen_up"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_pen_up), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_pen_stamp"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_pen_stamp), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_jit_pen_set_size_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_jit_pen_set_size_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_looks_show"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_looks_show), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_looks_hide"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_looks_hide), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_looks_switch_backdrop_value_ptr"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_looks_switch_backdrop_value_ptr), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_looks_set_effect_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_looks_set_effect_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_looks_change_effect_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_looks_change_effect_number), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_looks_clear_effects"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_looks_clear_effects), llvm::JITSymbolFlags::Exported}},
            {mangle("sjit_sprite_set_draggable"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_sprite_set_draggable), llvm::JITSymbolFlags::Exported}},
        })),
        "define JIT bridge symbols");
}

JitEngine::~JitEngine() {
    if (!impl_) {
        return;
    }
    for (Impl::ScriptBinding &binding : impl_->scriptBindings) {
        SCompiledScript *script = binding.script;
        if (!script) {
            continue;
        }
        if (!script->procedures || script->procedure_count <= 0) {
            continue;
        }
        const size_t count = std::min(
            static_cast<size_t>(script->procedure_count),
            binding.procedureEntries.size());
        for (size_t i = 0; i < count; ++i) {
            if (script->procedures[i].jit_entry == binding.procedureEntries[i]) {
                script->procedures[i].jit_entry = nullptr;
            }
        }
    }
}

bool JitEngine::hasRuntimeBitcode() const {
    return impl_->runtimeBitcodeLoaded;
}

SRuntimeTickFn JitEngine::runtimeTick() {
    if (!impl_->runtimeBitcodeLoaded) {
        return nullptr;
    }
    auto symbol = unwrapExpected(impl_->jit->lookup("sjit_runtime_tick"), "lookup runtime tick");
    return symbol.toPtr<SRuntimeTickFn>();
}

SRuntimeVoidFn JitEngine::runtimeGreenFlag() {
    if (!impl_->runtimeBitcodeLoaded) {
        return nullptr;
    }
    auto symbol = unwrapExpected(impl_->jit->lookup("sjit_runtime_green_flag"), "lookup runtime green flag");
    return symbol.toPtr<SRuntimeVoidFn>();
}

SRuntimeVoidFn JitEngine::runtimeStopAll() {
    if (!impl_->runtimeBitcodeLoaded) {
        return nullptr;
    }
    auto symbol = unwrapExpected(impl_->jit->lookup("sjit_runtime_stop_all"), "lookup runtime stop all");
    return symbol.toPtr<SRuntimeVoidFn>();
}

SRuntimeThreadQueryFn JitEngine::runtimeHasThreads() {
    if (!impl_->runtimeBitcodeLoaded) {
        return nullptr;
    }
    auto symbol = unwrapExpected(impl_->jit->lookup("sjit_runtime_has_threads"), "lookup runtime has threads");
    return symbol.toPtr<SRuntimeThreadQueryFn>();
}

SRuntimeThreadQueryFn JitEngine::runtimeThreadCount() {
    if (!impl_->runtimeBitcodeLoaded) {
        return nullptr;
    }
    auto symbol = unwrapExpected(impl_->jit->lookup("sjit_runtime_thread_count"), "lookup runtime thread count");
    return symbol.toPtr<SRuntimeThreadQueryFn>();
}

SRuntimePenPathDataFn JitEngine::runtimePenPathData() {
    if (!impl_->runtimeBitcodeLoaded) {
        return nullptr;
    }
    auto symbol = unwrapExpected(impl_->jit->lookup("sjit_runtime_pen_path_data"), "lookup runtime pen path data");
    return symbol.toPtr<SRuntimePenPathDataFn>();
}

SRuntimeThreadQueryFn JitEngine::runtimePenPathCount() {
    if (!impl_->runtimeBitcodeLoaded) {
        return nullptr;
    }
    auto symbol = unwrapExpected(impl_->jit->lookup("sjit_runtime_pen_path_count"), "lookup runtime pen path count");
    return symbol.toPtr<SRuntimeThreadQueryFn>();
}

SRuntimeThreadQueryFn JitEngine::runtimePenPathRevision() {
    if (!impl_->runtimeBitcodeLoaded) {
        return nullptr;
    }
    auto symbol = unwrapExpected(impl_->jit->lookup("sjit_runtime_pen_path_revision"), "lookup runtime pen path revision");
    return symbol.toPtr<SRuntimeThreadQueryFn>();
}

SScriptEntryFn JitEngine::compileScript(
    const SCompiledScript &script,
    const std::string &name,
    SRuntime *runtime) {
    SCompiledScript &mutableScript = const_cast<SCompiledScript &>(script);
    const bool logTiming = std::getenv("SJIT_LOG_JIT_TIMING") != nullptr;
    const auto compileStarted = logTiming ?
        std::chrono::steady_clock::now() :
        std::chrono::steady_clock::time_point{};
    const auto finishCompile = [&](SScriptEntryFn entry, const char *outcome) {
        if (logTiming) {
            const double elapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - compileStarted).count();
            std::fprintf(
                stderr,
                "sjit: compile %s: %.3f ms (%s)\n",
                name.c_str(),
                elapsedMs,
                outcome);
        }
        return entry;
    };
    impl_->lastFallbackReason.clear();
    bool logFallback = std::getenv("SJIT_LOG_JIT_FALLBACK") != nullptr;
#ifndef NDEBUG
    logFallback = true;
#endif
    const bool prebindVariables =
        runtime && std::getenv("SJIT_DISABLE_VARIABLE_PREBIND") == nullptr;
    mutableScript.jit_runtime_instance_id = prebindVariables ? runtime->instance_id : 0;
    if (!unsafeRawRuntimeObjectConstantsEnabled()) {
        mutableScript.bound_target = nullptr;
    }
    for (int i = 0; i < mutableScript.procedure_count; ++i) {
        mutableScript.procedures[i].jit_entry = nullptr;
    }
    if (const std::string reason = scriptInterpreterFallbackReason(script); !reason.empty()) {
        impl_->lastFallbackReason = reason;
        if (logFallback) {
            std::fprintf(
                stderr,
                "sjit: %s uses interpreter fallback: %s\n",
                name.c_str(),
                reason.c_str());
        }
        return finishCompile(
            sjit_script_interpreter_entry,
            "interpreter eligibility fallback");
    }
    if (prebindVariables) {
        prebindScriptVariables(runtime, const_cast<SCompiledScript &>(script));
    }
    ModuleBuild built = createScriptModule(impl_->jit->getDataLayout(), script, name);
    auto threadSafeModule = llvm::orc::ThreadSafeModule(std::move(built.module), std::move(built.context));
    checkError(impl_->jit->addIRModule(std::move(threadSafeModule)), "add script IR module");

    std::vector<bool> directProcedures = findJitProcedureEligibility(script);
    if (logFallback) {
        std::vector<bool> reachableProcedures;
        if (std::getenv("SJIT_DISABLE_PROCEDURE_REACHABILITY") == nullptr) {
            reachableProcedures.assign(
                static_cast<size_t>(std::max(0, script.procedure_count)),
                false);
            collectReachableProcedures(
                script,
                script.statements,
                script.statement_count,
                reachableProcedures);
        }
        for (int i = 0; i < mutableScript.procedure_count; ++i) {
            if (directProcedures[static_cast<size_t>(i)]) {
                continue;
            }
            if (!reachableProcedures.empty() &&
                !reachableProcedures[static_cast<size_t>(i)]) {
                std::fprintf(
                    stderr,
                    "sjit: %s procedure %d native body omitted: "
                    "unreachable from this script entry\n",
                    name.c_str(),
                    i);
                continue;
            }
            std::string reason = procedureInterpreterFallbackReason(script, i);
            if (reason.empty()) {
                reason = "procedure does not satisfy native eligibility constraints";
            }
            std::fprintf(
                stderr,
                "sjit: %s procedure %d uses generic/interpreter fallback: %s\n",
                name.c_str(),
                i,
                reason.c_str());
        }
    }
    for (int i = 0; i < mutableScript.procedure_count; ++i) {
        if (!directProcedures[static_cast<size_t>(i)]) {
            continue;
        }
        const std::string procedureName = name + "_procedure_" + std::to_string(i);
        auto procedureSymbol = unwrapExpected(impl_->jit->lookup(procedureName), "lookup procedure symbol");
        mutableScript.procedures[i].jit_entry = procedureSymbol.toPtr<SProcedureEntryFn>();
    }
    auto symbol = unwrapExpected(impl_->jit->lookup(name), "lookup script symbol");
    SScriptEntryFn entry = symbol.toPtr<SScriptEntryFn>();
    impl_->rememberEntries(mutableScript, entry);
    return finishCompile(entry, "native");
}

bool JitEngine::certifyScriptOwnership(
    SRuntime *runtime,
    int scriptId,
    const SCompiledScript &script,
    SScriptEntryFn entry) const {
    if (!impl_ || !runtime || !entry ||
        entry == sjit_script_interpreter_entry ||
        !impl_->hasExactBinding(script, entry)) {
        return false;
    }
    return sjit_runtime_record_attested_script_ownership(
        runtime, scriptId, &script, entry) != 0;
}

const std::string &JitEngine::lastFallbackReason() const {
    return impl_->lastFallbackReason;
}

void JitEngine::emitScriptLl(const SCompiledScript &script, const std::string &name, const std::string &path) {
    if (!unsafeRawRuntimeObjectConstantsEnabled()) {
        const_cast<SCompiledScript &>(script).bound_target = nullptr;
    }
    ModuleBuild built = createScriptModule(impl_->jit->getDataLayout(), script, name);

    std::error_code error;
    llvm::raw_fd_ostream output(path, error, llvm::sys::fs::OF_Text);
    if (error) {
        throw std::runtime_error("open " + path + ": " + error.message());
    }
    built.module->print(output, nullptr);
    output.flush();
}

void JitEngine::emitRuntimeLl(const std::string &path) {
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = loadRuntimeBitcodeModule(*context, impl_->jit->getDataLayout(), true);

    std::error_code error;
    llvm::raw_fd_ostream output(path, error, llvm::sys::fs::OF_Text);
    if (error) {
        throw std::runtime_error("open " + path + ": " + error.message());
    }
    module->print(output, nullptr);
    output.flush();
}

SValue runSmokeJit() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    auto jit = createOptimizedLlJit("create LLJIT");
    llvm::orc::MangleAndInterner mangle(jit->getExecutionSession(), jit->getDataLayout());
    checkError(
        jit->getMainJITDylib().define(llvm::orc::absoluteSymbols({
            {mangle("sjit_make_number"),
             {llvm::orc::ExecutorAddr::fromPtr(&sjit_make_number), llvm::JITSymbolFlags::Exported}},
        })),
        "define runtime symbols");

    SmokeModule smoke = createSmokeModule(jit->getDataLayout());
    auto threadSafeModule = llvm::orc::ThreadSafeModule(std::move(smoke.module), std::move(smoke.context));
    checkError(jit->addIRModule(std::move(threadSafeModule)), "add IR module");

    auto symbol = unwrapExpected(jit->lookup("sjit_jit_smoke"), "lookup smoke symbol");
    auto *compiled = symbol.toPtr<SValue (*)()>();
    return compiled();
}

void emitSmokeLl(const std::string &path) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    auto jit = createOptimizedLlJit("create LLJIT for IR emission");
    SmokeModule smoke = createSmokeModule(jit->getDataLayout());

    std::error_code error;
    llvm::raw_fd_ostream output(path, error, llvm::sys::fs::OF_Text);
    if (error) {
        throw std::runtime_error("open " + path + ": " + error.message());
    }
    smoke.module->print(output, nullptr);
    output.flush();
}

void emitRuntimeLl(const std::string &path) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    auto jit = createOptimizedLlJit("create LLJIT for runtime IR emission");
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = loadRuntimeBitcodeModule(*context, jit->getDataLayout(), true);

    std::error_code error;
    llvm::raw_fd_ostream output(path, error, llvm::sys::fs::OF_Text);
    if (error) {
        throw std::runtime_error("open " + path + ": " + error.message());
    }
    module->print(output, nullptr);
    output.flush();
}

}
