// Standalone policy test; no runtime/LLVM optimization pipeline needed.
#include "../../src/jit/llvm_helper_attributes.hpp"
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <cstdio>
#include <string_view>

int main(int argc, char** argv) {
    // Run in separate processes: the diagnostic flag is frozen on first use.
    const bool enabled = argc == 1 || std::string_view(argv[1]) != "off";
    llvm::LLVMContext context;
    llvm::Module module("helper-attributes", context);
    auto* d = llvm::Type::getDoubleTy(context);
    auto* i = llvm::Type::getInt64Ty(context);
    auto* signature = llvm::FunctionType::get(d, {d}, false);
    unsigned checks = 0;
    auto require = [&](bool condition, const char* reason) {
        ++checks;
        if (!condition) std::fprintf(stderr, "FAIL: %s\n", reason);
        return condition;
    };
    for (const char* name : {"xyo_asin", "xyo_acos", "xyo_atan", "xyo_exp",
                             "xyo_log", "xyo_round", "xyo_to_num", "xyo_mathop",
                             "xyo_gc_epoch", "xyo_yield", "sin", "cos", "sqrt"}) {
        auto callee = module.getOrInsertFunction(name, signature);
        xyo::llvmc::applyHelperAttributes(callee, name);
        auto* f = llvm::cast<llvm::Function>(callee.getCallee());
        const bool expected = enabled &&
            (std::string_view(name) == "xyo_asin" || std::string_view(name) == "xyo_acos");
        if (!require(f->doesNotAccessMemory() == expected, name) ||
            !require(f->hasFnAttribute(llvm::Attribute::NoUnwind) == expected, name) ||
            !require(f->hasFnAttribute(llvm::Attribute::WillReturn) == expected, name))
            return 1;
    }
    // Whitelist names alone must not authorize a different ABI or a definition.
    llvm::Module wrong("wrong-abi", context);
    for (auto* type : {llvm::FunctionType::get(i, {d}, false),
                       llvm::FunctionType::get(d, {i}, false),
                       llvm::FunctionType::get(d, {}, false),
                       llvm::FunctionType::get(d, {d}, true)}) {
        auto* f = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                                         "wrong", wrong);
        xyo::llvmc::applyHelperAttributes(llvm::FunctionCallee(type, f), "xyo_asin");
        if (!require(!f->doesNotAccessMemory(), "wrong signature")) return 1;
    }
    auto* definition = llvm::Function::Create(signature, llvm::GlobalValue::ExternalLinkage,
                                             "definition", wrong);
    llvm::BasicBlock::Create(context, "entry", definition);
    xyo::llvmc::applyHelperAttributes(llvm::FunctionCallee(signature, definition), "xyo_acos");
    if (!require(!definition->doesNotAccessMemory(), "definition")) return 1;
    std::printf("PASS: helper attribute policy (%s), %u checks\n", enabled ? "on" : "off", checks);
}
