#include "gc.hpp"
#include "../jit/rtprof.hpp"
#include "slist.hpp"
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include "../engine/fiber.hpp"
#elif defined(__EMSCRIPTEN__)
#include <emscripten/stack.h>
#else
#include <pthread.h>
#endif

// 保守的スタックスキャンは poisoned redzone を読むため ASan 計装対象外。
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define XYO_NO_ASAN __attribute__((no_sanitize("address")))
#  endif
#endif
#if !defined(XYO_NO_ASAN) && defined(__SANITIZE_ADDRESS__)
#  define XYO_NO_ASAN __attribute__((no_sanitize("address")))
#endif
#if !defined(XYO_NO_ASAN)
#  define XYO_NO_ASAN
#endif

namespace xyo {

#ifdef _WIN32
namespace {
// メインスレッドのコミット済みスタック範囲 — Gc::get() 初回呼出し
// （実質メインスレッドでの初期化）で記録する。ファイバ上の collect は
// TIB がファイバの範囲を返すため、メインスタックを別途走査する必要がある。
ULONG_PTR g_mainStackLo = 0, g_mainStackHi = 0;
DWORD g_mainTid = 0;
} // namespace
#endif

Gc& Gc::get() {
    static Gc gc;
#ifdef _WIN32
    if (g_mainTid == 0) {
        g_mainTid = GetCurrentThreadId();
        GetCurrentThreadStackLimits(&g_mainStackLo, &g_mainStackHi);
    }
#endif
    return gc;
}

void* Gc::alloc(GcType type, uint32_t bytes) {
    std::lock_guard<std::recursive_mutex> lk(allocMtx_);
    if (!deferredCollect_.load() &&
        bytesAllocated_ + bytes > threshold_ && !collecting_) {
        collect();
    }
    size_t total = sizeof(GcHeader) + bytes;
    auto* h = static_cast<GcHeader*>(std::malloc(total));
    h->next = allObjects_;
    h->size = bytes;
    h->type = type;
    // collect のマークフェーズ中に割り当てられたオブジェクトは、
    // ルート走査（Mutation::parseArgs 等）が作った生存オブジェクト
    // —— mark=0 のままだと同じ sweep で即座に解放されてしまうため
    // 黒色割当（割当時に生扱い）にする。
    h->mark = collecting_ ? 1 : 0;
    allObjects_ = h;
    objSet_.insert(h);
    pageSet_.insert(reinterpret_cast<uintptr_t>(h) >> 12);
    pageSet_.insert(reinterpret_cast<uintptr_t>(h + 1) >> 12);
    bytesAllocated_ += total;
    return static_cast<void*>(h + 1);
}

void Gc::markValue(Value v) {
    if ((v.bits >> 48) == val::kTagStr >> 48) {
        markPtr(v.asStrPtr());
    }
}

void Gc::markPtr(void* p) {
    if (!p) return;
    // p はペイロード先頭（SStr*/SList*）。ヘッダは 1 つ手前。
    auto* h = reinterpret_cast<GcHeader*>(
        static_cast<char*>(p) - sizeof(GcHeader));
    if (objSet_.count(h)) markObject(h);
}

void Gc::markObject(GcHeader* h) {
    if (h->mark) return;
    h->mark = 1;
    if (h->type == GcType::List) {
        auto* list = reinterpret_cast<SList*>(h + 1);
        for (uint32_t i = 0; i < list->len; ++i) {
            markValue(list->items[i]);
        }
    }
}

XYO_NO_ASAN
void Gc::scanRange(const void* begin, const void* end) {
    const uint64_t* lo = static_cast<const uint64_t*>(begin);
    const uint64_t* hi = static_cast<const uint64_t*>(end);
    for (const uint64_t* w = lo; w < hi; ++w) {
        uint64_t v = *w;
        // タグ付き文字列ポインタ候補
        if ((v >> 48) == (val::kTagStr >> 48)) {
            void* p = reinterpret_cast<void*>(v & val::kPtrMask);
            if (pageSet_.count(reinterpret_cast<uintptr_t>(p) >> 12)) {
                // ペイロード先頭 → ヘッダは p - sizeof(GcHeader)
                auto* h = reinterpret_cast<GcHeader*>(
                    static_cast<char*>(p) - sizeof(GcHeader));
                if (objSet_.count(h)) markObject(h);
            }
        } else {
            // 生ポインタ/ヘッダ候補 — ページが GC ヒープに無ければ
            // objSet ルックアップ3回をまとめて省略する
            if (!pageSet_.count(v >> 12)) continue;
            auto* h = reinterpret_cast<GcHeader*>(
                reinterpret_cast<char*>(v) - sizeof(GcHeader));
            if (objSet_.count(h)) markObject(h);
            // 直接ヘッダを指す場合
            if (objSet_.count(reinterpret_cast<GcHeader*>(v))) {
                markObject(reinterpret_cast<GcHeader*>(v));
            }
        }
    }
}

void Gc::collectIfNeeded() {
    std::lock_guard<std::recursive_mutex> lk(allocMtx_);
    if (!collecting_ && bytesAllocated_ > threshold_) collect();
}

void Gc::removeRootProvider(void* owner) {
    for (auto it = rootProviders_.begin(); it != rootProviders_.end(); ++it) {
        if (it->owner == owner) { rootProviders_.erase(it); return; }
    }
}

void Gc::collect() {
    XYO_RT_PROF(kGc);
    collecting_ = true;
    ++collectCount_;

    // 保守的ルート: callee-saved レジスタを jmp_buf 経由でスタックに
    // 退避させ、ネイティブスタック全域をワード走査する。
    // ローカル変数の Value / SStr* もここで捕捉される。
    {
        jmp_buf jb;
        memset(jb, 0, sizeof(jb));
        if (setjmp(jb) == 0) {
            volatile char* jbBase = reinterpret_cast<volatile char*>(jb);
            (void)jbBase;
        }
#ifdef _WIN32
        // 現コンテキストのコミット範囲。ファイバ上では TIB の
        // StackBase/StackLimit がファイバのものに差し替わる。
        {
            ULONG_PTR lo, hi;
            GetCurrentThreadStackLimits(&lo, &hi);
            uintptr_t cur = reinterpret_cast<uintptr_t>(
                __builtin_alloca(0));
            if (cur < lo) cur = lo;
            if (cur < hi)
                scanRange(reinterpret_cast<void*>(cur),
                          reinterpret_cast<void*>(hi));
        }
        // ファイバ/別スレッド上の collect では現在範囲がメインスタックを
        // カバーしない — Gc::get() で記録したメイン範囲を併走する。
        // メインスレッド上かつファイバ外なら現在範囲と重複するので省く。
        if (GetCurrentThreadId() != g_mainTid ||
            xyo::currentFiber() != nullptr) {
            if (g_mainStackLo < g_mainStackHi)
                scanRange(reinterpret_cast<void*>(g_mainStackLo),
                          reinterpret_cast<void*>(g_mainStackHi));
        }
#elif defined(__linux__)
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            void* stackAddr = nullptr;
            size_t stackSize = 0;
            if (pthread_attr_getstack(&attr, &stackAddr, &stackSize) == 0 &&
                stackAddr != nullptr) {
                // 現 rsp からスタック底まで走査（alloca(0) は現 rsp 相当 —
                // jmp_buf を含む全ローカルが範囲に入る）
                uintptr_t lo = reinterpret_cast<uintptr_t>(
                    __builtin_alloca(0));
                uintptr_t base = reinterpret_cast<uintptr_t>(stackAddr);
                uintptr_t top = base + stackSize;
                if (lo < base) lo = base;
                if (lo < top) {
                    scanRange(reinterpret_cast<void*>(lo),
                              reinterpret_cast<void*>(top));
                }
            }
            pthread_attr_destroy(&attr);
        }
#elif defined(__EMSCRIPTEN__)
        // WASM: スタックは下方向成長。現 SP（alloca(0)）から base までを
        // ワード走査する（Linux 経路と同じ「jmp_buf を含む全ローカルが
        // 範囲に入る」方針）。emscripten_stack_get_base は
        // <emscripten/stack.h>（Emscripten のスタック管理 API）。
        {
            uintptr_t lo = reinterpret_cast<uintptr_t>(__builtin_alloca(0));
            uintptr_t hi = reinterpret_cast<uintptr_t>(
                emscripten_stack_get_base());
            if (lo < hi)
                scanRange(reinterpret_cast<void*>(lo),
                          reinterpret_cast<void*>(hi));
        }
#endif
    }

