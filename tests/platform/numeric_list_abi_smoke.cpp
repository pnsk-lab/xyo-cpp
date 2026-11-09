// Numeric-list ABI, fallback behavior, and explicit GC rooting regression.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/list_access.hpp"
#include "../../src/wasm/list_host_bridge.hpp"
#include "../../src/wasm/wasm_compile.hpp"
#include <fstream>
#include "../../src/rt/runtime_helpers.hpp"
#include "../../src/core/sstr.hpp"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

int main(int argc, char** argv) {
    using namespace xyo;
    using namespace xyo::ir;
    SList* rooted = SList::create();
    // The live list is owned by the mock runtime Variable represented by
    // this root provider. It may be collected only after we unregister it.
    Gc::get().addRootProvider(&rooted, [&rooted](Gc& gc) {
        gc.markPtr(rooted);
    });

    rooted->push(Value::num(12));
    rooted->push(Value::num(-0.0));
    rooted->push(Value::nan());
    assert(rooted->numericOnly);
    assert(readNumericListItem(rooted, 1.9).available);
    assert(readNumericListItem(rooted, 1.9).bits == Value::num(12).bits);
    assert(readNumericListItem(rooted, 2).bits == Value::num(-0.0).bits);
    assert(std::signbit(rt::listGet(rooted, Value::num(2)).asNumberUnchecked()));
    assert(readNumericListItem(rooted, 3).bits == val::kNan);
    assert(!readNumericListItem(rooted, 0).available);
    assert(!readNumericListItem(rooted, 4).available);
    assert(!readNumericListItem(rooted, std::nan("")).available);
    assert(!readNumericListItem(nullptr, 1).available);
    assert(rt::listGet(rooted, Value::num(1.9)).bits ==
           Value::num(12).bits);
    assert(rt::listGet(rooted, Value::num(4)).isEmptyString());

    // The shared value read never skips nonnumeric mutation feedback.
    rooted->replace0(0, Value::string(SStr::fromUtf8("123")));
    assert(!rooted->numericOnly);
    assert(!readNumericListItem(rooted, 1).available);
    assert(rt::listGet(rooted, Value::num(1)).isString());
    assert(rt::listGet(rooted, Value::string(SStr::fromUtf8("last"))).isNaN());
    // A monotonic flag never becomes true again after clear/replace.
    rooted->clear();
    rooted->push(Value::num(7));
    assert(!rooted->numericOnly);
    assert(rt::listGet(rooted, Value::num(1)).bits == Value::num(7).bits);

    // Collection traverses the list's values while the list itself is
    // rooted; stale element-buffer pointers must not be retained by callers.
    rooted->replace0(0, Value::string(SStr::fromUtf8("77")));
    Gc::get().collect();
    Value afterGc = rt::listGet(rooted, Value::num(1));
    assert(afterGc.isString());
    assert(afterGc.asStrPtr()->view() == u"77");

    Variable bound;
    bound.type = Variable::kListType;
    bound.list = rooted;
    std::vector<Variable*> bindings{&bound};
    uint64_t scratch = Value::num(1).bits;
    {
        wasm::NumericListTransaction txn(bindings);
        wasm::ScopedListBindings scope(txn);
        assert(!wasm::loadBoundNumericListItem(&scratch, 0));
        assert(scratch == Value::num(1).bits);
        assert(!wasm::loadBoundNumericListItem(&scratch, 1));
    }
    assert(!wasm::loadBoundNumericListItem(&scratch, 0));
    SList* numeric = SList::create();
    numeric->push(Value::num(12.75));
    Variable numericVar;
    numericVar.type = Variable::kListType;
    numericVar.list = numeric;
    std::vector<Variable*> numericBindings{&numericVar};
    Gc::get().addRootProvider(&numeric, [&numeric](Gc& gc) {
        gc.markPtr(numeric);
    });
    {
        wasm::NumericListTransaction txn(numericBindings);
        wasm::ScopedListBindings scope(txn);
        assert(wasm::loadBoundNumericListItem(&scratch, 0));
        assert(scratch == Value::num(12.75).bits);
        scratch = Value::num(9).bits;
        assert(!wasm::loadBoundNumericListItem(&scratch, 0));
        assert(scratch == Value::num(9).bits);
    }
    // Copy-on-write transaction: reads see speculative edits; the actual
    // list remains unmodified through a bailout. Multiple edits in order.
    {
        wasm::NumericListTransaction txn(numericBindings);
        wasm::ScopedListBindings scope(txn);
        double appendArgs[2] = {0.0, 20.0};
        assert(wasm::editBoundNumericList(
            appendArgs, 0, wasm::NumericListTransaction::Edit::Append));
        assert(numeric->len == 1);
        double idx = 2;
        assert(wasm::loadBoundNumericListItem(&idx, 0));
        assert(idx == 20.0);
        double replaceArgs[2] = {1.0, 99.0};
        assert(wasm::editBoundNumericList(
            replaceArgs, 0, wasm::NumericListTransaction::Edit::Replace));
        double deleteArgs[2] = {2.0, 0.0};
        assert(wasm::editBoundNumericList(
            deleteArgs, 0, wasm::NumericListTransaction::Edit::Delete));
        idx = 1;
        assert(wasm::loadBoundNumericListItem(&idx, 0));
        assert(idx == 99.0);
        assert(numeric->len == 1 && numeric->items[0].asNumberUnchecked() == 12.75);
        // Simulated Guard failure: no commit, so no runtime side effects.
    }
    assert(numeric->len == 1 && numeric->items[0].asNumberUnchecked() == 12.75);
    {
        wasm::NumericListTransaction txn(numericBindings);
        wasm::ScopedListBindings scope(txn);
        double appendArgs[2] = {0.0, 25.0};
        assert(wasm::editBoundNumericList(
            appendArgs, 0, wasm::NumericListTransaction::Edit::Append));
        txn.commit();
    }
    assert(numeric->len == 2 && numeric->items[1].asNumberUnchecked() == 25);
    assert(numeric->numericOnly && numeric->monitorDirty);
    // Sparse journal: a large numeric list must not be copied just to
    // append or replace one element. A delete materializes exactly once.
    SList* large = SList::create();
    Gc::get().addRootProvider(&large, [&large](Gc& gc) { gc.markPtr(large); });
    for (int i = 0; i < 50000; ++i) large->push(Value::num(i));
    Variable largeVar;
    largeVar.type = Variable::kListType;
    largeVar.list = large;
    std::vector<Variable*> largeBindings{&largeVar};
    {
        wasm::NumericListTransaction txn(largeBindings);
        wasm::ScopedListBindings scope(txn);
        assert(txn.stages[0].items.empty());
        double replace[2] = {25000, 42};
        double append[2] = {0, 99};
        assert(wasm::editBoundNumericList(
            replace, 0, wasm::NumericListTransaction::Edit::Replace));
        assert(wasm::editBoundNumericList(
            append, 0, wasm::NumericListTransaction::Edit::Append));
        assert(txn.stages[0].items.empty() && !txn.stages[0].materialized);
        assert(txn.stages[0].replacements.size() == 1);
        assert(txn.stages[0].appended.size() == 1);
        uint64_t found = 0;
        assert(txn.read(0, 25000, found) && found == Value::num(42).bits);
        assert(txn.read(0, 50001, found) && found == Value::num(99).bits);
        assert(large->len == 50000 &&
               large->items[24999].asNumberUnchecked() == 24999);
        // No commit: dropping the transaction rolls back all changes.
    }
    assert(large->len == 50000 &&
           large->items[24999].asNumberUnchecked() == 24999);
    {
        wasm::NumericListTransaction txn(largeBindings);
        wasm::ScopedListBindings scope(txn);
        double replace[2] = {25000, 42};
        double append[2] = {0, 99};
        assert(wasm::editBoundNumericList(
            replace, 0, wasm::NumericListTransaction::Edit::Replace));
        assert(wasm::editBoundNumericList(
            append, 0, wasm::NumericListTransaction::Edit::Append));
        txn.commit();
    }
    assert(large->len == 50001 &&
           large->items[24999].asNumberUnchecked() == 42 &&
           large->items[50000].asNumberUnchecked() == 99);
    {
        wasm::NumericListTransaction txn(largeBindings);
        wasm::ScopedListBindings scope(txn);
        double del[2] = {25000, 0};
        assert(wasm::editBoundNumericList(
            del, 0, wasm::NumericListTransaction::Edit::Delete));
        assert(txn.stages[0].materialized);
        assert(txn.stages[0].items.size() == 50000);
        txn.commit();
    }
    assert(large->len == 50000 &&
           large->items[24999].asNumberUnchecked() == 25000);
    // Deterministic property test against an eager reference vector.
    // Interleave append/replace/delete/read and verify sequential staging,
    // rollback, and successful publication after a mixed edit sequence.
    {
        std::vector<uint64_t> reference;
        reference.reserve(large->len + 300);
        for (uint32_t i = 0; i < large->len; ++i)
            reference.push_back(large->items[i].bits);
        wasm::NumericListTransaction txn(largeBindings);
        std::mt19937 engine(20261107);
        for (int step = 0; step < 240; ++step) {
            const uint32_t op = engine() % 4;
            const uint32_t position = uint32_t(engine() % reference.size());
            const uint64_t value = Value::num(double(step + 700)).bits;
            if (op == 0) {
                assert(txn.write(wasm::NumericListTransaction::Edit::Append,
                                 0, 0, value));
                reference.push_back(value);
            } else if (op == 1) {
                assert(txn.write(wasm::NumericListTransaction::Edit::Replace,
                                 0, double(position + 1), value));
                reference[position] = value;
            } else if (op == 2) {
                assert(txn.write(wasm::NumericListTransaction::Edit::Delete,
                                 0, double(position + 1), 0));
                reference.erase(reference.begin() + position);
            } else {
                uint64_t result = 0;
                assert(txn.read(0, double(position + 1), result));
                assert(result == reference[position]);
            }
            if (step % 31 == 0) {
                for (int sample = 0; sample < 16; ++sample) {
                    uint32_t pos = uint32_t(engine() % reference.size());
                    uint64_t result = 0;
                    assert(txn.read(0, double(pos + 1), result));
                    assert(result == reference[pos]);
                }
            }
        }
        // The live list cannot observe speculative updates.
        assert(large->len == 50000);
        txn.commit();
        assert(large->len == reference.size());
        for (size_t i = 0; i < reference.size(); ++i)
            assert(large->items[i].bits == reference[i]);
    }
    Gc::get().removeRootProvider(&large);
    Gc::get().removeRootProvider(&numeric);
    Gc::get().removeRootProvider(&rooted);

    if (argc == 2) {
        wasm::Program p;
        p.slotIds = {"result"};
        p.listIds = {"list_0"};
        p.nSlots = 1;
        const uint32_t index = p.exprNum(1);
        wasm::Expr get;
        get.k = wasm::EK::ListGet;
        get.a = index;
        get.var = 0;
        const uint32_t listExpr = p.expr(get);
        wasm::Stmt set;
        set.k = wasm::SK::SetVar;
        set.var = 0;
        set.expr = listExpr;
        p.main = {p.stmt(set)};
        for (bool host : {false, true}) {
            p.useHostMath = host;
            const auto bytes = wasm::compileProgram(p);
            assert(!bytes.empty());
            std::ofstream file(std::string(argv[1]) +
                (host ? "/list-host.wasm" : "/list-standalone.wasm"),
                std::ios::binary);
            assert(file);
            file.write(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
            assert(file);
        }
        // An append, a read of the staged item, a replacement and a
        // deletion must all be compiled through the same host transaction.
        wasm::Program edits;
        edits.slotIds = {"result"};
        edits.listIds = {"list_0"};
        edits.nSlots = 1;
        edits.useHostMath = true;
        wasm::Stmt append;
        append.k = wasm::SK::ListAppend;
        append.var = 0;
        append.extraExpr = edits.exprNum(11);
        edits.main.push_back(edits.stmt(append));
        wasm::Expr secondItem;
        secondItem.k = wasm::EK::ListGet;
        secondItem.var = 0;
        secondItem.a = edits.exprNum(2);
        wasm::Stmt save;
        save.k = wasm::SK::SetVar;
        save.var = 0;
        save.expr = edits.expr(secondItem);
        edits.main.push_back(edits.stmt(save));
        wasm::Stmt replace;
        replace.k = wasm::SK::ListReplace;
        replace.var = 0;
        replace.expr = edits.exprNum(1);
        replace.extraExpr = edits.exprNum(99);
        edits.main.push_back(edits.stmt(replace));
        wasm::Stmt erase;
        erase.k = wasm::SK::ListDelete;
        erase.var = 0;
        erase.expr = edits.exprNum(2);
        edits.main.push_back(edits.stmt(erase));
        const auto compiled = wasm::compileProgram(edits);
        assert(!compiled.empty());
        std::ofstream editsOut(std::string(argv[1]) + "/list-edits.wasm",
                               std::ios::binary);
        editsOut.write(reinterpret_cast<const char*>(compiled.data()),
                       static_cast<std::streamsize>(compiled.size()));
        assert(editsOut);
    } else assert(argc == 1);
    return 0;
}
