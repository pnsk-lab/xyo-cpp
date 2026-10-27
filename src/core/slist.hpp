// xyo-cpp: SList — Scratch リスト実体（GC 管理）。
// items は別途 malloc した可変バッファ（オブジェクト自体は非移動、
// GC の sweep 時に items も解放される）。
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "gc.hpp"
#include "value.hpp"

namespace xyo {

struct SList {
    uint32_t len;
    uint32_t cap;
    bool monitorDirty;   // _monitorUpToDate=!dirty 相当（モニタ更新用）
    Value* items;

    static SList* create(uint32_t initialCap = 4) {
        auto* l = static_cast<SList*>(
            Gc::get().alloc(GcType::List, sizeof(SList)));
        l->len = 0;
        l->cap = initialCap;
        l->monitorDirty = false;
        l->items = static_cast<Value*>(std::malloc(initialCap * sizeof(Value)));
        return l;
    }

    void ensure(uint32_t need) {
        if (need <= cap) return;
        cap = cap * 2 < need ? need : cap * 2;
        items = static_cast<Value*>(std::realloc(items, cap * sizeof(Value)));
    }
    void push(Value v) {
        ensure(len + 1);
        items[len++] = v;
    }
    /// 0-based インデックスで挿入
    void insert0(uint32_t i, Value v) {
        if (i > len) return;              // defensive: callers pass 1-based-1
        ensure(len + 1);
        std::memmove(items + i + 1, items + i, (len - i) * sizeof(Value));
        items[i] = v;
        ++len;
    }
    void erase0(uint32_t i) {
        if (i >= len) return;             // defensive: never memmove past len
        std::memmove(items + i, items + i + 1, (len - i - 1) * sizeof(Value));
        --len;
    }
    void clear() { len = 0; }
    SList* clone() const {
        SList* c = create(cap);
        c->len = len;
        std::memcpy(c->items, items, len * sizeof(Value));
        c->monitorDirty = monitorDirty;
        return c;
    }
};

} // namespace xyo