    for (auto& rp : rootProviders_) rp.fn(*this);

    // sweep
    GcHeader** prev = &allObjects_;
    size_t live = 0;
    size_t listBytes = 0;
    for (GcHeader* h = allObjects_; h; ) {
        GcHeader* next = h->next;
        if (h->mark) {
            h->mark = 0;
            *prev = h;
            prev = &h->next;
            live += sizeof(GcHeader) + h->size;
            // SList の items バッファは Gc::alloc を通らず malloc で
            // 管理している。これを live に数えないと、巨大リストを
            // 持つプロジェクトで「文字列を少し確保するたびに全リストを
            // 走査する GC」が頻発する（voxelRenderer では 1 回 17ms ×
            // 数百回が支配的になる）。cap を外部メモリとして算入し、
            // 次回閾値を live 比例で引き上げる。
            if (h->type == GcType::List) {
                auto* list = reinterpret_cast<SList*>(h + 1);
                listBytes += size_t(list->cap) * sizeof(Value);
            }
        } else {
            objSet_.erase(h);
            destroyPayload(h);
            std::free(h);
        }
        h = next;
    }
    *prev = nullptr;
    // sweep で pageSet を生存オブジェクトから再構築（解放済みの
    // ページは malloc 再利用で偽陽性が増えるため残さない）
    pageSet_.clear();
    for (GcHeader* h = allObjects_; h; h = h->next) {
        pageSet_.insert(reinterpret_cast<uintptr_t>(h) >> 12);
        pageSet_.insert(reinterpret_cast<uintptr_t>(h + 1) >> 12);
    }
    bytesAllocated_ = live + listBytes;
    // ヒープ倍増方針: (GC オブジェクト + リストバッファ) の 2 倍 or
    // 2MiB の大きい方。リストが育つほど GC 間隔を広げ、走査コストに
    // 見合う量のゴミを溜めてから回収する。
    size_t next = (live + listBytes) * 2;
    threshold_ = next > (2 << 20) ? next : (2 << 20);
    collecting_ = false;
}

void Gc::destroyPayload(GcHeader* h) {
    if (h->type == GcType::List) {
        auto* list = reinterpret_cast<SList*>(h + 1);
        std::free(list->items);
    }
}

} // namespace xyo
