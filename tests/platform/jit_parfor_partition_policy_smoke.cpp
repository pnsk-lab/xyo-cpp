// Construct LLVM IR directly to exercise the par-for callback safety proof.
// No Scratch fixture is needed: these cases include IR shapes that Scratch
// codegen deliberately never emits.
#include "../../src/jit/parfor_lazy_partition.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

void require(bool ok, const char* label) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", label);
        std::exit(1);
    }
    std::printf("PASS %s\n", label);
}

struct Fixture {
    llvm::LLVMContext context;
    llvm::Module module{"parfor-lazy-policy", context};
    llvm::Type* ptr = llvm::PointerType::get(context, 0);
    llvm::Type* i64 = llvm::Type::getInt64Ty(context);
    llvm::Type* f64 = llvm::Type::getDoubleTy(context);
    llvm::Function* dispatch;

    Fixture() {
        dispatch = llvm::Function::Create(
            llvm::FunctionType::get(
                llvm::Type::getInt32Ty(context),
                {ptr, i64, f64, f64, ptr, ptr, i64, ptr}, false),
            llvm::GlobalValue::ExternalLinkage, "xyo_par_for", module);
    }

    llvm::Function* body(const char* name,
                         llvm::GlobalValue::LinkageTypes linkage =
                             llvm::GlobalValue::InternalLinkage) {
        auto* f = llvm::Function::Create(
            llvm::FunctionType::get(
                llvm::Type::getVoidTy(context),
                {ptr, i64, f64, ptr, ptr}, false),
            linkage, name, module);
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "body", f));
        b.CreateRetVoid();
        return f;
    }

    llvm::Function* caller(const char* name,
                           const std::vector<llvm::Value*>& callbacks) {
        auto* f = llvm::Function::Create(
            llvm::FunctionType::get(
                llvm::Type::getVoidTy(context), false),
            llvm::GlobalValue::ExternalLinkage, name, module);
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", f));
        for (auto* callback : callbacks) {
            b.CreateCall(dispatch, {
                llvm::Constant::getNullValue(ptr),
                llvm::ConstantInt::get(i64, 8192),
                llvm::ConstantFP::get(f64, 0),
                llvm::ConstantFP::get(f64, 1),
                llvm::Constant::getNullValue(ptr),
                llvm::Constant::getNullValue(ptr),
                llvm::ConstantInt::get(i64, 1),
                callback,
            });
        }
        b.CreateRetVoid();
        return f;
    }

    void verify() {
        require(!llvm::verifyModule(module, &llvm::errs()),
                "valid synthetic LLVM module");
    }
};

void testValidTwoCallbacks() {
    Fixture fixture;
    auto* first = fixture.body("first");
    auto* second = fixture.body("second");
    auto* caller = fixture.caller("script", {first, second});
    fixture.verify();
    require(xyo::supportsLazyParForPartition(fixture.module),
            "two direct private callback references accepted");

    llvm::orc::IRPartitionLayer::GlobalValueSet requested{caller};
    auto combined = xyo::partitionParForCallbacks(requested);
    require(combined.has_value(), "valid partition exists");
    require(combined->size() == 3 && combined->count(caller) &&
                combined->count(first) && combined->count(second),
            "requested caller co-materializes both callback bodies");

    auto* unused = fixture.body("unused");
    requested = {unused};
    auto noCallback = xyo::partitionParForCallbacks(requested);
    require(noCallback && noCallback->size() == 1 &&
                noCallback->count(unused),
            "unrelated lazy function stays in its own partition");
}

void testEscapingCallback() {
    Fixture fixture;
    auto* cb = fixture.body("callback");
    fixture.caller("script", {cb});
    new llvm::GlobalVariable(
        fixture.module, fixture.ptr, true,
        llvm::GlobalValue::InternalLinkage, cb, "escaped_callback");
    fixture.verify();
    require(!xyo::supportsLazyParForPartition(fixture.module),
            "callback address escaping to global rejected");
}

void testExtraCallUse() {
    Fixture fixture;
    auto* cb = fixture.body("callback");
    auto* script = fixture.caller("script", {cb});
    auto* sink = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(fixture.context),
                                {fixture.ptr}, false),
        llvm::GlobalValue::ExternalLinkage, "capture_function", fixture.module);
    auto& bb = script->getEntryBlock();
    llvm::IRBuilder<> b(bb.getTerminator());
    b.CreateCall(sink, {cb});
    fixture.verify();
    require(!xyo::supportsLazyParForPartition(fixture.module),
            "callback passed to another C function rejected");
}

