#include "gc.hpp"
#include "../jit/rtprof.hpp"
#include "../engine/fiber.hpp"
#include "slist.hpp"
#include "sstr.hpp"
#include <bit>
#include <cassert>
#include <chrono>
#include <new>
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

// Preserve ASan's per-object checks even though several objects now share one
// allocation: metadata stays accessible, free cells and live padding do not.
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define XYO_GC_ASAN 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#  define XYO_GC_ASAN 1
#endif
#ifdef XYO_GC_ASAN
#  include <sanitizer/asan_interface.h>
#  define GC_POISON(p, n) __asan_poison_memory_region((p), (n))
#  define GC_UNPOISON(p, n) __asan_unpoison_memory_region((p), (n))
#else
#  define GC_POISON(p, n) ((void)0)
#  define GC_UNPOISON(p, n) ((void)0)
#endif

namespace xyo {

namespace {
/// Per-thread slab ownership. A slab is only used by the thread that took it
/// from the available list, and every collection bumps poolGen_ so cached
/// pointers are discarded instead of being dereferenced after a slab release.
struct PoolTls {
    Gc* owner;
    uint64_t gen;
    void* slab[2][3];
    size_t pendingBytes;
};
thread_local PoolTls g_poolTls{nullptr, 0, {}, 0};
std::atomic<uint64_t> g_poolGenCounter{0};
} // namespace

Gc::Gc() {
    const char* enabled = std::getenv("XYO_GC_POOL");
    poolEnabled_ = !enabled || enabled[0] != '0';
    if (const char* mb = std::getenv("XYO_GC_THRESHOLD")) {
        const long v = std::atol(mb);
        if (v > 0) minThreshold_ = size_t(v) << 20;
    }
    // A collector can be constructed at an address a previous one used (local
    // collectors in tests). The unique generation keeps stale thread caches
    // from matching a new instance.
    poolGen_.store(g_poolGenCounter.fetch_add(1, std::memory_order_relaxed) + 1,
                   std::memory_order_relaxed);
}

size_t Gc::smallPoolSlabCount(PoolKind pool) const {
    size_t count = 0;
    for (const SmallSlab* slab : poolSlabs_)
        if (slab->pool == pool) ++count;
    return count;
}

size_t Gc::smallPoolReservedBytes(PoolKind pool) const {
    return smallPoolSlabCount(pool) * kSlabBytes;
}

size_t Gc::smallPoolObjectCount(PoolKind pool) const {
    size_t count = 0;
    for (const SmallSlab* slab : poolSlabs_)
        if (slab->pool == pool) count += slab->live;
    return count;
}

Gc::~Gc() {
    assert(slabBases_.size() == poolSlabs_.size());
    for (GcHeader* h = allObjects_; h;) {
        GcHeader* next = h->next;
        destroyPayload(h);
        std::free(h);
        h = next;
    }
    // Pooled strings are only reachable through the slab bitmaps.
    for (SmallSlab* slab : poolSlabs_) {
        const size_t stride = kCellBytes[slab->classIndex];
        char* base = reinterpret_cast<char*>(slab) + kSlabDataOffset;
        for (unsigned word = 0; word < 16; ++word) {
            uint64_t bits = slab->allocated[word];
            while (bits) {
                const unsigned bit = std::countr_zero(bits);
                bits &= bits - 1;
                const size_t index = size_t(word) * 64 + bit;
                if (index >= slab->capacity) break;
                destroyPayload(reinterpret_cast<GcHeader*>(base + index * stride));
            }
        }
        GC_UNPOISON(slab, kSlabBytes);
        ::operator delete(slab, std::align_val_t{kSlabBytes});
    }
}

Gc::SmallSlab* Gc::acquireSlabLocked(unsigned cls, PoolKind pool) {
    const unsigned kind = static_cast<unsigned>(pool);
    SmallSlab* slab = availableSlabs_[kind][cls];
    if (slab) {
        assert(slab->pool == pool && slab->classIndex == cls);
        availableSlabs_[kind][cls] = slab->availableNext;
        slab->availableNext = nullptr;
        slab->firstFreeWord = 0;
        return slab;
    }
    slab = static_cast<SmallSlab*>(::operator new(kSlabBytes, std::align_val_t{kSlabBytes}));
    slab->availableNext = nullptr;
    slab->classIndex = cls;
    slab->pool = pool;
    slab->capacity = uint32_t((kSlabBytes - kSlabDataOffset) / kCellBytes[cls]);
    slab->live = 0;
    slab->firstFreeWord = 0;
    for (unsigned word = 0; word < 16; ++word) {
        const unsigned start = word * 64;
        const unsigned free = start < slab->capacity
            ? std::min(64u, slab->capacity - start) : 0;
        // Bits outside the complete-cell area are permanently unavailable.
        slab->allocated[word] = free == 64 ? 0 : (~uint64_t(0) << free);
    }
    poolSlabs_.push_back(slab);
    slabBases_.insert(reinterpret_cast<uintptr_t>(slab));
    GC_POISON(reinterpret_cast<char*>(slab) + kSlabDataOffset,
              kSlabBytes - kSlabDataOffset);
    return slab;
}

GcHeader* Gc::allocSmallTls(size_t total, PoolKind pool) {
    const unsigned cls = total <= kCellBytes[0] ? 0 : (total <= kCellBytes[1] ? 1 : 2);
    const unsigned kind = static_cast<unsigned>(pool);
    PoolTls& tls = g_poolTls;
    const uint64_t gen = poolGen_.load(std::memory_order_acquire);
    if (tls.owner != this || tls.gen != gen) {
        tls.owner = this;
        tls.gen = gen;
        std::memset(tls.slab, 0, sizeof(tls.slab));
    }
    for (;;) {
        auto* slab = static_cast<SmallSlab*>(tls.slab[kind][cls]);
        if (!slab) {
            std::lock_guard<std::recursive_mutex> lk(allocMtx_);
            slab = acquireSlabLocked(cls, pool);
            tls.slab[kind][cls] = slab;
        }
        unsigned word = slab->firstFreeWord;
        while (word * 64 < slab->capacity && slab->allocated[word] == ~uint64_t(0))
            ++word;
        if (word * 64 >= slab->capacity) {
            tls.slab[kind][cls] = nullptr;   // full; it stays registered for sweep
            continue;
        }
        const unsigned bit = std::countr_zero(~slab->allocated[word]);
        slab->allocated[word] |= uint64_t(1) << bit;
        slab->firstFreeWord = word;
        ++slab->live;
        auto* h = reinterpret_cast<GcHeader*>(reinterpret_cast<char*>(slab) +
            kSlabDataOffset + size_t(word * 64 + bit) * kCellBytes[cls]);
        GC_UNPOISON(h, total);
        return h;
    }
}

void Gc::sweepSmallSlabs(size_t& live) {
    // Called with workers joined. The bitmap is the authority, so pooled cells
    // never enter allObjects_: sweeping a slab costs one pass over its bits.
    for (SmallSlab* slab : poolSlabs_) {
        const size_t stride = kCellBytes[slab->classIndex];
        char* base = reinterpret_cast<char*>(slab) + kSlabDataOffset;
        for (unsigned word = 0; word < 16; ++word) {
            uint64_t bits = slab->allocated[word];
            while (bits) {
                const unsigned bit = std::countr_zero(bits);
                bits &= bits - 1;
                const size_t index = size_t(word) * 64 + bit;
                if (index >= slab->capacity) break;
                auto* h = reinterpret_cast<GcHeader*>(base + index * stride);
                if (h->mark) {
                    h->mark = 0;
                    live += sizeof(GcHeader) + h->size;
                } else {
                    destroyPayload(h);
                    GC_POISON(h, stride);
                    slab->allocated[word] &= ~(uint64_t(1) << bit);
                    --slab->live;
                }
            }
        }
        slab->firstFreeWord = 0;
    }
}

void Gc::rebuildSmallSlabs() {
    // Called only after sweep, with workers joined. No freelist may retain a
    // pointer into a retired slab. Cache at most one empty slab per pool/class.
    bool emptyKept[2][3] = {};
    for (auto& pool : availableSlabs_)
        for (auto& available : pool) available = nullptr;
    slabBases_.clear();
    size_t kept = 0;
    for (SmallSlab* slab : poolSlabs_) {
        const unsigned cls = slab->classIndex;
        const unsigned kind = static_cast<unsigned>(slab->pool);
        assert(kind < 2 && cls < 3);
        assert(reinterpret_cast<uintptr_t>(slab) % kSlabBytes == 0);
        if (!slab->live && emptyKept[kind][cls]) {
            GC_UNPOISON(slab, kSlabBytes);
            ::operator delete(slab, std::align_val_t{kSlabBytes});
            continue;
        }
        if (!slab->live) emptyKept[kind][cls] = true;
        if (slab->live < slab->capacity) {
            slab->firstFreeWord = 0;
            slab->availableNext = availableSlabs_[kind][cls];
            availableSlabs_[kind][cls] = slab;
        }
        poolSlabs_[kept++] = slab;
        slabBases_.insert(reinterpret_cast<uintptr_t>(slab));
    }
    poolSlabs_.resize(kept);
    // Invalidate every thread's cached slab: the next allocation re-acquires.
    const uint64_t nextGen =
        g_poolGenCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    poolGen_.store(nextGen, std::memory_order_release);
}

bool Gc::validHeader(uintptr_t address) const {
    SmallSlab* slab = slabBase(address);
    if (slabBases_.count(reinterpret_cast<uintptr_t>(slab))) {
        const size_t offset = address - reinterpret_cast<uintptr_t>(slab);
        if (offset < kSlabDataOffset) return false;
        const size_t stride = kCellBytes[slab->classIndex];
        const size_t relative = offset - kSlabDataOffset;
        if (relative % stride != 0) return false;
        const size_t index = relative / stride;
        return index < slab->capacity &&
               (slab->allocated[index / 64] & (uint64_t(1) << (index % 64)));
    }
    // The page set is a rejection filter, never permission to dereference an
    // arbitrary header candidate. Exact malloc membership must come first.
    return (pageSet_.count(address >> 12) ||
            pageSet_.count((address + sizeof(GcHeader)) >> 12)) && objSet_.count(address);
}

bool Gc::isObject(void* p) const {
    return validHeader(reinterpret_cast<uintptr_t>(p));
}

void Gc::markHeader(uintptr_t address) {
    if (!validHeader(address)) return;
    auto* h = reinterpret_cast<GcHeader*>(address);
    if (!h->mark) {
        h->mark = 1;
        pending_.push_back(h);
    }
}

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
    const size_t total = sizeof(GcHeader) + size_t(bytes);
    const bool pooled = poolEnabled_ && type == GcType::Str && total <= kCellBytes[2];
    if (pooled) {
        // Hot path: the slab is owned by this thread, so no lock and no global
        // object list are touched. Byte accounting is accumulated in TLS and
        // published in batches; a per-allocation atomic add would cost more
        // than the lock it replaced.
        GcHeader* h = allocSmallTls(total, PoolKind::General);
        h->next = nullptr;
        h->size = bytes;
        h->type = type;
        h->mark = collecting_.load(std::memory_order_relaxed) ? 1 : 0;
        PoolTls& tls = g_poolTls;
        tls.pendingBytes += total;
        if (tls.pendingBytes >= (4u << 10)) {
            const size_t pending = tls.pendingBytes;
            tls.pendingBytes = 0;
            const size_t now =
                bytesAllocated_.fetch_add(pending, std::memory_order_relaxed) + pending;
            if (!deferredCollect_.load(std::memory_order_relaxed) && now > threshold_ &&
                !collecting_.load(std::memory_order_relaxed)) {
                std::lock_guard<std::recursive_mutex> lk(allocMtx_);
                if (!deferredCollect_.load(std::memory_order_relaxed) &&
                    bytesAllocated_.load(std::memory_order_relaxed) > threshold_ &&
                    !collecting_.load(std::memory_order_relaxed))
                    collect();
            }
        }
        return static_cast<void*>(h + 1);
    }
    std::lock_guard<std::recursive_mutex> lk(allocMtx_);
    if (!deferredCollect_.load() &&
        bytesAllocated_.load(std::memory_order_relaxed) + bytes > threshold_ &&
        !collecting_.load(std::memory_order_relaxed)) {
        collect();
    }
    auto* h = static_cast<GcHeader*>(std::malloc(total));
    if (!h) std::abort(); // The runtime is built with -fno-exceptions.
    h->next = allObjects_;
    h->size = bytes;
    h->type = type;
    // collect のマークフェーズ中に割り当てられたオブジェクトは、
    // ルート走査（Mutation::parseArgs 等）が作った生存オブジェクト
    // —— mark=0 のままだと同じ sweep で即座に解放されてしまうため
    // 黒色割当（割当時に生扱い）にする。
    h->mark = collecting_.load(std::memory_order_relaxed) ? 1 : 0;
    allObjects_ = h;
    objSet_.insert(reinterpret_cast<uintptr_t>(h));
    uintptr_t p1 = reinterpret_cast<uintptr_t>(h) >> 12;
    uintptr_t p2 = reinterpret_cast<uintptr_t>(h + 1) >> 12;
    if (p1 != lastPage_) { pageSet_.insert(p1); lastPage_ = p1; }
    if (p2 != lastPage_) pageSet_.insert(p2);
    bytesAllocated_.fetch_add(total, std::memory_order_relaxed);
    return static_cast<void*>(h + 1);
}

