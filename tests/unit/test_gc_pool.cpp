// Local collectors isolate allocator accounting from the process-wide runtime.
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

#include "core/gc.hpp"
#include "core/slist.hpp"
#include "core/sstr.hpp"

#if defined(__has_feature)
# if __has_feature(address_sanitizer)
#  define XYO_POOL_TEST_ASAN 1
# endif
#endif
#if defined(__SANITIZE_ADDRESS__)
# define XYO_POOL_TEST_ASAN 1
#endif
#ifdef XYO_POOL_TEST_ASAN
# include <sanitizer/asan_interface.h>
#endif

namespace {
using namespace xyo;
constexpr size_t slabBytes = 64 * 1024;
int failures = 0, checks = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; std::printf("GC pool FAIL: %s\n", label); }
}
GcHeader* header(void* payload) {
    return reinterpret_cast<GcHeader*>(static_cast<char*>(payload) - sizeof(GcHeader));
}
SStr* flat(Gc& gc, size_t len, char16_t value = u'x') {
    auto* s = static_cast<SStr*>(gc.alloc(GcType::Str,
        uint32_t(sizeof(SStr) + (len + 1) * sizeof(char16_t))));
    s->len = uint32_t(len);
    s->hash = 0;
    s->initCaches();
    for (size_t i = 0; i < len; ++i) s->inlineBody()[i] = value;
    s->inlineBody()[len] = 0;
    return s;
}
SStr* cons(Gc& gc, SStr* a, SStr* b) {
#ifdef XYO_STRING_CONS_POOL
    auto* s = static_cast<SStr*>(gc.allocCons(sizeof(SStr)));
#else
    auto* s = static_cast<SStr*>(gc.alloc(GcType::Str, sizeof(SStr)));
#endif
    s->len = a->len + b->len;
    s->hash = 0;
    s->initCaches();
    s->flags = SStr::kConsFlag;
    s->left = a;
    s->right = b;
    return s;
}

void boundariesAndCandidates() {
    Gc gc;
    gc.setDeferred(true);
    const size_t fixed = sizeof(GcHeader) + sizeof(SStr) + sizeof(char16_t);
    for (size_t limit : {size_t(80), size_t(96), size_t(128)}) {
        if (fixed > limit) continue;
        const size_t last = (limit - fixed) / sizeof(char16_t);
        for (size_t len : {last, last + 1}) {
            const auto before = gc.smallPoolObjectCount();
            SStr* s = flat(gc, len);
            const size_t total = fixed + len * sizeof(char16_t);
            check(gc.smallPoolObjectCount() == before +
                (gc.smallPoolEnabled() && total <= 128 ? 1 : 0), "class boundary routes by total bytes");
            auto* h = header(s);
            check(gc.isObject(h), "header membership");
            check(!gc.isObject(s), "payload is not header membership");
            check(!gc.isObject(reinterpret_cast<char*>(h) + 1), "interior is not header membership");
            check(h->size == sizeof(SStr) + (len + 1) * sizeof(char16_t), "logical payload size unchanged");
            check(s->inlineBody()[len] == 0, "boundary terminator accessible");
            if (gc.smallPoolEnabled() && total <= 128) {
                const size_t stride = total <= 80 ? 80 : (total <= 96 ? 96 : 128);
                SStr* next = flat(gc, len);
                check(reinterpret_cast<uintptr_t>(header(next)) -
                      reinterpret_cast<uintptr_t>(h) == stride, "exact pool size class stride");
#ifdef XYO_POOL_TEST_ASAN
                if (total < stride)
                    check(__asan_address_is_poisoned(reinterpret_cast<char*>(h) + total) != 0,
                          "live cell padding remains ASan poisoned");
#endif
            }
        }
    }
    SStr* s = flat(gc, 1);
    const uintptr_t h = reinterpret_cast<uintptr_t>(header(s));
    const uintptr_t base = h & ~(uintptr_t(slabBytes) - 1);
    if (gc.smallPoolEnabled()) {
        check(!gc.isObject(reinterpret_cast<void*>(base)), "slab metadata is not object");
        check(!gc.isObject(reinterpret_cast<void*>(base + slabBytes - 1)), "slab tail is not object");
    }
    check(!gc.isObject(nullptr), "null rejected");
    check(!gc.isObject(reinterpret_cast<void*>(uintptr_t(1))), "small integer rejected");
    // Invalid words deliberately include addresses in valid heap pages. A page
    // filter must never authorize reading a fabricated or poisoned header.
    std::array<uint64_t, 8> words = {
        1, uint64_t(h + 1), uint64_t(h + sizeof(GcHeader) + 1),
        uint64_t(base), uint64_t(base + slabBytes - 1),
        uint64_t(val::kTagStr | ((h + sizeof(GcHeader) + 1) & val::kPtrMask)),
        uint64_t(val::kTagStr | (uintptr_t(1) & val::kPtrMask)),
        std::numeric_limits<uint64_t>::max()
    };
    gc.scanRange(words.data(), words.data() + words.size());
    gc.markPtr(reinterpret_cast<void*>(uintptr_t(1)));
    gc.markObject(reinterpret_cast<GcHeader*>(h + 1));
    gc.drainPending();
    check(gc.isObject(reinterpret_cast<void*>(h)), "forged candidates do not damage live allocation");
}

