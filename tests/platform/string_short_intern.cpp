// Native GC/JIT ABI-independent short interner semantics and threading.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "core/cast.hpp"
#include "core/gc.hpp"
#include "core/sstr.hpp"
#include "rt/runtime_helpers.hpp"

namespace {
int failures = 0;
void check(bool passed, const char* label) {
    if (!passed) {
        ++failures;
        std::fprintf(stderr, "FAIL short intern: %s\n", label);
    }
}
}
int main(int argc, char** argv) {
    using namespace xyo;
    if (argc != 3) return 2;
    const bool enabled = std::strcmp(argv[1], "on") == 0;
    check(setStringLeafMode(argv[2]), "leaf mode");
    check(stringShortInternEnabled() == enabled, "environment selects intern");
    {
        Gc::DeferCollect defer;
        SStr* first = sstrConcat(u"a", u"b");
        SStr* second = sstrConcat(u"a", u"b");
        check((first == second) == enabled, "repeated join results shared");
        check(first != SStr::create(u"ab"), "plain constructors are not interned");
        check(first->view() == u"ab", "exact value");
        check(first->isLatin1() == (std::strcmp(argv[2], "latin1") == 0),
              "matches leaf representation");
        SStr* ascii = sstrFromAscii("ab");
        check(first != ascii, "normal numeric/literal conversion not interned");
        SStr* join = sstrConcat(u"a", u"b");
        check((first == join) == enabled, "UTF16 concat shares");
        SStr* concatAscii = sstrConcatAscii(u"a", "b");
        check((first == concatAscii) == enabled, "ASCII suffix shares");
        SStr* lhs = SStr::create(u"a");
        SStr* rhs = SStr::create(u"b");
        check((first == sstrConcatStrings(lhs, rhs)) == enabled,
              "leaf concat shares");
        check((first == sstrConcatStringAscii(lhs, "b")) == enabled,
              "leaf+ASCII shares");
        check(sstrConcat(u"a", u"b") != sstrConcat(u"A", u"B"),
              "case-sensitive keys");
        check(sstrConcat(u"0", u"1") != sstrConcat(u"1", u".0"),
              "different number spellings");
        const char16_t withNull[] = {u'a', 0, u'b'};
        SStr* nul = sstrConcat({withNull, 1}, {withNull + 1, 2});
        check(nul->len == 3 && nul->unitAt(1) == 0,
              "embedded NUL preserved");
        check((nul == sstrConcat({withNull, 1}, {withNull + 1, 2})) ==
                  enabled, "embedded NUL key is binary-safe");
        SStr* wide1 = sstrConcat(u"\u00e9", u"z");
        SStr* wide2 = sstrConcat(u"\u00e9", u"z");
        check(wide1 != wide2, "non-ASCII strings do not enter cache");
        check(cast::compare(Value::string(first),
                            Value::string(SStr::create(u"AB"))) == 0,
              "Scratch case-insensitive equality preserved");

        // Confirm the cache pin survives GC, even with no other references.
        // The source string is a separate static literal, not the cached body.
        first = nullptr;
        second = nullptr;
        ascii = nullptr;
        join = nullptr;
        concatAscii = nullptr;
    }
    if (enabled) {
        SStr* root = sstrConcat(u"x", u"y");
        Gc::get().collect();
        check(root == sstrConcat(u"x", u"y"), "interned leaf survives GC");
    }

    {
        // Publication under concurrent execution must not create a stale
        // pointer; repeated creation must always be equal to the canonical
        // string, though two racing allocators may briefly create duplicates.
        std::atomic<bool> good{true};
        std::vector<std::thread> threads;
        for (int t = 0; t < 6; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < 1000; ++i) {
                    SStr* a = sstrConcat(u"q", u":");
                    SStr* b = sstrConcat(u"q", u":");
                    if (a->view() != u"q:" || b->view() != u"q:" ||
                        (enabled && a != b)) good.store(false);
                }
            });
        }
        for (auto& t : threads) t.join();
        check(good.load(), "concurrent reads and admissions");
    }
    if (enabled) {
        SStr* root = sstrConcat(u"q", u":");
        check(root == sstrConcat(u"q", u":"), "cache remains valid after threading");
    }

    const bool initial = stringShortInternEnabled();
    setStringShortInternEnabled(false);
    check(sstrConcat(u"a", u"b") != sstrConcat(u"a", u"b"),
          "runtime disable avoids lookups");
    setStringShortInternEnabled(true);
    check(sstrConcat(u"a", u"b") == sstrConcat(u"a", u"b"),
          "runtime enable restores sharing");
    setStringShortInternEnabled(initial);
    std::printf("short intern %s leaf=%s: %s\n", argv[1], argv[2],
                failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