#if defined(XYO_STRING_CONS_POOL)
void* Gc::allocCons(uint32_t bytes) {
    const size_t total = sizeof(GcHeader) + size_t(bytes);
    if (!poolEnabled_ || total > kCellBytes[2])
        return alloc(GcType::Str, bytes);

    // This is deliberately a separate entry point rather than a third defaulted
    // alloc argument: the ordinary allocation ABI and call sites stay unchanged.
    GcHeader* h = allocSmallTls(total, PoolKind::Cons);
    h->next = nullptr;
    h->size = bytes;
    h->type = GcType::Str;
    h->mark = collecting_.load(std::memory_order_relaxed) ? 1 : 0;
    PoolTls& tls = g_poolTls;
    tls.pendingBytes += total;
    if (tls.pendingBytes >= (4u << 10)) {
        const size_t pending = tls.pendingBytes;
        tls.pendingBytes = 0;
        const size_t now =
            bytesAllocated_.fetch_add(pending, std::memory_order_relaxed) + pending;
        if (!deferredCollect_.load(std::memory_order_relaxed) && now > threshold_ &&
            !collecting_.load(std::memory_order_relaxed)) {
            std::lock_guard<std::recursive_mutex> lk(allocMtx_);
            if (!deferredCollect_.load(std::memory_order_relaxed) &&
                bytesAllocated_.load(std::memory_order_relaxed) > threshold_ &&
                !collecting_.load(std::memory_order_relaxed))
                collect();
        }
    }
    return static_cast<void*>(h + 1);
}
#endif

