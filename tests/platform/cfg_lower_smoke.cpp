// Real LLVM and WASM lowering of the same multi-Phi executable CFGs.
// Program generation is deliberately backend-independent.
#include "../../src/ir/cfg_lower_llvm.hpp"
#include "../../src/ir/cfg_lower_wasm.hpp"
#include "../../src/ir/scratch_numeric_kernel.hpp"
#include "../../src/ir/scratch_f64_kernel.hpp"

#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <fstream>
#include <string>

using namespace xyo::ir;

static Executable loopProgram() {
    Executable p;
    auto& g = p.ssa.cfg;
    const auto entry = g.createBlock();
    const auto header = g.createBlock();
    const auto body = g.createBlock();
    const auto exit = g.createBlock();
    g.block(entry).terminator = {TerminatorKind::Jump, header};
    g.block(header).terminator = {TerminatorKind::Branch, body, exit, 5};
    g.block(body).terminator = {TerminatorKind::Jump, header};
    g.block(exit).terminator = {TerminatorKind::Return, 0, 0, 0, 4};
    g.block(header).phis = {
        {Type::I64, {{entry, 0}, {body, 6}}},
        {Type::I64, {{entry, 0}, {body, 7}}}
    };
    p.ssa.definitions = {
        {entry,Type::I64,1}, {entry,Type::I64,2},
        {entry,Type::I64,3},
        {header,Type::I64,0,true,0},
        {header,Type::I64,0,true,1},
        {header,Type::I1,1},
        {body,Type::I64,1}, {body,Type::I64,2},
    };
    p.instructions.resize(4);
    p.instructions[entry] = {
        {0,ExecOp::ConstI64,0,0,0},
        {1,ExecOp::ConstI64,0,0,10},
        {2,ExecOp::ConstI64,0,0,1}
    };
    p.instructions[header] = {{5,ExecOp::LessI64,3,1}};
    p.instructions[body] = {
        {6,ExecOp::AddI64,3,2},
        {7,ExecOp::AddI64,4,6}
    };
    return p;
}
static Executable diamondProgram(bool truth = true) {
    Executable p;
    auto& g = p.ssa.cfg;
    const auto entry = g.createBlock();
    const auto yes = g.createBlock();
    const auto no = g.createBlock();
    const auto merge = g.createBlock();
    g.block(entry).terminator = {TerminatorKind::Branch, yes, no, 2};
    g.block(yes).terminator = {TerminatorKind::Jump, merge};
    g.block(no).terminator = {TerminatorKind::Jump, merge};
    g.block(merge).terminator = {TerminatorKind::Return, 0, 0, 0, 5};
    g.block(merge).phis = {{Type::I64, {{yes, 3}, {no, 4}}}};
    p.ssa.definitions = {
        {entry,Type::I64,1}, {entry,Type::I64,2},
        {entry,Type::I1,3}, {yes,Type::I64,1},
        {no,Type::I64,1}, {merge,Type::I64,0,true,0}
    };
    p.instructions.resize(4);
    p.instructions[entry] = {
        {0,ExecOp::ConstI64,0,0,truth ? 3 : 7},
        {1,ExecOp::ConstI64,0,0,5},
        {2,ExecOp::LessI64,0,1}
    };
    p.instructions[yes] = {{3,ExecOp::ConstI64,0,0,42}};
    p.instructions[no] = {{4,ExecOp::ConstI64,0,0,99}};
    return p;
}

// The two loop-carried PHIs swap on every backedge. A WASM lowering
// that assigns Phi locals sequentially would corrupt the second value.
static Executable swapProgram() {
    Executable p;
    auto& g = p.ssa.cfg;
    const auto entry = g.createBlock();
    const auto header = g.createBlock();
    const auto body = g.createBlock();
    const auto exit = g.createBlock();
    g.block(entry).terminator = {TerminatorKind::Jump, header};
    g.block(header).terminator = {TerminatorKind::Branch, body, exit, 8};
    g.block(body).terminator = {TerminatorKind::Jump, header};
    g.block(exit).terminator = {TerminatorKind::Return, 0, 0, 0, 6};
    g.block(header).phis = {
        {Type::I64, {{entry,0}, {body,9}}},
        {Type::I64, {{entry,3}, {body,7}}},
        {Type::I64, {{entry,4}, {body,6}}}
    };
    p.ssa.definitions = {
        {entry,Type::I64,1}, {entry,Type::I64,2},
        {entry,Type::I64,3}, {entry,Type::I64,4},
        {entry,Type::I64,5},
        {header,Type::I64,0,true,0},
        {header,Type::I64,0,true,1},
        {header,Type::I64,0,true,2},
        {header,Type::I1,1},
        {body,Type::I64,1}
    };
    p.instructions.resize(4);
    p.instructions[entry] = {
        {0,ExecOp::ConstI64,0,0,0},
        {1,ExecOp::ConstI64,0,0,1},
        {2,ExecOp::ConstI64,0,0,3},
        {3,ExecOp::ConstI64,0,0,10},
        {4,ExecOp::ConstI64,0,0,20}
    };
    p.instructions[header] = {{8,ExecOp::LessI64,5,2}};
    p.instructions[body] = {{9,ExecOp::AddI64,5,1}};
    return p;
}

// A conditional with identical successors is one CFG predecessor edge.
// Lowering must not generate two incoming LLVM PHI edges.
static Executable identicalTargetsProgram() {
    Executable p;
    auto& g = p.ssa.cfg;
    auto entry = g.createBlock();
    auto merge = g.createBlock();
    g.block(entry).terminator = {TerminatorKind::Branch, merge, merge, 1};
    g.block(merge).terminator = {TerminatorKind::Return, 0, 0, 0, 3};
    g.block(merge).phis = {{Type::I64, {{entry,0}}}};
    p.ssa.definitions = {
        {entry,Type::I64,1}, {entry,Type::I1,3},
        {entry,Type::I64,2}, {merge,Type::I64,0,true,0}
    };
    p.instructions.resize(2);
    p.instructions[entry] = {
        {0,ExecOp::ConstI64,0,0,7},
        {2,ExecOp::ConstI64,0,0,10},
        {1,ExecOp::LessI64,0,2}
    };
    return p;
}