void rootedGraphAndReentrantAllocation() {
    Gc gc;
    gc.setDeferred(true);
    SStr* a = flat(gc, 2, u'a');
    SStr* b = flat(gc, 3, u'b');
    SStr* c = cons(gc, a, b);
#ifdef XYO_STRING_CONS_POOL
    check(gc.smallPoolReservedBytes(Gc::PoolKind::Cons) > 0,
          "cons allocation enters dedicated slabs");
    check(gc.smallPoolReservedBytes(Gc::PoolKind::General) > 0,
          "flat allocation remains in general slabs");
#endif
    SStr* large = flat(gc, 200, u'z');
    auto* list = static_cast<SList*>(gc.alloc(GcType::List, sizeof(SList)));
    list->len = list->cap = 2;
    list->monitorDirty = false;
    list->items = static_cast<Value*>(std::malloc(2 * sizeof(Value)));
    list->items[0] = Value::string(c);
    list->items[1] = Value::string(large);
    gc.addRootProvider(&list, [&list](Gc& g) { g.markPtr(list); });
    for (int pass = 0; pass < 3; ++pass) {
        gc.collect();
        check(gc.isObject(header(list)) && gc.isObject(header(c)), "rooted list and cons survive");
        check(gc.isObject(header(c->left)) && gc.isObject(header(c->right)), "cons children survive");
        check(c->left->view() == u"aa" && c->right->view() == u"bbb", "cons children contents survive");
        check(large->len == 200 && large->inlineBody()[199] == u'z', "large malloc string survives");
        check(gc.objectCount() >= 5, "mixed graph accounting");
    }
    SStr* allocatedInCallback = nullptr;
    bool create = true;
    gc.addRootProvider(&allocatedInCallback, [&](Gc& g) {
        if (create) { allocatedInCallback = flat(g, 4, u'r'); create = false; }
        else g.markPtr(allocatedInCallback);
    });
    const auto count = gc.collectCount();
    gc.setDeferred(false);
    gc.setThreshold(1);
    gc.collect();
    check(gc.collectCount() == count + 1, "root callback allocation does not recursively collect");
    check(gc.isObject(header(allocatedInCallback)), "black callback allocation survives same sweep");
    gc.collect();
    check(allocatedInCallback->view() == u"rrrr", "callback allocation survives subsequent rooted sweep");
    gc.removeRootProvider(&allocatedInCallback);
    gc.removeRootProvider(&list);
}

// Fragmented slabs must reuse only unallocated cells. Exercise all size
// classes across repeated collection/reallocation, including surviving ropes.
void fragmentedRunsPreserveSurvivors() {
    Gc gc;
    gc.setDeferred(true);
    struct Root { SStr* str; char16_t value; };
    std::vector<Root> roots;
    roots.reserve(5000);
    gc.addRootProvider(&roots, [&](Gc& g) {
        for (const Root& r : roots) g.markPtr(r.str);
    });
    constexpr uint32_t cycleCount = 3;
    for (uint32_t cycle = 0; cycle < cycleCount; ++cycle) {
        for (uint32_t i = 0; i < 2400; ++i) {
            const uint32_t len = i % 3 == 0 ? 1 : (i % 3 == 1 ? 8 : 20);
            const char16_t value = char16_t(u'A' + (cycle * 7 + i) % 26);
            SStr* current = flat(gc, len, value);
            if (i % 5 == 0) roots.push_back({current, value});
        }
        gc.collect();
        for (const Root& r : roots) {
            check(gc.isObject(header(r.str)),
                  "fragmented slab retained exact rooted allocation");
            check(r.str->inlineBody()[0] == r.value,
                  "fragmented allocation did not overwrite survivor");
        }
    }
    gc.removeRootProvider(&roots);
}