void Gc::markValue(Value v) {
    if ((v.bits >> 48) == val::kTagStr >> 48) {
        markPtr(v.asStrPtr());
    }
}

void Gc::markPtr(void* p) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(p);
    if (address >= sizeof(GcHeader)) markHeader(address - sizeof(GcHeader));
}

/// マーク済みオブジェクトの子を積む（**再帰しない**）。cons string は
/// 1 本の鎖が数万ノードになる（`s = s ++ "ab"`）ので、ここで再帰すると
/// マーク中にスタックを溢れさせる。積むだけにして collect が反復で
/// 消化する（リストの入れ子も同じ経路）。
void Gc::markObject(GcHeader* h) {
    markHeader(reinterpret_cast<uintptr_t>(h));
}

void Gc::drainPending() {
    while (!pending_.empty()) {
        GcHeader* h = pending_.back();
        pending_.pop_back();
        if (h->type == GcType::List) {
            auto* list = reinterpret_cast<SList*>(h + 1);
            for (uint32_t i = 0; i < list->len; ++i) {
                markValue(list->items[i]);
            }
        } else if (h->type == GcType::Str) {
            auto* s = reinterpret_cast<SStr*>(h + 1);
            if (s->isCons()) {
                markPtr(s->left);
                markPtr(s->right);
            }
        }
    }
}