// Mutable state across a backedge, independent of the SSA induction Phi.
static Executable variableLoopProgram() {
    Executable p;
    p.variableSlots = 1;
    auto& cfg = p.ssa.cfg;
    const auto entry = cfg.createBlock();
    const auto header = cfg.createBlock();
    const auto body = cfg.createBlock();
    const auto exit = cfg.createBlock();
    cfg.block(entry).terminator = {TerminatorKind::Jump, header};
    cfg.block(header).terminator = {TerminatorKind::Branch, body, exit, 5};
    cfg.block(body).terminator = {TerminatorKind::Jump, header};
    cfg.block(exit).terminator = {TerminatorKind::Return,0,0,0,8};
    cfg.block(header).phis = {{Type::I64, {{entry,0}, {body,7}}}};
    p.ssa.definitions = {
        {entry,Type::I64,1}, {entry,Type::I64,2},
        {entry,Type::I64,4}, {entry,Type::I64,5},
        {header,Type::I64,0,true,0},
        {header,Type::I1,1},
        {body,Type::I64,1}, {body,Type::I64,3},
        {exit,Type::I64,1}, {entry,Type::I64,6}
    };
    p.instructions.resize(4);
    p.instructions[entry] = {
        {0,ExecOp::ConstI64,0,0,0},
        {1,ExecOp::ConstI64,0,0,4},
        {kNoValue,ExecOp::StoreVar,0,0,0,0},
        {2,ExecOp::ConstI64,0,0,5},
        {3,ExecOp::ConstI64,0,0,3},
        {9,ExecOp::ConstI64,0,0,1},
        {kNoValue,ExecOp::StoreVar,2,0,0,0}
    };
    p.instructions[header] = {{5,ExecOp::LessI64,4,1}};
    p.instructions[body] = {
        {6,ExecOp::ChangeVar,3,0,0,0},
        {kNoValue,ExecOp::StoreVar,6,0,0,0},
        {7,ExecOp::AddI64,4,9}
    };
    p.instructions[exit] = {{8,ExecOp::LoadVar,0,0,0,0}};
    return p;
}

// Branch-local writes are visible in the merge, but only the chosen path
// may update the mutable slot.
static Executable variableBranchProgram(bool truth) {
    Executable p;
    p.variableSlots = 1;
    auto& cfg = p.ssa.cfg;
    const auto entry = cfg.createBlock();
    const auto yes = cfg.createBlock();
    const auto no = cfg.createBlock();
    const auto merge = cfg.createBlock();
    cfg.block(entry).terminator = {TerminatorKind::Branch,yes,no,2};
    cfg.block(yes).terminator = {TerminatorKind::Jump,merge};
    cfg.block(no).terminator = {TerminatorKind::Jump,merge};
    cfg.block(merge).terminator = {TerminatorKind::Return,0,0,0,5};
    p.ssa.definitions = {
        {entry,Type::I64,1}, {entry,Type::I64,2},
        {entry,Type::I1,3}, {yes,Type::I64,1},
        {no,Type::I64,1}, {merge,Type::I64,1}
    };
    p.instructions.resize(4);
    p.instructions[entry] = {
        {0,ExecOp::ConstI64,0,0,truth ? 3 : 7},
        {1,ExecOp::ConstI64,0,0,5},
        {2,ExecOp::LessI64,0,1}
    };
    p.instructions[yes] = {
        {3,ExecOp::ConstI64,0,0,27},
        {kNoValue,ExecOp::StoreVar,3,0,0,0}
    };
    p.instructions[no] = {
        {4,ExecOp::ConstI64,0,0,91},
        {kNoValue,ExecOp::StoreVar,4,0,0,0}
    };
    p.instructions[merge] = {{5,ExecOp::LoadVar,0,0,0,0}};
    return p;
}

// Two independent mutable cells, including read-after-write.
static Executable twoSlotsProgram() {
    Executable p;
    p.variableSlots = 2;
    auto& cfg = p.ssa.cfg;
    cfg.createBlock();
    cfg.block(0).terminator = {TerminatorKind::Return, 0,0,0, 7};
    p.ssa.definitions = {
        {0,Type::I64,1}, {0,Type::I64,2},
        {0,Type::I64,4}, {0,Type::I64,6},
        {0,Type::I64,7}, {0,Type::I64,9},
        {0,Type::I64,10}, {0,Type::I64,11}
    };
    p.instructions.resize(1);
    p.instructions[0] = {
        {0,ExecOp::ConstI64,0,0,8},
        {1,ExecOp::ConstI64,0,0,-3},
        {kNoValue,ExecOp::StoreVar,0,0,0,0},
        {2,ExecOp::ChangeVar,1,0,0,0},
        {kNoValue,ExecOp::StoreVar,2,0,0,0},
        {3,ExecOp::ConstI64,0,0,30},
        {4,ExecOp::LoadVar,0,0,0,1},
        {kNoValue,ExecOp::StoreVar,3,0,0,1},
        {5,ExecOp::LoadVar,0,0,0,0},
        {6,ExecOp::LoadVar,0,0,0,1},
        {7,ExecOp::AddI64,5,6}
    };
    return p;
}
static Executable zeroSlotProgram() {
    Executable p;
    p.variableSlots = 1;
    p.ssa.cfg.createBlock();
    p.ssa.cfg.block(0).terminator = {TerminatorKind::Return,0,0,0,0};
    p.ssa.definitions = {{0,Type::I64,1}};
    p.instructions = {{{0,ExecOp::LoadVar,0,0,0,0}}};
    return p;
}
// A genuine Scratch block sequence (including nested reporter expressions)
// lowered to exactly the same executable CFG consumed by LLVM and WASM.
static xyo::ir::ScratchScript numericScratchScript() {
    xyo::ir::ScratchScript s;
    s.topId = "hat";
    auto add = [&](const std::string& id, const std::string& opcode) -> xyo::Block& {
        xyo::Block block;
        block.id = id;
        block.opcode = opcode;
        s.byId.emplace(id, s.nodes.size());
        s.nodes.push_back(std::move(block));
        return s.nodes.back();
    };
    add("hat", "event_whenflagclicked");
    add("a", "data_setvariableto");
    add("b", "data_changevariableby");
    add("c", "data_setvariableto");
    add("d", "data_changevariableby");
    add("n10", "math_number");
    add("n5", "math_number");
    add("n7", "math_number");
    add("n2", "math_number");
    add("sum", "operator_add");
    add("get", "data_variable");
    s.nodes[s.byId.at("hat")].next = "a";
    s.nodes[s.byId.at("a")].next = "b";
    s.nodes[s.byId.at("b")].next = "c";
    s.nodes[s.byId.at("c")].next = "d";
    auto setVar = [&](const char* id, const char* variable) {
        s.nodes[s.byId.at(id)].fields["VARIABLE"].id = variable;
    };
    setVar("a", "x");
    setVar("b", "x");
    setVar("c", "y");
    setVar("d", "y");
    setVar("get", "x");
    auto input = [&](const char* id, const char* name, const char* reporter) {
        s.nodes[s.byId.at(id)].inputs.set(name, {reporter, ""});
    };
    input("a", "VALUE", "n10");
    input("b", "VALUE", "n5");
    input("c", "VALUE", "sum");
    input("d", "VALUE", "n2");
    input("sum", "NUM1", "get");
    input("sum", "NUM2", "n7");
    for (const auto& [id, number] : std::vector<std::pair<std::string, double>>{
             {"n10", 10}, {"n5", 5}, {"n7", 7}, {"n2", 2}}) {
        s.nodes[s.byId.at(id)].fields["NUM"].value = xyo::Value::num(number);
    }
    return s;
}