void workerAllocationAndReclamation() {
    Gc gc;
    gc.setDeferred(true);
    gc.setThreshold(1);
    constexpr size_t workers = 4, perWorker = 2500;
    std::vector<SStr*> roots(workers * perWorker);
    std::vector<std::thread> threads;
    std::atomic<bool> start{false};
    const auto collections = gc.collectCount();
    for (size_t worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&, worker] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (size_t i = 0; i < perWorker; ++i) {
                if ((i & 1) == 0) {
                    roots[worker * perWorker + i] =
                        flat(gc, i % 3 == 0 ? 1 : (i % 3 == 1 ? 8 : 20));
                } else {
                    SStr* left = flat(gc, 2, u'a');
                    SStr* right = flat(gc, 3, u'b');
                    roots[worker * perWorker + i] = cons(gc, left, right);
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : threads) worker.join();
    check(gc.collectCount() == collections, "worker allocations remain deferred");
    const size_t expectedObjects = roots.size() + workers * (perWorker / 2) * 2;
    check(gc.objectCount() == expectedObjects,
          "serialized mixed worker allocation accounting");
    for (SStr* root : roots) {
        if (root->isCons())
            check(root->view() == u"aabbb", "mixed pool cons retains flat children");
    }
    gc.addRootProvider(&roots, [&roots](Gc& g) {
        for (auto* s : roots) g.markPtr(s);
    });
    gc.collect();
    check(gc.objectCount() == expectedObjects,
          "heap vector provider retains all mixed worker graphs");
#ifdef XYO_STRING_CONS_POOL
    check(gc.smallPoolObjectCount(Gc::PoolKind::Cons) == roots.size() / 2,
          "cons pool live count isolated");
    check(gc.smallPoolObjectCount(Gc::PoolKind::General) == roots.size() + roots.size() / 2,
          "flat pool live count isolated");
#endif
    const auto reserved = gc.smallPoolReservedBytes();
    const auto pooled = gc.smallPoolObjectCount();
    if (gc.smallPoolEnabled()) check(reserved > 3 * slabBytes, "stress spans multiple slabs");
    // Removing heap-held references does not leave every pointer on the main
    // stack, unlike allocating garbage directly in this function.
    std::vector<uintptr_t> oldHeaders;
    oldHeaders.reserve(roots.size());
    for (auto* s : roots) oldHeaders.push_back(reinterpret_cast<uintptr_t>(header(s)));
    roots.clear();
    gc.collect();
    size_t noLongerObjects = 0;
    for (uintptr_t address : oldHeaders) {
        if (!gc.isObject(reinterpret_cast<void*>(address))) {
            ++noLongerObjects;
#ifdef XYO_POOL_TEST_ASAN
            if (gc.smallPoolEnabled())
                check(__asan_address_is_poisoned(reinterpret_cast<void*>(address)) != 0,
                      "reclaimed header is ASan poisoned");
#endif
        }
    }
    check(noLongerObjects > oldHeaders.size() / 2, "freed cells fail exact membership");
    check(gc.objectCount() < perWorker, "unrooted worker objects reclaimed despite conservative residue");
    if (gc.smallPoolEnabled()) {
        check(gc.smallPoolObjectCount() < pooled, "allocated bitmap count drops after sweep");
        check(gc.smallPoolReservedBytes() < reserved, "entirely empty excess slabs released");
        check(gc.smallPoolReservedBytes() <= 6 * slabBytes, "empty cache bounded with conservative survivors");
    }
    const auto afterSweep = gc.smallPoolReservedBytes();
    for (size_t len : {size_t(1), size_t(8), size_t(20)}) roots.push_back(flat(gc, len));
    gc.collect();
    for (auto* s : roots) check(gc.isObject(header(s)) && s->inlineBody()[0] == u'x', "reallocated cells survive and initialize");
    if (gc.smallPoolEnabled()) check(gc.smallPoolReservedBytes() <= afterSweep + 3 * slabBytes, "reallocation uses bounded slab storage");
    gc.removeRootProvider(&roots);
}
void registryHighWaterReclamation() {
    PtrSet set;
    for (uintptr_t i = 1; i <= 100000; ++i) set.insert(i * 16);
    const auto peak = set.capacity();
    check(set.size() == 100000, "registry contains burst objects");
    set.resetForRebuild(100);
    check(set.size() == 0, "registry rebuild removes old membership");
    check(set.capacity() == 1024 && peak >= 131072,
          "registry releases historical peak capacity");
    for (uintptr_t i = 1; i <= 100; ++i) set.insert(i * 32);
    for (uintptr_t i = 1; i <= 100; ++i)
        check(set.count(i * 32), "rebuilt registry membership survives shrink");
    check(!set.count(16), "discarded membership does not survive rebuild");
    set.reserve(1000);
    const auto moderate = set.capacity();
    set.resetForRebuild(100);
    check(set.capacity() == moderate && set.size() == 0,
          "registry retains capacity across small fluctuations");
    set.resetForRebuild(0);
    check(!set.count(32), "empty rebuilt registry rejects old keys");
}
} // namespace

int test_gc_pool_main() {
    failures = checks = 0;
    registryHighWaterReclamation();
    boundariesAndCandidates();
    rootedGraphAndReentrantAllocation();
    workerAllocationAndReclamation();
    fragmentedRunsPreserveSurvivors();
    std::printf("GC pool: %d checks, %d failures\n", checks, failures);
    return failures;
}