XYO_NO_ASAN
void Gc::scanRange(const void* begin, const void* end) {
    const uint64_t* lo = static_cast<const uint64_t*>(begin);
    const uint64_t* hi = static_cast<const uint64_t*>(end);
    for (const uint64_t* w = lo; w < hi; ++w) {
        uint64_t v = *w;
        const bool tagged = (v >> 48) == (val::kTagStr >> 48);
        const uintptr_t candidate = uintptr_t(tagged ? v & val::kPtrMask : v);
        if (!slabBases_.count(reinterpret_cast<uintptr_t>(slabBase(candidate))) &&
            !pageSet_.count(candidate >> 12)) continue;
        if (tagged) {
            const uintptr_t p = uintptr_t(v & val::kPtrMask);
            if (p >= sizeof(GcHeader)) markHeader(p - sizeof(GcHeader));
        } else {
            const uintptr_t p = uintptr_t(v);
            if (p >= sizeof(GcHeader)) markHeader(p - sizeof(GcHeader));
            markHeader(p);
        }
    }
}

void Gc::collectIfNeeded() {
    std::lock_guard<std::recursive_mutex> lk(allocMtx_);
    if (!collecting_.load(std::memory_order_relaxed) &&
        bytesAllocated_.load(std::memory_order_relaxed) > threshold_) collect();
}

void Gc::removeRootProvider(void* owner) {
    for (auto it = rootProviders_.begin(); it != rootProviders_.end(); ++it) {
        if (it->owner == owner) { rootProviders_.erase(it); return; }
    }
}