static xyo::ir::ScratchScript conditionalNumericScript(double threshold) {
    auto source = numericScratchScript();
    auto add = [&](const char* id, const char* op) {
        xyo::Block b;
        b.id = id;
        b.opcode = op;
        source.byId.emplace(id, source.nodes.size());
        source.nodes.push_back(std::move(b));
    };
    add("if", "control_if_else");
    add("then", "data_changevariableby");
    add("else", "data_changevariableby");
    add("compare", "operator_lt");
    add("threshold", "math_number");
    source.nodes[source.byId.at("b")].next = "if";
    source.nodes[source.byId.at("if")].next = "c";
    auto& cond = source.nodes[source.byId.at("if")];
    cond.inputs.set("CONDITION", {"compare", ""});
    cond.inputs.set("SUBSTACK", {"then", ""});
    cond.inputs.set("SUBSTACK2", {"else", ""});
    auto& cmp = source.nodes[source.byId.at("compare")];
    cmp.inputs.set("OPERAND1", {"get", ""});
    cmp.inputs.set("OPERAND2", {"threshold", ""});
    source.nodes[source.byId.at("threshold")].fields["NUM"].value =
        xyo::Value::num(threshold);
    source.nodes[source.byId.at("then")].fields["VARIABLE"].id = "x";
    source.nodes[source.byId.at("then")].inputs.set("VALUE", {"n7", ""});
    source.nodes[source.byId.at("else")].fields["VARIABLE"].id = "x";
    source.nodes[source.byId.at("else")].inputs.set("VALUE", {"n2", ""});
    return source;
}

static xyo::ir::ScratchScript repeatingNumericScript(
        double repetitions, bool conditionalBody = false,
        bool nested = false) {
    auto source = numericScratchScript();
    auto add = [&](const char* id, const char* opcode) {
        xyo::Block b;
        b.id = id;
        b.opcode = opcode;
        source.byId.emplace(id, source.nodes.size());
        source.nodes.push_back(std::move(b));
    };
    add("repeat", "control_repeat");
    add("times", "math_number");
    add("loop-change", "data_changevariableby");
    source.nodes[source.byId.at("b")].next = "repeat";
    source.nodes[source.byId.at("repeat")].next = "c";
    source.nodes[source.byId.at("times")].fields["NUM"].value =
        xyo::Value::num(repetitions);
    source.nodes[source.byId.at("repeat")].inputs.set(
        "TIMES", {"times", ""});
    source.nodes[source.byId.at("loop-change")].fields["VARIABLE"].id = "x";
    source.nodes[source.byId.at("loop-change")].inputs.set(
        "VALUE", {"n2", ""});
    source.nodes[source.byId.at("repeat")].inputs.set(
        "SUBSTACK", {"loop-change", ""});
    if (conditionalBody) {
        add("loop-if", "control_if_else");
        add("loop-compare", "operator_lt");
        add("loop-threshold", "math_number");
        add("loop-else", "data_changevariableby");
        source.nodes[source.byId.at("repeat")].inputs.set(
            "SUBSTACK", {"loop-if", ""});
        auto& compare = source.nodes[source.byId.at("loop-compare")];
        compare.inputs.set("OPERAND1", {"get", ""});
        compare.inputs.set("OPERAND2", {"loop-threshold", ""});
        source.nodes[source.byId.at("loop-threshold")].fields["NUM"].value =
            xyo::Value::num(20);
        auto& branch = source.nodes[source.byId.at("loop-if")];
        branch.inputs.set("CONDITION", {"loop-compare", ""});
        branch.inputs.set("SUBSTACK", {"loop-change", ""});
        branch.inputs.set("SUBSTACK2", {"loop-else", ""});
        source.nodes[source.byId.at("loop-else")].fields["VARIABLE"].id = "x";
        source.nodes[source.byId.at("loop-else")].inputs.set(
            "VALUE", {"n5", ""});
    }
    if (nested) {
        add("inner", "control_repeat");
        source.nodes[source.byId.at("repeat")].inputs.set(
            "SUBSTACK", {"inner", ""});
        source.nodes[source.byId.at("inner")].inputs.set(
            "TIMES", {"n2", ""});
        source.nodes[source.byId.at("inner")].inputs.set(
            "SUBSTACK", {"loop-change", ""});
    }
    return source;
}

static xyo::ir::ScratchScript dynamicRepeatingScript(
    bool arithmetic = false) {
    auto source = repeatingNumericScript(3);
    xyo::Block reporter;
    reporter.id = "count-get";
    reporter.opcode = "data_variable";
    reporter.fields["VARIABLE"].id = "y";
    source.byId.emplace(reporter.id, source.nodes.size());
    source.nodes.push_back(std::move(reporter));
    auto& loop = source.nodes[source.byId.at("repeat")];
    loop.inputs.set("TIMES", {"count-get", ""});
    if (arithmetic) {
        xyo::Block sum;
        sum.id = "count-sum";
        sum.opcode = "operator_add";
        sum.inputs.set("NUM1", {"count-get", ""});
        sum.inputs.set("NUM2", {"n2", ""});
        source.byId.emplace(sum.id, source.nodes.size());
        source.nodes.push_back(std::move(sum));
        loop.inputs.set("TIMES", {"count-sum", ""});
    }
    return source;
}

static xyo::ir::ScratchScript sequentialRepeatsScript() {
    auto source = repeatingNumericScript(3);
    auto add = [&](const char* id, const char* opcode) {
        xyo::Block b;
        b.id = id;
        b.opcode = opcode;
        source.byId.emplace(id, source.nodes.size());
        source.nodes.push_back(std::move(b));
    };
    add("repeat-second", "control_repeat");
    add("second-count", "math_number");
    add("second-change", "data_changevariableby");
    source.nodes[source.byId.at("repeat")].next = "repeat-second";
    source.nodes[source.byId.at("repeat-second")].next = "c";
    source.nodes[source.byId.at("repeat-second")].inputs.set(
        "TIMES", {"second-count", ""});
    source.nodes[source.byId.at("repeat-second")].inputs.set(
        "SUBSTACK", {"second-change", ""});
    source.nodes[source.byId.at("second-count")].fields["NUM"].value =
        xyo::Value::num(2);
    source.nodes[source.byId.at("second-change")].fields["VARIABLE"].id = "x";
    source.nodes[source.byId.at("second-change")].inputs.set(
        "VALUE", {"n5", ""});
    return source;
}

