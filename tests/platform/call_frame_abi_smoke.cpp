// Same CallFrame ABI must behave byte-identically in native C++ and
// WebAssembly. Keep all assertions enabled in Release builds.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/scratch_call_frame.hpp"

#include <cassert>
#include <bit>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

using namespace xyo::ir;

static uint64_t bits(double number) {
    return std::bit_cast<uint64_t>(number);
}

int main() {
    static_assert(CallFrameAbi::kHeaderWords == 5);
    static_assert(CallFrameAbi::argumentWord(0) == 5);
    static_assert(CallFrameAbi::localWord(2, 3) == 10);
    static_assert(CallFrameAbi::kLoopCounterLocal == 0);
    static_assert(CallFrameAbi::kNoLoopCounter == UINT64_MAX);
    static_assert(CallFrameAbi::frameWords(2, 4) == 11);
    static_assert(CallFrameAbi::byteOffset(
        CallFrameAbi::kReturnPcWord) == 8);

    ScratchTransferGraph graph;
    graph.valid = true;
    graph.sites.push_back({
        ScratchTransferKind::ProcedureEntry, "zeta-def", "", "zeta-def"
    });
    graph.sites.push_back({
        ScratchTransferKind::ProcedureEntry, "alpha-def", "", "alpha-def"
    });
    graph.sites.push_back({
        ScratchTransferKind::Return, "zeta-def", "", "zeta-def"
    });
    const auto table = ScratchProcedureTable::capture(graph);
    assert(table.valid() && table.size() == 2);
    assert(table.idFor("alpha-def") == 1);
    assert(table.idFor("zeta-def") == 2);
    assert(!table.idFor("unknown"));
    assert(table.sourceFor(1) && *table.sourceFor(1) == "alpha-def");
    assert(!table.sourceFor(0) && !table.sourceFor(3));
    assert(!ScratchProcedureTable::capture({}).valid());

    const CallFrameLayout layout{2, 4, 5};
    assert(layout.valid());
    assert(layout.strideWords() == 11);
    assert(layout.arenaWords() == 55);
    ScratchCallStack stack(layout);
    assert(stack.valid() && stack.empty() && stack.depth() == 0);
    assert(!stack.top() && !stack.argument(0) && !stack.local(0));
    assert(!stack.push(1, CallFrameAbi::kNoPc, 0, {}, 2));
    assert(stack.push(0, CallFrameAbi::kNoPc, 0, {}, 2));
    assert(stack.depth() == 1 && stack.validateImage());
    assert(stack.top()->procedureId == 0);
    assert(stack.setResumePc(10));
    assert(stack.setLocal(0, bits(19.0)));
    assert(stack.setLocal(1, bits(-0.0)));
    assert(stack.local(0) == bits(19.0));
    assert(stack.local(1) == bits(-0.0));
    assert(!stack.local(2) && !stack.argument(0));

    // Precondition for the generated code: a stable contiguous array of
    // words with precisely the same word offsets on native and WASM.
    const uint64_t* const stableBase = stack.words();
    const uint64_t oldRevision = stack.revision();
    const uint64_t argsA[] = {bits(2.0), bits(3.0)};
    assert(stack.push(1, 14, 0, argsA, 3));
    assert(stack.words() == stableBase); // no vector realloc on call
    assert(stack.revision() > oldRevision);
    assert(stack.argument(0) == bits(2.0));
    assert(stack.argument(1) == bits(3.0));
    assert(!stack.argument(2));
    assert(stack.setLocal(0, bits(97.0)));
    assert(stack.setResumePc(5));
    assert(stack.wordOffset(1, CallFrameAbi::kReturnPcWord) == 12);
    assert(stack.words()[12] == 14);
    assert(stack.wordOffset(1, CallFrameAbi::localWord(2, 0)) == 18);
    assert(stack.words()[18] == bits(97.0));

    // A nested child must restore the parent continuation and leave all
    // parent arguments and SSA locals unmodified by the child.
    const uint64_t argsB[] = {bits(8.0)};
    assert(stack.push(2, 27, 0, argsB, 2));
    assert(stack.setResumePc(1));
    assert(stack.setLocal(0, bits(12345.0)));
    assert(stack.pop() == 27);
    assert(stack.depth() == 2);
    assert(stack.top()->resumePc == 27);
    assert(stack.local(0) == bits(97.0));
    assert(stack.argument(0) == bits(2.0));
    assert(stack.argument(1) == bits(3.0));
    assert(stack.pop() == 14);
    assert(stack.depth() == 1);
    assert(stack.top()->resumePc == 14);
    assert(stack.local(0) == bits(19.0)); // restored root SSA state
    assert(stack.local(1) == bits(-0.0));

    // Yield: first slice may publish a valid staged continuation. The next
    // slice starts from the exact same stored PC and exact bits of locals.
    {
        ScratchCallTransaction transaction(stack);
        assert(transaction.staged().setResumePc(41));
        assert(transaction.staged().setLocal(0, bits(222.0)));
        assert(stack.top()->resumePc == 14); // not published yet
        assert(transaction.commit());
    }
    assert(stack.top()->resumePc == 41);
    assert(stack.local(0) == bits(222.0));

    // Failure/rollback: speculative writes remain completely invisible.
    const uint64_t baselineRevision = stack.revision();
    {
        ScratchCallTransaction transaction(stack);
        assert(transaction.staged().setResumePc(99));
        assert(transaction.staged().setLocal(0, bits(9999.0)));
        transaction.discard(); // e.g. failed WASM runtime import
        assert(!transaction.commit());
    }
    assert(stack.revision() == baselineRevision);
    assert(stack.local(0) == bits(222.0));
    assert(stack.top()->resumePc == 41);

    // Publishing a previously staged frame is rejected if its owner was
    // changed by another scheduler step in the meantime.
    {
        ScratchCallTransaction stale(stack);
        assert(stale.staged().setResumePc(55));
        assert(stack.setResumePc(56));
        assert(!stale.commit());
    }
    assert(stack.top()->resumePc == 56);

    // Never commit an untrusted WASM frame image with invalid metadata.
    {
        ScratchCallTransaction broken(stack);
        const auto word = broken.staged().wordOffset(
            0, CallFrameAbi::kArgumentCountWord);
        assert(word);
        auto* staging = const_cast<uint64_t*>(broken.staged().words());
        staging[*word] = UINT64_MAX;
        assert(!broken.staged().validateImage());
        assert(!broken.commit());
        assert(stack.top()->resumePc == 56);
    }
    assert(!stack.setLocal(5, 1));
    assert(!stack.setResumePc(CallFrameAbi::kNoPc));

    // Max stack depth is a real limit; pushing one more call never modifies
    // live state or the frame buffer.
    for (unsigned depth = 1; depth < layout.maximumDepth; ++depth)
        assert(stack.push(1, 42, 0, argsA, 2));
    assert(stack.depth() == layout.maximumDepth);
    const uint64_t fullRevision = stack.revision();
    assert(!stack.push(1, 42, 0, argsA, 2));
    assert(stack.revision() == fullRevision);
    while (!stack.empty()) assert(stack.pop());
    assert(!stack.pop() && stack.validateImage());

    // Layout bounds; no overflow or allocations for nonsensical layouts.
    assert(!ScratchCallStack({33, 0, 2}).valid());
    assert(!ScratchCallStack({0, 4097, 2}).valid());
    assert(!ScratchCallStack({0, 0, 0}).valid());
    assert(!ScratchCallStack({0, 0, 1025}).valid());

    // Executing the same source file natively and under WASM must produce
    // an identical output digest of the portable layout + raw SSA bits.
    ScratchCallStack digestStack(layout);
    assert(digestStack.push(0, CallFrameAbi::kNoPc, 0, {}, 2));
    assert(digestStack.setLocal(0, bits(19.0)));
    assert(digestStack.setLocal(1, bits(-0.0)));
    assert(digestStack.push(1, 31, 9, argsA, 3));
    assert(digestStack.setLocal(0, bits(12345.0)));
    uint64_t digest = 14695981039346656037ull;
    for (size_t i = 0; i < digestStack.arenaWords(); ++i) {
        digest ^= digestStack.words()[i];
        digest *= 1099511628211ull;
    }
    std::cout << "PASS call-frame ABI: nested call/return, yield, rollback, "
                 "stale publication, bounds, deterministic numeric words\n";
    std::cout << "frame_digest=" << std::hex << digest << "\n";
}