void Gc::collect() {
    std::lock_guard<std::recursive_mutex> lk(allocMtx_);
    if (collecting_.load(std::memory_order_relaxed)) return;
    // This is still a stop-the-world operation. Workers must be joined before
    // collecting; the mutex only serializes allocator bookkeeping.
    collecting_.store(true, std::memory_order_relaxed);
    // 診断（既定 off）: collect 回数・live・累計時間を stderr へ。
    // off のときは時計も読まない。
    static const bool gcDebug = std::getenv("XYO_GC_DEBUG") != nullptr;
    const auto tStart = gcDebug ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
    XYO_RT_PROF(kGc);
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
                // jmp_buf を含む全ローカルが範囲に入る）。
                // **ファイバ上では rsp が pthread スタックの外**にあるため、
                // そのままクランプすると未使用の 8MiB 全域を毎回走査して
                // しまう（JIT の文字列ループで collect 1 回 2.9ms の主因）。
                // ファイバ自身のスタックは Runtime::markRoots が走査するので、
                // ここではスケジューラ（呼出し元）の退避 rsp から先だけ見る。
                uintptr_t lo = xyo::currentFiber()
                    ? reinterpret_cast<uintptr_t>(xyo::detail::gSchedulerRegs.rsp)
                    : reinterpret_cast<uintptr_t>(__builtin_alloca(0));
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
    // 根から辿った子を反復で消化（マーク中の再帰を無くす）
    drainPending();

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
            destroyPayload(h);
            std::free(h);
        }
        h = next;
    }
    *prev = nullptr;
    // Pooled strings are swept through their slab bitmaps (they are not in
    // allObjects_), then empty slabs are released and the generation bumps.
    sweepSmallSlabs(live);
    rebuildSmallSlabs();
    // sweep で pageSet を生存オブジェクトから再構築（解放済みの
    // ページは malloc 再利用で偽陽性が増えるため残さない）
    pageSet_.clear();
    lastPage_ = ~uintptr_t(0);
    // objSet_ も生存分で作り直す（フラット集合は削除を持たない）。
    objSet_.clear();
    std::vector<uintptr_t> pages;
    pages.reserve(objSet_.size() ? 0 : 1024);
    for (GcHeader* h = allObjects_; h; h = h->next) {
        objSet_.insert(reinterpret_cast<uintptr_t>(h));
        pages.push_back(reinterpret_cast<uintptr_t>(h) >> 12);
        uintptr_t p2 = reinterpret_cast<uintptr_t>(h + 1) >> 12;
        if (p2 != pages.back()) pages.push_back(p2);
    }
    for (uintptr_t pg : pages)
        if (pg != lastPage_) { pageSet_.insert(pg); lastPage_ = pg; }
    bytesAllocated_.store(live + listBytes, std::memory_order_relaxed);
    if (gcDebug) {
        static uint64_t ncoll = 0;
        static double tsum = 0;
        ++ncoll;
        tsum += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - tStart).count();
        const size_t generalSlabs = smallPoolSlabCount(PoolKind::General);
        const size_t consSlabs = smallPoolSlabCount(PoolKind::Cons);
        std::fprintf(stderr,
                     "[gc] collect #%llu live=%.1fMB objs=%zu "
                     "total=%.1fms pool=%.1fMB "
                     "general=%zu/%zuB cons=%zu/%zuB\n",
                     (unsigned long long)ncoll, live / 1048576.0,
                     objectCount(), tsum, smallPoolReservedBytes() / 1048576.0,
                     smallPoolObjectCount(PoolKind::General), generalSlabs * kSlabBytes,
                     smallPoolObjectCount(PoolKind::Cons), consSlabs * kSlabBytes);
    }
    // ヒープ倍増方針: (GC オブジェクト + リストバッファ) の 2 倍 or
    // 2MiB の大きい方。リストが育つほど GC 間隔を広げ、走査コストに
    // 見合う量のゴミを溜めてから回収する。
    // 下限は 8MiB: 文字列連結のような「小さな live + 大量のゴミ」では
    // 下限がそのまま collect 間隔になり、2MiB だと 100ms 予算の半分を
    // 走査に使ってしまう（str_join の実測で 400 回/round）。
    size_t next = (live + listBytes) * 2;
    threshold_ = next > minThreshold_ ? next : minThreshold_;
    collecting_.store(false, std::memory_order_relaxed);
}

void Gc::destroyPayload(GcHeader* h) {
    if (h->type == GcType::List) {
        auto* list = reinterpret_cast<SList*>(h + 1);
        std::free(list->items);
    } else if (h->type == GcType::Str) {
        // cons の flatten 結果は GC オブジェクトではないので明示的に返す。
        auto* s = reinterpret_cast<SStr*>(h + 1);
        if (s->isCons() && s->body) std::free(s->body);
    }
}

} // namespace xyo
