// リスト長上限（--list-limit / XYO_LIST_LIMIT）。
// scratch の 200000 上限と同じ「insert は末尾を落として長さを保つ」挙動を
// SList と rt::listInsert の両方で検証する。
#include <cstdio>
#include <vector>
#include <random>

#include "core/slist.hpp"
#include "rt/runtime_helpers.hpp"

namespace {
using namespace xyo;
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL list limit: %s\n", what);
    }
}
} // namespace

int test_list_limit_main() {
    // 上限なし（既定）は今まで通り伸びる
    xyo_list_limit = 0;
    {
        SList* l = SList::create();
        for (int i = 1; i <= 5; ++i) l->push(Value::numCanonical(i));
        check(l->len == 5, "unlimited push");
        l->insert0(0, Value::numCanonical(0));
        check(l->len == 6, "unlimited insert");
    }

    // push は上限で落とす
    xyo_list_limit = 3;
    {
        SList* l = SList::create();
        for (int i = 1; i <= 5; ++i) l->push(Value::numCanonical(i));
        check(l->len == 3, "push caps at limit");
        check(l->get0(0).asNumberUnchecked() == 1 &&
                  l->get0(2).asNumberUnchecked() == 3,
              "push keeps the first items");

        // 先頭への insert は末尾を落として長さを保つ
        l->insert0(0, Value::numCanonical(9));
        check(l->len == 3, "insert at 1 keeps length");
        check(l->get0(0).asNumberUnchecked() == 9 &&
                  l->get0(1).asNumberUnchecked() == 1 &&
                  l->get0(2).asNumberUnchecked() == 2,
              "insert at 1 shifts and drops the tail");

        // 末尾への insert も落とされる（長さは変わらない）
        l->insert0(l->len, Value::numCanonical(7));
        check(l->len == 3, "insert at last keeps length");
        check(l->get0(2).asNumberUnchecked() == 2, "append is dropped");
    }

    // rt::listInsert（JIT/インタプリタ共通の helper）も同じ挙動
    {
        SList* l = SList::create();
        for (int i = 1; i <= 3; ++i) l->push(Value::numCanonical(i));
        rt::listInsert(l, Value::string(SStr::fromUtf8("last")),
                       Value::numCanonical(4));
        check(l->len == 3, "listInsert last keeps length");
        rt::listInsert(l, Value::numCanonical(1), Value::numCanonical(0));
        check(l->len == 3, "listInsert 1 keeps length");
        check(l->get0(0).asNumberUnchecked() == 0 &&
                  l->get0(1).asNumberUnchecked() == 1 &&
                  l->get0(2).asNumberUnchecked() == 2,
              "listInsert shifts at the front");
    }

    // 上限を外せばまた伸びる
    xyo_list_limit = 0;
    {
        SList* l = SList::create();
        for (int i = 0; i < 10; ++i) l->push(Value::numCanonical(i));
        check(l->len == 10, "limit can be cleared");
    }

    // Storage variants must preserve the JIT-visible contiguous items ABI,
    // Scratch list semantics and clone behavior across SBO promotion.
    const uint32_t savedMode = xyo_list_storage_mode;
    for (uint32_t mode = 0; mode < 8; ++mode) {
        xyo_list_storage_mode = mode;
        SList* l = SList::create();
        const bool shouldInline = mode == 1 || mode == 2;
        check(l->usesInline() == shouldInline, "storage initial layout");
        for (int i = 0; i < 64; ++i) l->push(Value::numCanonical(i));
        check(!l->usesInline(), "storage grows to heap");
        check(l->len == 64, "storage push length");
        for (int i = 0; i < 64; ++i)
            check(l->get0(i).asNumberUnchecked() == i, "storage push contents");

        l->insert0(4, Value::numCanonical(1234));
        check(l->len == 65 && l->get0(4).asNumberUnchecked() == 1234 &&
              l->get0(5).asNumberUnchecked() == 4, "storage insert");
        l->erase0(4);
        check(l->len == 64 && l->get0(4).asNumberUnchecked() == 4,
              "storage erase");
        l->replace0(0, Value::emptyStr());
        check(!l->numericOnly, "storage numeric feedback");

        SList* copy = l->clone();
        check(copy->len == l->len && !copy->numericOnly &&
              copy->get0(63).asNumberUnchecked() == 63, "storage clone");
        l->clear();
        check(l->len == 0 && copy->len == 64, "storage clone independence");

        if (mode >= 4) {
            // Exercise several chunk splits and arbitrary edit positions.
            SList* randomList = SList::create();
            std::vector<Value> reference;
            std::mt19937 random(0x12345);
            for (int i = 0; i < 3000; ++i) {
                if (reference.empty() || i % 3 != 0) {
                    const uint32_t at = uint32_t(random() % (reference.size() + 1));
                    const Value v = Value::numCanonical(double(i));
                    randomList->insert0(at, v);
                    reference.insert(reference.begin() + at, v);
                } else {
                    const uint32_t at = uint32_t(random() % reference.size());
                    randomList->erase0(at);
                    reference.erase(reference.begin() + at);
                }
                check(randomList->len == reference.size(),
                      "random edits preserve length");
                if (i % 17 == 0) {
                    bool matches = true;
                    for (size_t j = 0; j < reference.size(); ++j)
                        if (randomList->get0(uint32_t(j)) != reference[j]) {
                            matches = false;
                            break;
                        }
                    check(matches, "random edits preserve ordering");
                }
            }
        }
        if (shouldInline) {
            SList* small = SList::create();
            check(small->usesInline(), "small list inline");
            SList* smallCopy = small->clone();
            check(smallCopy->usesInline(), "small list clone inline");
            for (int i = 0; i < (mode == 1 ? 4 : 8); ++i)
                small->push(Value::numCanonical(i));
            check(small->usesInline(), "inline capacity boundary");
            small->push(Value::numCanonical(99));
            check(!small->usesInline() &&
                  small->get0(0).asNumberUnchecked() == 0 &&
                  small->get0(small->len - 1).asNumberUnchecked() == 99,
                  "inline promotion");
        }
    }
    xyo_list_storage_mode = 7;
    {
        SList* l = SList::create();
        std::vector<Value> reference;
        for (int i = 0; i < 1000; ++i) {
            const Value v = Value::numCanonical(double(i));
            l->push(v);
            reference.push_back(v);
        }
        check(l->isContiguous(), "adaptive starts flat");
        const uint64_t flatEpoch = l->storageEpoch;
        for (int i = 0; i < 48; ++i) {
            const Value v = Value::numCanonical(double(10000 + i));
            l->insert0(499, v);
            reference.insert(reference.begin() + 499, v);
            l->erase0(500);
            reference.erase(reference.begin() + 500);
        }
        check(l->extra != nullptr && l->adaptiveBackend == 5,
              "adaptive selects gap for localized edits");
        check(l->storageEpoch > flatEpoch,
              "adaptive promotion advances storage epoch");
        const uint64_t gapEpoch = l->storageEpoch;
        bool matches = l->len == reference.size();
        for (size_t i = 0; i < reference.size() && matches; ++i)
            matches &= l->get0(uint32_t(i)) == reference[i];
        check(matches, "adaptive gap conversion preserves data");
        SList* clone = l->clone();
        check(clone->isContiguous() && clone->len == l->len,
              "adaptive clone starts flat");
        for (size_t i = 0; i < reference.size() && matches; ++i)
            matches &= clone->get0(uint32_t(i)) == reference[i];
        check(matches, "adaptive clone preserves data");
        for (int i = 0; i < 64; ++i)
            l->noteAdaptiveRead();
        check(l->isContiguous(), "adaptive demotes on read-heavy phase");
        check(l->storageEpoch > gapEpoch,
              "adaptive demotion advances storage epoch");
        matches = l->len == reference.size();
        for (size_t i = 0; i < reference.size() && matches; ++i)
            matches &= l->get0(uint32_t(i)) == reference[i];
        check(matches, "adaptive demotion preserves data");
        for (int i = 0; i < 28; ++i) {
            l->insert0(499, Value::numCanonical(double(i)));
            l->erase0(500);
        }
        check(l->extra && l->adaptiveBackend == 5,
              "adaptive repromotes after edits");
        const uint64_t lastEpoch = l->storageEpoch;
        l->clear();
        check(l->storageEpoch > lastEpoch,
              "adaptive clear advances storage epoch");
        check(l->isContiguous() && l->len == 0,
              "adaptive clear restores flat representation");
        l->push(Value::numCanonical(42));
        check(l->get0(0).asNumberUnchecked() == 42,
              "adaptive clear allows reuse");
    }
    {
        SList* randomList = SList::create();
        for (int i = 0; i < 1000; ++i)
            randomList->push(Value::numCanonical(double(i)));
        std::mt19937 engine(42);
        const uint64_t originalEpoch = randomList->storageEpoch;
        for (int i = 0; i < 200; ++i) {
            const uint32_t pos = 64 + engine() % (randomList->len - 128);
            randomList->insert0(pos, Value::numCanonical(double(i)));
        }
        check(randomList->extra && randomList->adaptiveBackend == 6,
              "adaptive selects chunks for dispersed edits");
        check(randomList->storageEpoch > originalEpoch,
              "chunked migration advances storage epoch");
        check(randomList->len == 1200,
              "adaptive chunked edits maintain length");
        SList* chunkClone = randomList->clone();
        check(chunkClone->isContiguous() &&
              chunkClone->len == randomList->len,
              "adaptive chunked clone materializes flat");
        bool clonedEqual = true;
        for (uint32_t i = 0; i < randomList->len && clonedEqual; ++i)
            clonedEqual &= chunkClone->get0(i) == randomList->get0(i);
        check(clonedEqual, "adaptive chunked clone preserves every value");
        const uint64_t chunkEpoch = randomList->storageEpoch;
        randomList->makeContiguous();
        check(randomList->isContiguous() && randomList->len == 1200,
              "adaptive chunked can be flattened");
        check(randomList->storageEpoch > chunkEpoch,
              "chunked demotion advances storage epoch");
    }
    // Bulk migration and flat materialization must preserve the exact bit
    // pattern across gap boundaries and chunk boundaries.
    for (uint32_t mode : {4u, 5u, 6u}) {
        for (uint32_t n : {0u, 1u, 2u, 127u, 128u, 129u, 1000u}) {
            std::vector<Value> original;
            for (uint32_t i = 0; i < n; ++i)
                original.push_back(i % 13 == 0 ? Value::emptyStr() :
                                   i % 17 == 0 ? Value::boolean(false) :
                                   Value::numCanonical(double(i) + 0.25));
            const Value dummy = Value::numCanonical(0);
            const Value* source = n ? original.data() : &dummy;
            for (uint32_t focus : {0u, n / 2, n}) {
                ListStore* store = makeListStore(mode, n);
                store->assignFlat(source, n, focus);
                check(store->size() == n, "bulk assign size");
                std::vector<Value> copied(n + 2, Value::undefined());
                store->copyFlat(copied.data() + 1);
                bool equal = copied.front().isUndefined() &&
                             copied.back().isUndefined();
                for (uint32_t i = 0; i < n; ++i)
                    equal &= copied[i + 1] == original[i];
                check(equal, "bulk assign/copy preserves bits and boundaries");
                if (n > 0) {
                    store->insert(focus, Value::numCanonical(123456));
                    store->erase(focus);
                    store->copyFlat(copied.data() + 1);
                    bool same = store->size() == n;
                    for (uint32_t i = 0; i < n; ++i)
                        same &= copied[i + 1] == original[i];
                    check(same, "bulk copy survives localized edits");
                }
                delete store;
            }
        }
    }
    xyo_list_storage_mode = savedMode;

    std::printf("test_list_limit: %d checks, %d failures\n", checks,
                failures);
    return failures == 0 ? 0 : 1;
}
