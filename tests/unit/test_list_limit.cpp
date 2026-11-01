// リスト長上限（--list-limit / XYO_LIST_LIMIT）。
// scratch の 200000 上限と同じ「insert は末尾を落として長さを保つ」挙動を
// SList と rt::listInsert の両方で検証する。
#include <cstdio>
#include <vector>

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
        check(l->items[0].asNumberUnchecked() == 1 &&
                  l->items[2].asNumberUnchecked() == 3,
              "push keeps the first items");

        // 先頭への insert は末尾を落として長さを保つ
        l->insert0(0, Value::numCanonical(9));
        check(l->len == 3, "insert at 1 keeps length");
        check(l->items[0].asNumberUnchecked() == 9 &&
                  l->items[1].asNumberUnchecked() == 1 &&
                  l->items[2].asNumberUnchecked() == 2,
              "insert at 1 shifts and drops the tail");

        // 末尾への insert も落とされる（長さは変わらない）
        l->insert0(l->len, Value::numCanonical(7));
        check(l->len == 3, "insert at last keeps length");
        check(l->items[2].asNumberUnchecked() == 2, "append is dropped");
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
        check(l->items[0].asNumberUnchecked() == 0 &&
                  l->items[1].asNumberUnchecked() == 1 &&
                  l->items[2].asNumberUnchecked() == 2,
              "listInsert shifts at the front");
    }

    // 上限を外せばまた伸びる
    xyo_list_limit = 0;
    {
        SList* l = SList::create();
        for (int i = 0; i < 10; ++i) l->push(Value::numCanonical(i));
        check(l->len == 10, "limit can be cleared");
    }

    std::printf("test_list_limit: %d checks, %d failures\n", checks,
                failures);
    return failures == 0 ? 0 : 1;
}