// Run identical executable Scratch numeric CFGs over externally staged
// NaN-boxed slots. The LLVM wrapper checks all resulting bytes and returns
// an exit status suitable for lli; WASM exports run(i32 slots) -> i64 bits.
static bool writeExternal(
    const ScratchNumericKernel& kernel, const std::string& prefix,
    const std::vector<uint64_t>& initial,
    const std::vector<uint64_t>& expected,
    uint64_t expectedReturn) {
    const auto& program = kernel.executable;
    if (!program.externalValueSlots ||
        !verifyExecutable(program) ||
        initial.size() != program.variableSlots ||
        expected.size() != program.variableSlots) return false;
    llvm::LLVMContext context;
    llvm::Module module("scratch-f64", context);
    LlvmCfgBackend backend(context, module, "run");
    if (kernel.requiresLoopScheduler) {
        const auto rejection = lowerExecutable(program, backend);
        if (rejection.error != VerifyError::RequiresScheduler ||
            module.getFunction("run")) return false;
    }
    if (!lowerExecutable(program, backend,
                         kernel.requiresLoopScheduler)) return false;
    auto* i64 = llvm::Type::getInt64Ty(context);
    auto* signature = llvm::FunctionType::get(i64, false);
    auto* main = llvm::Function::Create(signature,
        llvm::GlobalValue::ExternalLinkage, "main", module);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", main));
    auto* staged = b.CreateAlloca(
        i64, b.getInt32(uint32_t(initial.size())), "scratch.slots");
    for (uint32_t i = 0; i < initial.size(); ++i) {
        auto* element = b.CreateInBoundsGEP(i64, staged, b.getInt32(i));
        b.CreateStore(llvm::ConstantInt::get(i64, initial[i]), element);
    }
    auto* returned = b.CreateCall(backend.function(), {staged});
    llvm::Value* error = b.CreateICmpNE(
        returned, llvm::ConstantInt::get(i64, expectedReturn));
    for (uint32_t i = 0; i < expected.size(); ++i) {
        auto* element = b.CreateInBoundsGEP(i64, staged, b.getInt32(i));
        error = b.CreateOr(error, b.CreateICmpNE(
            b.CreateLoad(i64, element),
            llvm::ConstantInt::get(i64, expected[i])));
    }
    b.CreateRet(b.CreateZExt(error, i64));
    if (llvm::verifyModule(module)) return false;
    {
        std::error_code ec;
        llvm::raw_fd_ostream output(prefix + ".ll", ec);
        if (ec) return false;
        module.print(output, nullptr);
        output.flush();
    }
    xyo::wasm::Module wasm;
    wasm.importMemory("env", "memory", 1, 0, false);
    const auto type = wasm.type({xyo::wasm::ValType::I32},
                                {xyo::wasm::ValType::I64});
    const auto index = wasm.addFunc(type);
    wasm.exportFunc("run", index);
    auto& code = wasm.code(index);
    WasmCfgBackend portable(code, 1);
    if (!lowerExecutable(program, portable,
                         kernel.requiresLoopScheduler)) return false;
    code.end();
    const auto bytes = wasm.finish();
    std::ofstream binary(prefix + ".wasm", std::ios::binary);
    binary.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return bool(binary);
}

