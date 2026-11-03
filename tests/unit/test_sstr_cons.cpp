// 連結ノード（cons string）の意味論とライフサイクル。
//
// `s = s ++ "ab"` は V8 の cons string と同じく「コピーせずに left/right を
// 持つノード」を返し、本文は view()/data() が初めて必要になったときだけ
// materialize する。ここでは (1) 長さ・本文・大小比較・数値変換・リスト経由の
// 挙動が flat と同じであること、(2) 深い鎖でも flatten / GC マークが
// 再帰でスタックを溢れさせないこと、(3) flatten 後の GC で子が回収されても
// 本体が壊れないこと、を確認する。
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>
#include <string>
#include <vector>

#include "core/cast.hpp"
#include "core/gc.hpp"
#include "core/slist.hpp"
#include "core/sstr.hpp"
#include "rt/runtime_helpers.hpp"

namespace {
using namespace xyo;
int checks = 0, failures = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; \
    std::printf("FAIL sstr cons %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

std::u16string u16(const std::string& s) { return {s.begin(), s.end()}; }

Value ns(const char* s) { return Value::string(SStr::create(u16(s))); }

/// s = s ++ "x" を n 回（Scratch の join と同じ経路）
SStr* chain(const char* unit, int n) {
    Value acc = Value::emptyStr();
    for (int i = 0; i < n; ++i) acc = rt::join(acc, ns(unit));
    return cast::toString(acc);
}
} // namespace

int test_sstr_cons_main() {
    // --- 基本: flat と同じ見え方 -------------------------------------
    {
        SStr* s = chain("ab", 2);            // "" + "ab" + "ab" = "abab"
        CHECK(s->len == 4);
        CHECK(s->view() == u"abab");
        // 8 連結（16 文字）を超えると cons になる — まだ flatten していない
        SStr* c = chain("ab", 12);           // 24 文字
        CHECK(c->len == 24);
        CHECK(c->isCons());
        CHECK(c->body == nullptr);           // 遅延（まだ本文を作っていない）
        std::u16string want;
        for (int i = 0; i < 12; ++i) want += u"ab";
        CHECK(c->view() == want);
        CHECK(c->body != nullptr);           // view が materialize した
        CHECK(c->view() == want);            // 2 回目も同じ
    }

    // --- Direct cons indexing: UTF-16 units, both branches, bounded depth ---
    {
        SStr* left = SStr::create(u"abcdefghijklmno");
        SStr* right = SStr::create(u"\xD83D\xDE00z");
        SStr* c = sstrCons(left, right);
        const std::u16string expected = u"abcdefghijklmno\xD83D\xDE00z";
        for (uint32_t i = 0; i < c->len; ++i) CHECK(c->unitAt(i) == expected[i]);
        CHECK(c->body == nullptr);
        c->view();
        for (uint32_t i = 0; i < c->len; ++i) CHECK(c->unitAt(i) == expected[i]);
        // A deep left- or right-heavy tree uses an iterative bounded fallback.
        SStr* a = left;
        SStr* b = left;
        for (int i = 0; i < 200; ++i) {
            a = sstrCons(a, right);
            b = sstrCons(right, b);
        }
        CHECK(a->unitAt(0) == u'a');
        CHECK(a->body != nullptr);  // deep fallback caches the original root
        CHECK(a->unitAt(a->len - 1) == u'z');
        CHECK(b->unitAt(0) == char16_t(0xD83D));
        CHECK(b->unitAt(b->len - 1) == u'o');
        CHECK(cast::toString(rt::letterOf(Value::num(16.75), Value::string(c)))->view()
              == std::u16string_view(expected.data() + 15, 1));
        CHECK(rt::letterOf(Value::num(-1), Value::string(c)) == Value::emptyStr());
    }

    // --- 数値変換・比較・検索は view 経由で flat と同じ ---------------
    {
        SStr* c = chain("12", 4);            // "12121212"
        CHECK(c->numState == 0);
        CHECK(cast::toNumber(Value::string(c)) == 12121212.0);
        CHECK(c->loadNumState() == 1 || c->loadNumState() == 0);
        CHECK(cast::compare(Value::string(c), Value::string(SStr::create(u"12121213"))) < 0);
        Value needle = Value::string(SStr::create(u"2121"));
        CHECK(rt::strContains(Value::string(c), needle));
        CHECK(cast::toString(rt::letterOf(Value::num(3), Value::string(c)))->view() == u"1");
        CHECK(rt::strLength(Value::string(c)) == 8.0);
    }

    // --- 空文字列・混合（数値＋文字列）------------------------------
    {
        Value a = Value::string(SStr::create(u""));
        Value b = Value::num(42);
        Value r = rt::join(a, b);
        CHECK(cast::toString(r)->view() == u"42");
        Value r2 = rt::join(Value::string(SStr::create(u"x")), Value::num(3.5));
        CHECK(cast::toString(r2)->view() == u"x3.5");
        // 数値側が文字列化される join は従来どおり flat
        CHECK(!cast::toString(r2)->isCons());
    }

    // --- リストに入れた cons ----------------------------------------
    {
        SList* list = SList::create();
        SStr* c = chain("z", 20);
        list->push(Value::string(c));
        CHECK(list->len == 1);
        CHECK(cast::toString(list->items[0])->view() == u16(std::string(20, 'z')));
        CHECK(rt::listContents(list).asStrPtr()->view() == u16(std::string(20, 'z')));
        // 検索は compare 経由（view が flatten 済みでも同じ）
        CHECK(rt::listIndexOf(list, Value::string(SStr::create(u16(std::string(20, 'z'))))) == 1.0);
    }

    // --- 深い鎖: flatten もマークも再帰しない ------------------------
    {
        constexpr int kDepth = 40000;
        SStr* deep = chain("q", kDepth);
        CHECK(deep->len == uint32_t(kDepth));
        Gc::get().collect();                 // 根 from スタック（保守的走査）
        CHECK(deep->view() == u16(std::string(kDepth, 'q')));
        CHECK(deep->len == uint32_t(kDepth));
        Gc::get().collect();                 // flatten 後にもう一度
        CHECK(deep->view() == u16(std::string(kDepth, 'q')));
    }

    // --- flatten 前後での GC: 子が回収されても本体は無事 ------------
    {
        SStr* c = nullptr;
        {
            Value root = Value::emptyStr();
            for (int i = 0; i < 40; ++i)
                root = rt::join(root, ns("abcdefgh"));   // 320 文字の鎖
            c = cast::toString(root);
            CHECK(c->isCons());
            // ここで collect: 子ノードはルート（スタック上の Value）から
            // 辿れるので生き残る必要がある。
            Gc::get().collect();
            CHECK(c->len == 320);
        }
        // 依然として参照しているので flatten できる
        CHECK(c->view().size() == 320);
    }

    // Numeric append must not flatten the prefix, and must preserve formatting.
    {
        SStr* prefix = chain("ab", 1000);
        Value result = rt::join(Value::string(prefix), Value::num(12.5));
        CHECK(result.asStrPtr()->isCons());
        CHECK(prefix->body == nullptr);
        CHECK(result.asStrPtr()->view() == std::u16string(prefix->view()) + u"12.5");
        Value mixed = Value::emptyStr();
        std::u16string mixedWant;
        for (int i = 0; i < 2000; ++i) {
            double d = (i % 9 - 4) / 4.0;
            mixed = rt::join(mixed, Value::num(d));
            mixedWant += jsnum::toString16(d);
        }
        Gc::get().collect();
        CHECK(mixed.asStrPtr()->view() == mixedWant);
        CHECK(rt::join(ns(""), ns("keep")).asStrPtr()->view() == u"keep");
        CHECK(rt::join(Value::emptyStr(), Value::num(-0.0)).asStrPtr()->view() == u"0");
        CHECK(rt::join(ns("prefix-prefix-prefix"), Value::num(
            std::numeric_limits<double>::infinity())).asStrPtr()->view() ==
              u"prefix-prefix-prefixInfinity");
    }

    // A short non-interned numeric prefix allocates only the final result.
    {
        Gc::DeferCollect guard;
        Value suffix = ns(":");
        const size_t before = Gc::get().objectCount();
        Value result = rt::join(Value::num(1.25), suffix);
        CHECK(result.asStrPtr()->view() == u"1.25:");
        CHECK(Gc::get().objectCount() == before + 1);
        for (double d : {-1.25, 0.0, -0.0, 1e-6, 1e-7, 1e20, 1e21,
                         9007199254740992.0, std::numeric_limits<double>::infinity(),
                         -std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
            Value joined = rt::join(Value::num(d), suffix);
            CHECK(joined.asStrPtr()->view() == jsnum::toString16(d) + u":");
        }
    }

    // A seeded cache must match a separately parsed copy of the actual text,
    // not the original input (notably String(-0) reparses to positive zero).
    {
        uint64_t bits = 0;
        for (int i = 0; i < 4096; ++i) {
            bits = bits * 6364136223846793005ULL + 1442695040888963407ULL;
            double d = std::bit_cast<double>(bits);
            const std::u16string formatted = jsnum::toString16(d);
            for (const auto suffix : {u":", u"\xD83D\xDE00", u"long-unicode-\x3042-tail"}) {
                Value prefixed = rt::join(Value::num(d), Value::string(SStr::create(suffix)));
                CHECK(prefixed.asStrPtr()->view() == formatted + suffix);
            }
            Value outputs[] = {cast::toStringValue(Value::num(d)),
                               Value::string(cast::toString(Value::num(d))),
                               rt::join(Value::num(d), Value::emptyStr()),
                               rt::join(ns(""), Value::num(d))};
            for (Value v : outputs) {
                CHECK(v.isString());
                SStr* s = v.asStrPtr();
                CHECK(s->view() == jsnum::toString16(d));
                Value fresh = Value::string(SStr::create(s->view()));
                double parsed = cast::toNumberOrNaN(fresh);
                double cached = cast::toNumberOrNaN(v);
                CHECK((std::isnan(parsed) && std::isnan(cached)) ||
                      std::bit_cast<uint64_t>(parsed) == std::bit_cast<uint64_t>(cached));
                CHECK(s->loadNumState() == fresh.asStrPtr()->loadNumState());
            }
        }
        for (double d : {0.0, -0.0, std::numeric_limits<double>::infinity(),
                         -std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
            Value v = cast::toStringValue(Value::num(d));
            Value fresh = Value::string(SStr::create(v.asStrPtr()->view()));
            double a = cast::toNumberOrNaN(v), b = cast::toNumberOrNaN(fresh);
            CHECK((std::isnan(a) && std::isnan(b)) ||
                  std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b));
            CHECK(v.asStrPtr()->loadNumState() == fresh.asStrPtr()->loadNumState());
        }
    }

    // Small integers are interned like V8's number-string cache; the shared
    // entries must still be valid after a collection (they are GC roots).
    {
        SStr* seven = cast::toString(Value::num(7));
        CHECK(seven->view() == u"7");
        CHECK(seven == cast::toString(Value::num(7)));
        CHECK(cast::toString(Value::num(-0.0)) == cast::toString(Value::num(0.0)));
        CHECK(cast::toString(Value::num(-0.0))->view() == u"0");
        CHECK(cast::toString(Value::num(1023))->view() == u"1023");
        CHECK(cast::toString(Value::num(-128))->view() == u"-128");
        // Outside the cache the result must be a fresh, equal object.
        SStr* over = cast::toString(Value::num(1024));
        CHECK(over->view() == u"1024");
        CHECK(over != cast::toString(Value::num(1024)));
        SStr* fraction = cast::toString(Value::num(1.5));
        CHECK(fraction->view() == u"1.5");
        CHECK(fraction != cast::toString(Value::num(1.5)));
        Gc::get().collect();
        CHECK(cast::toString(Value::num(7))->view() == u"7");
        CHECK(cast::toString(Value::num(-1)) == cast::toString(Value::num(-1)));
    }

    // Multiple cold publishers and readers must observe fully initialized data.
    {
        Gc::DeferCollect guard;
        for (int round = 0; round < 8; ++round) {
            SStr* root = chain("abcdefgh", 1000);
            std::u16string want;
            for (int i = 0; i < 1000; ++i) want += u"abcdefgh";
            std::atomic<bool> start{false}, ok{true};
            std::vector<std::thread> workers;
            for (int i = 0; i < 4; ++i) workers.emplace_back([&] {
                while (!start.load(std::memory_order_acquire)) {}
                for (int j = 0; j < 10; ++j)
                    if (root->view() != want) ok.store(false);
            });
            start.store(true, std::memory_order_release);
            for (auto& worker : workers) worker.join();
            CHECK(ok.load());
        }
    }

    // Long, unflattened ropes must not recurse through every cons node when
    // computing the ASCII property. An earlier recursive implementation
    // overflowed the native stack on a sufficiently deep left/right chain.
    {
        Gc::DeferCollect guard;
        constexpr int kDepth = 60000;
        SStr* left = SStr::create(u"a");
        SStr* right = SStr::create(u"a");
        SStr* unit = SStr::create(u"a");
        for (int i = 0; i < kDepth; ++i) {
            left = sstrCons(left, unit);
            right = sstrCons(unit, right);
        }
        CHECK(left->body == nullptr);
        CHECK(right->body == nullptr);
        CHECK(left->isAscii());
        CHECK(right->isAscii());
        CHECK(left->body == nullptr);
        CHECK(right->body == nullptr);
        CHECK(left->left->isAscii()); // intermediate cache from postorder walk
        CHECK(right->right->isAscii());
        CHECK(sstrCons(left, SStr::create(u"\u00e9"))->isAscii() == false);
    }

    // Scratch applies Unicode toLowerCase *before* includes. UTF-16 length
    // checks on the original operands would reject an expanding lowercase.
    {
        SStr* expanded = SStr::create(u"\u0130");
        SStr* needle = SStr::create(u"i\u0307");
        CHECK(sstrContains(expanded, needle));
        CHECK(sstrContains(expanded->view(), needle->view()));
        CHECK(rt::strContains(Value::string(expanded), Value::string(needle)));
        CHECK(!sstrContains(SStr::create(u"i"), needle));
        CHECK(!sstrContains(std::u16string_view(u"i"), needle->view()));
    }

    // Cached ASCII property: computed lazily, composed for cons nodes, and
    // still correct after the rope is flattened.
    {
        SStr* ascii = SStr::create(u"hello");
        CHECK(!(ascii->flags & SStr::kAsciiKnown));
        CHECK(ascii->isAscii());
        CHECK(ascii->flags & SStr::kAsciiKnown);
        SStr* wide = SStr::create(u"h\u00e9llo");
        CHECK(!wide->isAscii());
        CHECK(wide->isAscii() == false);          // cached answer is stable
        SStr* c = sstrCons(ascii, SStr::create(u"world"));
        CHECK(c->isAscii());
        CHECK(sstrCons(ascii, wide)->isAscii() == false);
        CHECK(cast::compare(Value::string(SStr::create(u"ABC")),
                            Value::string(SStr::create(u"abc"))) == 0);
        CHECK(cast::compare(Value::string(ascii), Value::string(wide)) != 0);
        CHECK(rt::strContains(Value::string(c), Value::string(SStr::create(u"lowo"))));
        CHECK(!rt::strContains(Value::string(c), Value::string(SStr::create(u"\u00e9"))));
        CHECK(c->view() == u"helloworld");
        CHECK(c->isAscii());
        Gc::get().collect();
        CHECK(c->isAscii());
        CHECK(wide->isAscii() == false);
    }

    std::printf("%d checks, %d failures (sstr cons)\n", checks, failures);
    return failures ? 1 : 0;
}