void testSharedCallback() {
    Fixture fixture;
    auto* cb = fixture.body("callback");
    fixture.caller("script_a", {cb});
    fixture.caller("script_b", {cb});
    fixture.verify();
    require(!xyo::supportsLazyParForPartition(fixture.module),
            "callback address used by two distinct callers rejected");
}

void testCallbackReusedAsDataInSameCall() {
    Fixture fixture;
    auto* cb = fixture.body("callback");
    auto* script = fixture.caller("script", {cb});
    auto* call = llvm::cast<llvm::CallBase>(&script->getEntryBlock().front());
    // With opaque pointers, even the JitCtx* argument can carry this
    // function address. Merely checking the User* (same CallBase) is
    // insufficient: the callback body has two separate Uses.
    call->setArgOperand(0, cb);
    fixture.verify();
    require(!xyo::supportsLazyParForPartition(fixture.module),
            "callback reused as another argument to parfor rejected");
}

void testDispatchReusedAsDataInSameCall() {
    Fixture fixture;
    auto* cb = fixture.body("callback");
    auto* script = fixture.caller("script", {cb});
    auto* call = llvm::cast<llvm::CallBase>(&script->getEntryBlock().front());
    call->setArgOperand(0, fixture.dispatch);
    fixture.verify();
    require(!xyo::supportsLazyParForPartition(fixture.module),
            "dispatcher reused as data in same call rejected");
}

void testIndirectCallback() {
    Fixture fixture;
    auto* a = fixture.body("a");
    auto* b = fixture.body("b");
    auto* script = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(fixture.context),
                                {llvm::Type::getInt1Ty(fixture.context)}, false),
        llvm::GlobalValue::ExternalLinkage, "script", fixture.module);
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(fixture.context, "entry", script));
    auto* selected = ir.CreateSelect(script->getArg(0), a, b);
    ir.CreateCall(fixture.dispatch, {
        llvm::Constant::getNullValue(fixture.ptr),
        llvm::ConstantInt::get(fixture.i64, 8192),
        llvm::ConstantFP::get(fixture.f64, 0),
        llvm::ConstantFP::get(fixture.f64, 1),
        llvm::Constant::getNullValue(fixture.ptr),
        llvm::Constant::getNullValue(fixture.ptr),
        llvm::ConstantInt::get(fixture.i64, 1),
        selected,
    });
    ir.CreateRetVoid();
    fixture.verify();
    require(!xyo::supportsLazyParForPartition(fixture.module),
            "phi/select callback rejected");
    auto partition = xyo::partitionParForCallbacks({script});
    require(!partition,
            "unexpected indirect callback requests whole-module compilation");
}

void testNonPrivateCallback() {
    Fixture fixture;
    auto* cb = fixture.body("exported_body",
                            llvm::GlobalValue::ExternalLinkage);
    fixture.caller("script", {cb});
    fixture.verify();
    require(!xyo::supportsLazyParForPartition(fixture.module),
            "externally visible callback rejected");
}

void testDispatchAddressEscape() {
    Fixture fixture;
    auto* cb = fixture.body("callback");
    fixture.caller("script", {cb});
    new llvm::GlobalVariable(
        fixture.module, fixture.ptr, true,
        llvm::GlobalValue::InternalLinkage, fixture.dispatch,
        "escaped_dispatch");
    fixture.verify();
    require(!xyo::supportsLazyParForPartition(fixture.module),
            "par-for dispatcher address escape rejected");
}

void testAbsentDispatch() {
    Fixture fixture;
    fixture.body("unused");
    fixture.verify();
    require(!xyo::supportsLazyParForPartition(fixture.module),
            "no par-for references rejected by opt-in proof");
}

} // namespace

int main() {
    testValidTwoCallbacks();
    testEscapingCallback();
    testExtraCallUse();
    testSharedCallback();
    testCallbackReusedAsDataInSameCall();
    testDispatchReusedAsDataInSameCall();
    testIndirectCallback();
    testNonPrivateCallback();
    testDispatchAddressEscape();
    testAbsentDispatch();
    return 0;
}