// A resumable executable must publish exactly one completed iteration per
// call. LLVM's wrapper invokes the same function repeatedly over one frame;
// Node's WebAssembly test does the same with imported linear memory.
static bool writeResumable(
    const ScratchNumericKernel& kernel, const std::string& prefix,
    const std::vector<uint64_t>& initial,
    const std::vector<std::vector<uint64_t>>& snapshots,
    uint64_t expectedReturn) {
    const auto& program = kernel.executable;
    if (!program.resumable || !kernel.requiresLoopScheduler ||
        snapshots.empty() || initial.size() != program.variableSlots ||
        !verifyExecutable(program)) return false;
    for (const auto& checkpoint : snapshots)
        if (checkpoint.size() != initial.size()) return false;
    llvm::LLVMContext context;
    llvm::Module module("scratch-resume", context);
    LlvmCfgBackend backend(context, module, "run");
    if (!lowerExecutable(program, backend)) return false;
    auto* i64 = llvm::Type::getInt64Ty(context);
    auto* main = llvm::Function::Create(
        llvm::FunctionType::get(i64, false),
        llvm::GlobalValue::ExternalLinkage, "main", module);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", main));
    auto* slots = b.CreateAlloca(i64, b.getInt32(initial.size()), "slots");
    const uint32_t frameWords =
        uint32_t(2 + program.ssa.definitions.size());
    auto* frame = b.CreateAlloca(i64, b.getInt32(frameWords), "frame");
    for (uint32_t i = 0; i < initial.size(); ++i) {
        auto* ptr = b.CreateInBoundsGEP(i64, slots, b.getInt32(i));
        b.CreateStore(b.getInt64(initial[i]), ptr);
    }
    for (uint32_t i = 0; i < frameWords; ++i) {
        auto* ptr = b.CreateInBoundsGEP(i64, frame, b.getInt32(i));
        b.CreateStore(b.getInt64(0), ptr);
    }
    llvm::Value* bad = b.getFalse();
    for (size_t step = 0; step < snapshots.size(); ++step) {
        auto* status = b.CreateCall(backend.function(), {slots, frame});
        bad = b.CreateOr(bad, b.CreateICmpNE(
            status, b.getInt64(step + 1 == snapshots.size() ? 0 : 1)));
        for (uint32_t slot = 0; slot < initial.size(); ++slot) {
            auto* ptr = b.CreateInBoundsGEP(i64, slots, b.getInt32(slot));
            bad = b.CreateOr(bad, b.CreateICmpNE(
                b.CreateLoad(i64, ptr),
                b.getInt64(snapshots[step][slot])));
        }
        auto* pcPtr = b.CreateInBoundsGEP(i64, frame, b.getInt32(0));
        auto* pc = b.CreateLoad(i64, pcPtr);
        if (step + 1 == snapshots.size())
            bad = b.CreateOr(bad, b.CreateICmpNE(
                pc, b.getInt64(UINT64_MAX)));
        else
            bad = b.CreateOr(bad, b.CreateICmpEQ(
                pc, b.getInt64(UINT64_MAX)));
    }
    auto* resultPtr = b.CreateInBoundsGEP(i64, frame, b.getInt32(1));
    bad = b.CreateOr(bad, b.CreateICmpNE(
        b.CreateLoad(i64, resultPtr), b.getInt64(expectedReturn)));
    b.CreateRet(b.CreateZExt(bad, i64));
    if (llvm::verifyModule(module)) return false;
    {
        std::error_code ec;
        llvm::raw_fd_ostream file(prefix + ".ll", ec);
        if (ec) return false;
        module.print(file, nullptr);
        file.flush();
    }
    xyo::wasm::Module wasm;
    wasm.importMemory("env", "memory", 1, 0, false);
    auto type = wasm.type({xyo::wasm::ValType::I32,
                           xyo::wasm::ValType::I32},
                          {xyo::wasm::ValType::I64});
    auto index = wasm.addFunc(type);
    wasm.exportFunc("run", index);
    auto& code = wasm.code(index);
    WasmCfgBackend portable(code, 2);
    if (!lowerExecutable(program, portable)) return false;
    code.end();
    const auto bytes = wasm.finish();
    std::ofstream file(prefix + ".wasm", std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return bool(file);
}

static bool write(const Executable& program, const std::string& prefix) {
    if (!verifyExecutable(program)) return false;
    llvm::LLVMContext context;
    llvm::Module module("xyo-cfg", context);
    LlvmCfgBackend native(context, module);
    if (!lowerExecutable(program, native) || llvm::verifyModule(module))
        return false;
    std::error_code ec;
    llvm::raw_fd_ostream output(prefix + ".ll", ec);
    if (ec) return false;
    module.print(output, nullptr);
    output.flush();

    xyo::wasm::Module wasm;
    const auto signature = wasm.type({}, {xyo::wasm::ValType::I64});
    const auto function = wasm.addFunc(signature);
    wasm.exportFunc("main", function);
    auto& code = wasm.code(function);
    WasmCfgBackend portable(code);
    if (!lowerExecutable(program, portable)) return false;
    code.end();
    const auto bytes = wasm.finish();
    std::ofstream binary(prefix + ".wasm", std::ios::binary);
    binary.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return bool(binary);
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string directory(argv[1]);
    if (!write(loopProgram(), directory + "/loop")) return 3;
    if (!write(diamondProgram(), directory + "/diamond")) return 4;
    if (!write(diamondProgram(false), directory + "/diamond-false")) return 5;
    if (!write(swapProgram(), directory + "/phi-swap")) return 6;
    if (!write(identicalTargetsProgram(), directory + "/same-target")) return 9;
    if (!write(variableLoopProgram(), directory + "/var-loop")) return 10;
    if (!write(variableBranchProgram(true), directory + "/var-true")) return 11;
    if (!write(variableBranchProgram(false), directory + "/var-false")) return 12;
    if (!write(twoSlotsProgram(), directory + "/var-two")) return 15;
    if (!write(zeroSlotProgram(), directory + "/var-zero")) return 16;
    {
        auto source = numericScratchScript();
        source.nodes[source.byId.at("n10")].fields["NUM"].value =
            xyo::Value::num(10.25);
        source.nodes[source.byId.at("n5")].fields["NUM"].value =
            xyo::Value::num(0.5);
        source.nodes[source.byId.at("n7")].fields["NUM"].value =
            xyo::Value::num(2.25);
        source.nodes[source.byId.at("n2")].fields["NUM"].value =
            xyo::Value::num(-0.5);
        auto lowered = lowerNumericF64Script(source);
        if (!lowered ||
            lowered->slotIds != std::vector<std::string>({"x", "y"}) ||
            !writeExternal(*lowered, directory + "/scratch-f64",
                {xyo::Value::num(6.0).bits, xyo::Value::num(-2.0).bits},
                {xyo::Value::num(10.75).bits, xyo::Value::num(12.5).bits},
                xyo::Value::num(12.5).bits)) return 25;
        // Typed verifier must reject malformed f64 definitions and
        // external slot indices before either backend sees the program.
        auto invalidFloat = lowered->executable;
        invalidFloat.ssa.definitions[0].type = Type::I64;
        if (verifyExecutable(invalidFloat)) return 40;
        invalidFloat = lowered->executable;
        for (auto& insn : invalidFloat.instructions[0]) {
            if (insn.op == ExecOp::LoadNumber ||
                insn.op == ExecOp::StoreNumber) {
                insn.slot = 999;
                break;
            }
        }
        if (verifyExecutable(invalidFloat)) return 41;
        // Change by reads the host-staged initial value, not a zero local.
        source.nodes[source.byId.at("a")].opcode =
            "data_changevariableby";
        lowered = lowerNumericF64Script(source);
        if (!lowered ||
            !writeExternal(*lowered, directory + "/scratch-f64-change",
                {xyo::Value::num(1.5).bits, xyo::Value::num(-2.0).bits},
                {xyo::Value::num(12.25).bits, xyo::Value::num(14).bits},
                xyo::Value::num(14).bits)) return 26;
        source = numericScratchScript();
        source.nodes[source.byId.at("n10")].fields["NUM"].value =
            xyo::Value::num(-0.0);
        source.nodes[source.byId.at("a")].next.clear();
        lowered = lowerNumericF64Script(source);
        if (!lowered ||
            !writeExternal(*lowered, directory + "/scratch-f64-negzero",
                {xyo::Value::num(1).bits},
                {xyo::Value::num(-0.0).bits},
                xyo::Value::num(-0.0).bits)) return 27;
        source = numericScratchScript();
        source.nodes[source.byId.at("sum")].opcode = "operator_divide";
        source.nodes[source.byId.at("c")].next.clear();
        source.nodes[source.byId.at("n10")].fields["NUM"].value =
            xyo::Value::num(0);
        source.nodes[source.byId.at("n5")].fields["NUM"].value =
            xyo::Value::num(0);
        source.nodes[source.byId.at("n7")].fields["NUM"].value =
            xyo::Value::num(0);
        lowered = lowerNumericF64Script(source);
        if (!lowered ||
            !writeExternal(*lowered, directory + "/scratch-f64-nan",
                {xyo::Value::num(2).bits, xyo::Value::num(3).bits},
                {xyo::Value::num(0).bits, xyo::val::kNan},
                xyo::val::kNan)) return 28;
        source = numericScratchScript();
        source.nodes[source.byId.at("sum")].opcode = "operator_multiply";
        lowered = lowerNumericF64Script(source);
        if (!lowered ||
            !writeExternal(*lowered, directory + "/scratch-f64-mul",
                {xyo::Value::num(1).bits, xyo::Value::num(0).bits},
                {xyo::Value::num(15).bits, xyo::Value::num(107).bits},
                xyo::Value::num(107).bits)) return 29;
        auto conditional = conditionalNumericScript(20);
        auto conditionalKernel = lowerNumericF64Script(conditional);
        if (!conditionalKernel ||
            !writeExternal(*conditionalKernel, directory + "/scratch-if-true",
                {xyo::Value::num(4).bits, xyo::Value::num(0).bits},
                {xyo::Value::num(22).bits, xyo::Value::num(31).bits},
                xyo::Value::num(31).bits)) return 32;
        conditional = conditionalNumericScript(10);
        conditionalKernel = lowerNumericF64Script(conditional);
        if (!conditionalKernel ||
            !writeExternal(*conditionalKernel, directory + "/scratch-if-false",
                {xyo::Value::num(4).bits, xyo::Value::num(0).bits},
                {xyo::Value::num(17).bits, xyo::Value::num(26).bits},
                xyo::Value::num(26).bits)) return 33;
        // An empty false branch (control_if) still has a valid CFG merge.
        conditional.nodes[conditional.byId.at("if")].opcode = "control_if";
        conditionalKernel = lowerNumericF64Script(conditional);
        if (!conditionalKernel ||
            !writeExternal(*conditionalKernel, directory + "/scratch-if-empty",
                {xyo::Value::num(4).bits, xyo::Value::num(0).bits},
                {xyo::Value::num(15).bits, xyo::Value::num(24).bits},
                xyo::Value::num(24).bits)) return 34;
        conditional.nodes[conditional.byId.at("compare")].opcode =
            "operator_and";
        if (lowerNumericF64Script(conditional)) return 35;
        conditional = conditionalNumericScript(15);
        conditional.nodes[conditional.byId.at("compare")].opcode =
            "operator_equals";
        conditionalKernel = lowerNumericF64Script(conditional);
        if (!conditionalKernel ||
            !writeExternal(*conditionalKernel, directory + "/scratch-if-equal",
                {xyo::Value::num(4).bits, xyo::Value::num(0).bits},
                {xyo::Value::num(22).bits, xyo::Value::num(31).bits},
                xyo::Value::num(31).bits)) return 36;
        conditional = conditionalNumericScript(10);
        conditional.nodes[conditional.byId.at("compare")].opcode =
            "operator_gt";
        conditionalKernel = lowerNumericF64Script(conditional);
        if (!conditionalKernel ||
            !writeExternal(*conditionalKernel, directory + "/scratch-if-gt",
                {xyo::Value::num(4).bits, xyo::Value::num(0).bits},
                {xyo::Value::num(22).bits, xyo::Value::num(31).bits},
                xyo::Value::num(31).bits)) return 37;
        source = numericScratchScript();
        source.nodes[source.byId.at("a")].opcode = "data_changevariableby";
        lowered = lowerNumericF64Script(source);
        if (!lowered ||
            !writeExternal(*lowered, directory + "/scratch-initial-nan",
                {xyo::val::kNan, xyo::Value::num(3).bits},
                {xyo::Value::num(15).bits, xyo::Value::num(24).bits},
                xyo::Value::num(24).bits)) return 38;
        source = numericScratchScript();
        source.nodes[source.byId.at("sum")].opcode = "operator_divide";
        for (const char* id : {"n10", "n5", "n7"})
            source.nodes[source.byId.at(id)].fields["NUM"].value =
                xyo::Value::num(0);
        lowered = lowerNumericF64Script(source);
        if (!lowered ||
            !writeExternal(*lowered, directory + "/scratch-nan-to-zero",
                {xyo::Value::num(0).bits, xyo::Value::num(3).bits},
                {xyo::Value::num(0).bits, xyo::Value::num(2).bits},
                xyo::Value::num(2).bits)) return 39;
        {
            const NumericF64LoweringOptions unscheduled{true};
            auto repeat = repeatingNumericScript(3);
            // The normal compiler must NOT silently erase a per-iteration
            // scheduler yield, even for a tiny constant repeat count.
            if (lowerNumericF64Script(repeat)) return 42;
            auto compiled = lowerNumericF64Script(repeat, unscheduled);
            if (!compiled || !compiled->requiresLoopScheduler ||
                compiled->executable.yieldBoundaries.size() != 1 ||
                compiled->executable.yieldBoundaries[0].scratchBlockId
                    != "repeat" ||
                !writeExternal(*compiled, directory + "/scratch-repeat3",
                    {xyo::Value::num(0).bits, xyo::Value::num(0).bits},
                    {xyo::Value::num(21).bits, xyo::Value::num(30).bits},
                    xyo::Value::num(30).bits)) return 43;
            auto corrupted = compiled->executable;
            corrupted.yieldBoundaries[0].header = UINT32_MAX;
            if (verifyExecutable(corrupted)) return 49;
            repeat = repeatingNumericScript(0);
            compiled = lowerNumericF64Script(repeat, unscheduled);
            if (!compiled || !compiled->requiresLoopScheduler ||
                !writeExternal(*compiled, directory + "/scratch-repeat0",
                    {xyo::Value::num(0).bits, xyo::Value::num(0).bits},
                    {xyo::Value::num(15).bits, xyo::Value::num(24).bits},
                    xyo::Value::num(24).bits)) return 44;
            repeat = repeatingNumericScript(4, true);
            compiled = lowerNumericF64Script(repeat, unscheduled);
            if (!compiled || !compiled->requiresLoopScheduler ||
                !writeExternal(*compiled, directory + "/scratch-repeat-if",
                    {xyo::Value::num(0).bits, xyo::Value::num(0).bits},
                    {xyo::Value::num(26).bits, xyo::Value::num(35).bits},
                    xyo::Value::num(35).bits)) return 45;
            repeat = repeatingNumericScript(3, false, true);
            compiled = lowerNumericF64Script(repeat, unscheduled);
            if (!compiled || !compiled->requiresLoopScheduler ||
                compiled->executable.yieldBoundaries.size() != 2 ||
                !writeExternal(*compiled, directory + "/scratch-repeat-nested",
                    {xyo::Value::num(0).bits, xyo::Value::num(0).bits},
                    {xyo::Value::num(27).bits, xyo::Value::num(36).bits},
                    xyo::Value::num(36).bits)) return 46;
            for (double invalid : {1025.0,
                                   std::numeric_limits<double>::infinity()}) {
                repeat = repeatingNumericScript(invalid);
                if (lowerNumericF64Script(repeat, unscheduled)) return 47;
            }
            repeat = repeatingNumericScript(1024, false, true);
            repeat.nodes[repeat.byId.at("n2")].fields["NUM"].value =
                xyo::Value::num(1024);
            if (lowerNumericF64Script(repeat, unscheduled)) return 50;
            {
                const NumericF64LoweringOptions resumable{false, true};
                repeat = repeatingNumericScript(3);
                compiled = lowerNumericF64Script(repeat, resumable);
                if (!compiled || !compiled->executable.resumable ||
                    !writeResumable(*compiled,
                        directory + "/scratch-resume3",
                        {xyo::Value::num(0).bits, xyo::Value::num(0).bits},
                        {{xyo::Value::num(17).bits,
                          xyo::Value::num(0).bits},
                         {xyo::Value::num(19).bits,
                          xyo::Value::num(0).bits},
                         {xyo::Value::num(21).bits,
                          xyo::Value::num(0).bits},
                         {xyo::Value::num(21).bits,
                          xyo::Value::num(30).bits}},
                        xyo::Value::num(30).bits)) return 51;
                repeat = repeatingNumericScript(1.5);
                compiled = lowerNumericF64Script(repeat, resumable);
                if (!compiled ||
                    compiled->executable.yieldBoundaries[0].repeatCount != 2 ||
                    !writeResumable(*compiled,
                        directory + "/scratch-resume-literal-half",
                        {xyo::Value::num(0).bits, xyo::Value::num(0).bits},
                        {{xyo::Value::num(17).bits, xyo::Value::num(0).bits},
                         {xyo::Value::num(19).bits, xyo::Value::num(0).bits},
                         {xyo::Value::num(19).bits, xyo::Value::num(28).bits}},
                        xyo::Value::num(28).bits)) return 62;
                repeat = repeatingNumericScript(-0.5);
                compiled = lowerNumericF64Script(repeat, resumable);
                if (!compiled ||
                    compiled->executable.yieldBoundaries[0].repeatCount != 0 ||
                    !writeResumable(*compiled,
                        directory + "/scratch-resume-literal-negative",
                        {xyo::Value::num(0).bits, xyo::Value::num(0).bits},
                        {{xyo::Value::num(15).bits, xyo::Value::num(24).bits}},
                        xyo::Value::num(24).bits)) return 63;

                repeat = repeatingNumericScript(0);
                compiled = lowerNumericF64Script(repeat, resumable);
                if (!compiled || !writeResumable(*compiled,
                        directory + "/scratch-resume0",
                        {xyo::Value::num(0).bits, xyo::Value::num(0).bits},
                        {{xyo::Value::num(15).bits,
                          xyo::Value::num(24).bits}},
                        xyo::Value::num(24).bits)) return 52;
                repeat = repeatingNumericScript(4, true);
                compiled = lowerNumericF64Script(repeat, resumable);
                if (!compiled || !writeResumable(*compiled,
                        directory + "/scratch-resume-if",
                        {xyo::Value::num(0).bits, xyo::Value::num(0).bits},
                        {{xyo::Value::num(17).bits,
                          xyo::Value::num(0).bits},
                         {xyo::Value::num(19).bits,
                          xyo::Value::num(0).bits},
                         {xyo::Value::num(21).bits,
                          xyo::Value::num(0).bits},
                         {xyo::Value::num(26).bits,
                          xyo::Value::num(0).bits},
                         {xyo::Value::num(26).bits,
                          xyo::Value::num(35).bits}},
                        xyo::Value::num(35).bits)) return 53;
                repeat = repeatingNumericScript(3, false, true);
                compiled = lowerNumericF64Script(repeat, resumable);
                const std::vector<std::vector<uint64_t>> nested = {
                    {xyo::Value::num(17).bits, xyo::Value::num(0).bits},
                    {xyo::Value::num(19).bits, xyo::Value::num(0).bits},
                    {xyo::Value::num(19).bits, xyo::Value::num(0).bits},
                    {xyo::Value::num(21).bits, xyo::Value::num(0).bits},
                    {xyo::Value::num(23).bits, xyo::Value::num(0).bits},
                    {xyo::Value::num(23).bits, xyo::Value::num(0).bits},
                    {xyo::Value::num(25).bits, xyo::Value::num(0).bits},
                    {xyo::Value::num(27).bits, xyo::Value::num(0).bits},
                    {xyo::Value::num(27).bits, xyo::Value::num(0).bits},
                    {xyo::Value::num(27).bits, xyo::Value::num(36).bits}
                };
                if (!compiled || !writeResumable(*compiled,
                        directory + "/scratch-resume-nested",
                        {xyo::Value::num(0).bits, xyo::Value::num(0).bits},
                        nested, xyo::Value::num(36).bits)) return 54;

                repeat = sequentialRepeatsScript();
                compiled = lowerNumericF64Script(repeat, resumable);
                if (!compiled ||
                    compiled->executable.yieldBoundaries.size() != 2 ||
                    !writeResumable(*compiled,
                        directory + "/scratch-resume-sequential",
                        {xyo::Value::num(0).bits, xyo::Value::num(0).bits},
                        {{xyo::Value::num(17).bits, xyo::Value::num(0).bits},
                         {xyo::Value::num(19).bits, xyo::Value::num(0).bits},
                         {xyo::Value::num(21).bits, xyo::Value::num(0).bits},
                         {xyo::Value::num(26).bits, xyo::Value::num(0).bits},
                         {xyo::Value::num(31).bits, xyo::Value::num(0).bits},
                         {xyo::Value::num(31).bits, xyo::Value::num(40).bits}},
                        xyo::Value::num(40).bits)) return 56;

                repeat = dynamicRepeatingScript();
                compiled = lowerNumericF64Script(repeat, resumable);
                if (!compiled ||
                    compiled->executable.yieldBoundaries.size() != 1 ||
                    compiled->executable.yieldBoundaries[0].limitValue ==
                        kNoValue ||
                    !writeResumable(*compiled,
                        directory + "/scratch-resume-dynamic",
                        {xyo::Value::num(0).bits, xyo::Value::num(2.5).bits},
                        {{xyo::Value::num(17).bits, xyo::Value::num(2.5).bits},
                         {xyo::Value::num(19).bits, xyo::Value::num(2.5).bits},
                         {xyo::Value::num(21).bits, xyo::Value::num(2.5).bits},
                         {xyo::Value::num(21).bits, xyo::Value::num(30).bits}},
                        xyo::Value::num(30).bits)) return 57;

                repeat = dynamicRepeatingScript(true);
                compiled = lowerNumericF64Script(repeat, resumable);
                if (!compiled ||
                    !writeResumable(*compiled,
                        directory + "/scratch-resume-dynamic-expr",
                        {xyo::Value::num(0).bits, xyo::Value::num(1).bits},
                        {{xyo::Value::num(17).bits, xyo::Value::num(1).bits},
                         {xyo::Value::num(19).bits, xyo::Value::num(1).bits},
                         {xyo::Value::num(21).bits, xyo::Value::num(1).bits},
                         {xyo::Value::num(21).bits, xyo::Value::num(30).bits}},
                        xyo::Value::num(30).bits)) return 58;

                repeat = dynamicRepeatingScript();
                compiled = lowerNumericF64Script(repeat, resumable);
                if (!compiled ||
                    !writeResumable(*compiled,
                        directory + "/scratch-resume-dynamic-negative",
                        {xyo::Value::num(0).bits, xyo::Value::num(-0.5).bits},
                        {{xyo::Value::num(15).bits, xyo::Value::num(24).bits}},
                        xyo::Value::num(24).bits)) return 59;

                repeat = dynamicRepeatingScript();
                compiled = lowerNumericF64Script(repeat, resumable);
                if (!compiled ||
                    !writeResumable(*compiled,
                        directory + "/scratch-resume-dynamic-half",
                        {xyo::Value::num(0).bits, xyo::Value::num(0.5).bits},
                        {{xyo::Value::num(17).bits, xyo::Value::num(0.5).bits},
                         {xyo::Value::num(17).bits, xyo::Value::num(26).bits}},
                        xyo::Value::num(26).bits)) return 60;
                if (!writeResumable(*compiled,
                        directory + "/scratch-resume-dynamic-nan",
                        {xyo::Value::num(0).bits, xyo::val::kNan},
                        {{xyo::Value::num(15).bits, xyo::Value::num(24).bits}},
                        xyo::Value::num(24).bits)) return 61;

                repeat = dynamicRepeatingScript();
                compiled = lowerNumericF64Script(repeat, resumable);
                const double belowHalf = std::nextafter(0.5, 0.0);
                if (!compiled ||
                    !writeResumable(*compiled,
                        directory + "/scratch-resume-dynamic-below-half",
                        {xyo::Value::num(0).bits, xyo::Value::num(belowHalf).bits},
                        {{xyo::Value::num(15).bits, xyo::Value::num(24).bits}},
                        xyo::Value::num(24).bits)) return 64;
                repeat = repeatingNumericScript(belowHalf);
                compiled = lowerNumericF64Script(repeat, resumable);
                if (!compiled ||
                    compiled->executable.yieldBoundaries[0].repeatCount != 0 ||
                    !writeResumable(*compiled,
                        directory + "/scratch-resume-literal-below-half",
                        {xyo::Value::num(0).bits, xyo::Value::num(0).bits},
                        {{xyo::Value::num(15).bits, xyo::Value::num(24).bits}},
                        xyo::Value::num(24).bits)) return 65;

                // Two PHIs swap with one another across the backedge. A
                // resumable frame must retain the same parallel assignment
                // semantics even when native calls end after each tick.
                ScratchNumericKernel swapKernel;
                swapKernel.executable = swapProgram();
                swapKernel.executable.externalValueSlots = true;
                swapKernel.executable.resumable = true;
                swapKernel.executable.yieldBoundaries = {
                    {2, 1, "swap-loop"}
                };
                swapKernel.requiresLoopScheduler = true;
                if (!writeResumable(
                        swapKernel, directory + "/scratch-resume-phi",
                        {}, {{}, {}, {}, {}}, 20)) return 55;
            }
            repeat = repeatingNumericScript(2);
            repeat.nodes[repeat.byId.at("loop-change")].opcode =
                "looks_say";
            if (lowerNumericF64Script(repeat, unscheduled)) return 48;
        }
        source.nodes[source.byId.at("d")].opcode = "looks_say";
        if (lowerNumericF64Script(source)) return 30;
        source = numericScratchScript();
        source.nodes[source.byId.at("n5")].fields["NUM"].value =
            xyo::Value::boolean(true);
        if (lowerNumericF64Script(source)) return 31;
    }

    auto scratch = numericScratchScript();
    const auto lowered = lowerExactIntegerScript(scratch);
    if (!lowered || lowered->slotIds !=
            std::vector<std::string>({"x", "y"}) ||
        lowered->resultVariable != "y" ||
        !write(lowered->executable, directory + "/scratch-i64"))
        return 18;
    scratch = numericScratchScript();
    scratch.nodes[scratch.byId.at("sum")].opcode = "operator_subtract";
    const auto subtraction = lowerExactIntegerScript(scratch);
    if (!subtraction ||
        !write(subtraction->executable, directory + "/scratch-sub"))
        return 23;
    scratch = numericScratchScript();
    auto& numberInput = scratch.nodes[scratch.byId.at("sum")].inputs.items[1].second;
    numberInput.shadow = "n7";
    numberInput.block.clear(); // shadows must supply the value
    const auto shadow = lowerExactIntegerScript(scratch);
    if (!shadow || !write(shadow->executable, directory + "/scratch-shadow"))
        return 24;
    scratch = numericScratchScript();
    scratch.nodes[scratch.byId.at("n7")].fields["NUM"].value =
        xyo::Value::num(7.5);
    if (lowerExactIntegerScript(scratch)) return 19;
    scratch = numericScratchScript();
    scratch.nodes[scratch.byId.at("d")].opcode = "looks_say";
    if (lowerExactIntegerScript(scratch)) return 20;
    scratch = numericScratchScript();
    scratch.nodes[scratch.byId.at("a")].opcode = "data_changevariableby";
    if (lowerExactIntegerScript(scratch)) return 21;
    scratch = numericScratchScript();
    scratch.nodes[scratch.byId.at("n7")].fields["NUM"].value =
        xyo::Value::num(9007199254740991.0);
    if (lowerExactIntegerScript(scratch)) return 22; // unsafe overflow

    auto invalidSlot = variableLoopProgram();
    invalidSlot.instructions[0][2].slot = 9;
    if (verifyExecutable(invalidSlot)) return 13;
    invalidSlot = variableLoopProgram();
    invalidSlot.instructions[0][2].lhs = 3;
    if (verifyExecutable(invalidSlot)) return 14;
    invalidSlot = variableLoopProgram();
    invalidSlot.instructions[0][2].result = 0;
    if (verifyExecutable(invalidSlot)) return 17;

    auto broken = swapProgram();
    broken.ssa.cfg.block(1).phis[0].inputs[1].value = 12345;
    if (verifyExecutable(broken)) return 7;
    broken = swapProgram();
    broken.ssa.definitions[8].type = Type::I64;
    if (verifyExecutable(broken)) return 8;
    return 0;
}
