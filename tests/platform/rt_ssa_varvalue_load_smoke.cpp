// RT-001: exact Variable::value i64 address provenance; never GC permission.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/jit/rt_ssa_extra_capture.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <cassert>
#include <cstdio>
using namespace llvm;

int main() {
    LLVMContext ctx;
    Module mod("native.variable.value", ctx);
    IRBuilder<> b(ctx);
    using xyo::rt::NativeVariableValueOrigin;
    using xyo::rt::classifyNativeVariableValueLoad;
    auto* fn = Function::Create(FunctionType::get(b.getVoidTy(),
        {b.getPtrTy()}, false), GlobalValue::ExternalLinkage, "probe", mod);
    b.SetInsertPoint(BasicBlock::Create(ctx, "entry", fn));
    auto* jitCtx = fn->getArg(0);
    constexpr unsigned kSlotOff = 96, kValueOff = 104;
    auto* slotField = b.CreateConstGEP1_32(b.getInt8Ty(), jitCtx, kSlotOff);
    auto* slots = b.CreateLoad(b.getPtrTy(), slotField);
    auto* slotAddr = b.CreateGEP(b.getPtrTy(), slots, b.getInt64(3));
    auto* variable = b.CreateLoad(b.getPtrTy(), slotAddr);
    auto* valField = b.CreateConstGEP1_32(b.getInt8Ty(), variable, kValueOff);
    auto* value = b.CreateLoad(b.getInt64Ty(), valField);
    assert(classifyNativeVariableValueLoad(value, jitCtx, kSlotOff, kValueOff)
           == NativeVariableValueOrigin::SlotArray);
    assert(classifyNativeVariableValueLoad(value, nullptr, kSlotOff, kValueOff)
           == NativeVariableValueOrigin::Unknown);
    assert(classifyNativeVariableValueLoad(value, jitCtx, kSlotOff+1, kValueOff)
           == NativeVariableValueOrigin::Unknown);
    auto* wrongField = b.CreateConstGEP1_32(b.getInt8Ty(), variable, kValueOff+8);
    auto* wrongValue = b.CreateLoad(b.getInt64Ty(), wrongField);
    assert(classifyNativeVariableValueLoad(wrongValue,jitCtx,kSlotOff,kValueOff)
           == NativeVariableValueOrigin::Unknown);
    const auto* sig = FunctionType::get(b.getPtrTy(),
          {b.getPtrTy(), b.getInt64Ty(), b.getPtrTy(),
           b.getPtrTy(), b.getInt32Ty()}, false);
    auto guarded = mod.getOrInsertFunction("xyo_varslot_checked",
                                          const_cast<FunctionType*>(sig));
    auto forged = mod.getOrInsertFunction("other_varslot",
                                         const_cast<FunctionType*>(sig));
    const auto makeLoad = [&](FunctionCallee helper, int32_t isList) {
        auto* ptr = b.CreateCall(helper, {
            jitCtx, b.getInt64(3),
            ConstantPointerNull::get(b.getPtrTy()),
            ConstantPointerNull::get(b.getPtrTy()),
            b.getInt32(isList)});
        auto* field = b.CreateConstGEP1_32(b.getInt8Ty(), ptr, kValueOff);
        return b.CreateLoad(b.getInt64Ty(), field);
    };
    auto* checked = makeLoad(guarded, 0);
    auto* checkedList = makeLoad(guarded, 1);
    auto* badHelper = makeLoad(forged, 0);
    assert(classifyNativeVariableValueLoad(checked,jitCtx,kSlotOff,kValueOff)
           == NativeVariableValueOrigin::CheckedResolver);
    assert(classifyNativeVariableValueLoad(checkedList,jitCtx,kSlotOff,kValueOff)
           == NativeVariableValueOrigin::Unknown);
    assert(classifyNativeVariableValueLoad(badHelper,jitCtx,kSlotOff,kValueOff)
           == NativeVariableValueOrigin::Unknown);
    xyo::rt::LiveI64ProvenanceCensus census;
    for (auto* v : {value, checked, checkedList, badHelper, wrongValue})
        census.observe(v, jitCtx, kSlotOff, kValueOff);
    assert(census.total == 5 && census.variableSlotValues == 1 &&
           census.checkedVariableValues == 1 && census.unclassified == 3 &&
           census.complete());
    b.CreateRetVoid();
    assert(!verifyFunction(*fn, &errs()));
    std::puts("PASS RT-001 native variable loads: direct/guarded proven, "
              "wrong offset/list/helper denied; gc_safe=0");
}
