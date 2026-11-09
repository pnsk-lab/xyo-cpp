// Focused A/B microbenchmark of the numeric continuation *host* overhead.
// Does not measure LLVM machine code or Scratch full-frame throughput.
// `legacy` reconstructs the old resume path: verifyExecutable per
// yield, Target name lookup, fresh vector snapshots and copied SSA frame.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/scratch_continuation.hpp"
#include "../../src/ir/scratch_f64_kernel.hpp"
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace xyo;
using namespace xyo::ir;
using Clock = std::chrono::steady_clock;

static ScratchNumericKernel makeKernel() {
    Blocks blocks(nullptr);
    auto add = [&](const char* id, const char* opcode) {
        Block b;
        b.id = id;
        b.opcode = opcode;
        blocks.createBlock(std::move(b));
    };
    add("hat", "event_whenflagclicked");
    add("loop", "control_repeat");
    add("times", "math_number");
    add("change", "data_changevariableby");
    add("num", "math_number");
    blocks.getBlock("hat")->next = "loop";
    blocks.getBlock("loop")->inputs.set("TIMES", {"times", ""});
    blocks.getBlock("loop")->inputs.set("SUBSTACK", {"change", ""});
    blocks.getBlock("times")->fields["NUM"].value = Value::num(3);
    blocks.getBlock("change")->fields["VARIABLE"].id = "x";
    blocks.getBlock("change")->inputs.set("VALUE", {"num", ""});
    blocks.getBlock("num")->fields["NUM"].value = Value::num(1);
    auto kernel = lowerNumericF64Script(
        ScratchScript::capture(blocks, blocks.getBlock("hat")),
        NumericF64LoweringOptions{false, true});
    assert(kernel && kernel->slotIds.size() == 1);
    return std::move(*kernel);
}

struct Fixture {
    Target target{nullptr};
    Fixture() {
        target.variables.emplace(
            "x", Variable::create("x", u"x", Variable::kScalarType, false));
    }
};

static uint64_t fakeSlice(uint64_t* slots, uint64_t* frame, uint64_t header) {
    const Value input{slots[0]};
    slots[0] = Value::num(input.asNumberUnchecked() + 1).bits;
    frame[0] = header;
    return 1; // every call ends at a repeat checkpoint
}

static uint64_t oldPath(Fixture& fixture, const ScratchNumericKernel& kernel,
                        int rounds) {
    const auto& program = kernel.executable;
    std::vector<uint64_t> frame(program.ssa.definitions.size() + 2, 0);
    const uint64_t header = program.yieldBoundaries[0].header;
    for (int i = 0; i < rounds; ++i) {
        if (!verifyExecutable(program)) return UINT64_MAX;
        ScratchNumericSlots staged(fixture.target, kernel.slotIds);
        if (!staged.valid()) return UINT64_MAX;
        std::vector<uint64_t> nextFrame = frame;
        if (fakeSlice(staged.data(), nextFrame.data(), header) != 1 ||
            !staged.commit()) return UINT64_MAX;
        frame = std::move(nextFrame);
    }
    return fixture.target.variables.at("x").value.bits;
}

static uint64_t newPath(Fixture& fixture, const ScratchNumericKernel& kernel,
                        int rounds) {
    NumericContinuation state(kernel);
    const uint64_t header = kernel.executable.yieldBoundaries[0].header;
    const auto runner = [header](uint64_t* slots, uint64_t* frame) {
        return fakeSlice(slots, frame, header);
    };
    for (int i = 0; i < rounds; ++i) {
        if (resumeNumericKernel(fixture.target, kernel, state, runner) !=
            NumericSliceResult::Yielded) return UINT64_MAX;
    }
    return fixture.target.variables.at("x").value.bits;
}

template <typename F>
static double measure(const ScratchNumericKernel& kernel,
                      int rounds, F&& path) {
    double bestNs = 1e30;
    for (int repetition = 0; repetition < 3; ++repetition) {
        Fixture fixture;
        const auto before = Clock::now();
        const uint64_t result = path(fixture, kernel, rounds);
        const auto after = Clock::now();
        assert(result == Value::num(double(rounds)).bits);
        const auto ns = std::chrono::duration<double, std::nano>(
            after - before).count() / double(rounds);
        if (ns < bestNs) bestNs = ns;
    }
    return bestNs;
}

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::stoi(argv[1]) : 10000;
    assert(rounds > 0 && rounds < 1000000);
    const ScratchNumericKernel kernel = makeKernel();
    // Alternating order reduces thermal- and frequency-ramp bias.
    const auto oldMs = measure(kernel, rounds, oldPath);
    const auto newMs = measure(kernel, rounds, newPath);
    std::cout << std::fixed << std::setprecision(1)
              << "host-only numeric checkpoint benchmark; rounds="
              << rounds << "\n"
              << "legacy (verify+allocate each yield): "
              << oldMs << " ns/iteration\n"
              << "reused (verify once+reuse buffers): "
              << newMs << " ns/iteration\n"
              << "speedup: " << oldMs / newMs << "x\n";
    return 0;
}
