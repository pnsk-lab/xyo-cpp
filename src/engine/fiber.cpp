// xyo-cpp: ファイバ実装（POSIX mmap / Win32 VirtualAlloc + fiber_<arch>.S 切替）。
#include "fiber.hpp"

#ifdef _WIN32
#include <windows.h>
#elif defined(__EMSCRIPTEN__) || defined(__wasm32__) || defined(XYO_WII)
// WASM/Wii: ネイティブ stack switch は不可。Phase A は interp 専用で fiber は
// 実行されないため、malloc ベースのダミー領域 + abort スタブでコンパイルを通す。
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <atomic>
#include <array>
#include <vector>

#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define XYO_ASAN 1
#    include <sanitizer/asan_interface.h>
#  endif
#endif

namespace xyo {

namespace detail {
thread_local FiberRegs gSchedulerRegs{};
thread_local Fiber* gCurrentFiber = nullptr;
#ifndef XYO_ASAN
struct FiberChain {
    Fiber* const* fibers;
    size_t count;
    size_t next;
};
thread_local FiberChain* gFiberChain = nullptr;
Fiber* chainNext() {
    FiberChain* chain = gFiberChain;
    return chain && chain->next < chain->count
               ? chain->fibers[chain->next++] : nullptr;
}
#endif
} // namespace detail

Fiber* currentFiber() { return detail::gCurrentFiber; }

namespace {
std::atomic<size_t> gUnsafeNativeFibers{0};

bool protectNativeSsaAtGc() {
    static const bool enabled = [] {
        const char* roots = std::getenv("XYO_RT_REQUIRE_GENERATED_SSA_ROOTS");
        return roots && roots[0] == '1';
    }();
    return enabled;
}
}

Fiber::Fiber(FiberEntry entry, void* arg, size_t stackReserve)
    : entry_(entry), entryArg_(arg), stackReserve_(stackReserve) {
    // guard ページ + スタック領域を一括確保。guard は low end（下方向成長の先）。
#ifdef XYO_WII
    // No native fibers in interpreter-only Wii builds: reserve only a
    // placeholder instead of spending 1 MiB per accidental allocation.
    stackReserve_ = 0;
#endif
    const size_t page = 4096;
    const size_t total = stackReserve_ + page;
#ifdef _WIN32
    void* base = VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT,
                              PAGE_READWRITE);
    if (!base) {
        stackBase_ = nullptr;
        return;
    }
    DWORD old;
    VirtualProtect(base, page, PAGE_NOACCESS, &old);
#elif defined(__EMSCRIPTEN__) || defined(__wasm32__) || defined(XYO_WII)
    // WASM/Wii: 領域は未使用（resume は abort）だが、stackBase_ が非 null である
    // 約束（FiberPool::acquire の null チェック）を保つため malloc で埋める。
    void* base = std::malloc(total);
    if (!base) {
        stackBase_ = nullptr;
        return;
    }
#else
    void* base = mmap(nullptr, total, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        // mmap 失敗時は十分なサイズに落ちる（OOM は別途ハンドルする層で）
        stackBase_ = nullptr;
        return;
    }
    // low end の 1 ページを guard 化
    mprotect(base, page, PROT_NONE);
#endif
    stackBase_ = static_cast<uint8_t*>(base) + total;  // スタック底（最終端）

    // 初期コンテキスト: rsp = 16B アラインしたスタック先端 - 8
    // （トランポリンが "call から来た" 形で関数規約を満たすように rsp%16==8）
#if defined(XYO_WII)
    // A Wii build never enters the fiber trampoline.
#elif defined(__aarch64__)
    // AAPCS64 and Darwin require 16-byte SP alignment at function entry.
    regs_.sp = reinterpret_cast<uintptr_t>(stackBase_) & ~uintptr_t(15);
    regs_.x30 = reinterpret_cast<uintptr_t>(&xyo_fiber_trampoline);
    regs_.x19 = reinterpret_cast<uintptr_t>(this);
#else
    uintptr_t sp = reinterpret_cast<uintptr_t>(stackBase_) - 8;
    regs_.rsp = sp;
    regs_.rip = reinterpret_cast<uint64_t>(&xyo_fiber_trampoline);
    regs_.r15 = reinterpret_cast<uint64_t>(this);       // trampoline → Fiber*
#endif
}

Fiber::~Fiber() {
    if (stackBase_) {
#ifdef _WIN32
        VirtualFree(static_cast<uint8_t*>(stackBase_) - stackReserve_ - 4096,
                    0, MEM_RELEASE);
#elif defined(__EMSCRIPTEN__) || defined(__wasm32__) || defined(XYO_WII)
        std::free(static_cast<uint8_t*>(stackBase_) - stackReserve_ - 4096);
#else
        munmap(static_cast<uint8_t*>(stackBase_) - stackReserve_ - 4096,
               stackReserve_ + 4096);
#endif
    }
}

void Fiber::resume() {
    Fiber* prev = detail::gCurrentFiber;
    detail::gCurrentFiber = this;
#ifdef XYO_ASAN
    // ASan fiber プロトコル: 切替先の bottom（low end）+ size を通知し、
    // 現在の fake stack を asanCallerFake_ に退避させる。fiber 側の
    // finish_switch がこの値を受け取って呼出し側へ復元する。
    const void* bottom =
        static_cast<const uint8_t*>(stackBase_) - stackReserve_;
    __sanitizer_start_switch_fiber(&asanCallerFake_, bottom, stackReserve_);
#endif
    xyo_fiber_switch(&detail::gSchedulerRegs, &regs_);
    // ここに戻ってきた時点でファイバは yield または終了している
    // （fake stack の復元は fiber 側の finish_switch が済ませている）。
    detail::gCurrentFiber = prev;
}

void Fiber::resumeChain(Fiber* const* fibers, size_t count) {
    if (!count) return;
#ifdef XYO_ASAN
    for (size_t i = 0; i < count; ++i) fibers[i]->resume();
#else
    if (detail::gFiberChain) {
        for (size_t i = 0; i < count; ++i) fibers[i]->resume();
        return;
    }
    detail::FiberChain chain{fibers, count, 1};
    detail::gFiberChain = &chain;
    fibers[0]->resume();
    detail::gFiberChain = nullptr;
#endif
}

void Fiber::yield() {
#ifndef XYO_ASAN
    if (Fiber* next = detail::chainNext()) {
        detail::gCurrentFiber = next;
        xyo_fiber_switch(&regs_, &next->regs_);
        return;
    }
#endif
#ifdef XYO_ASAN
    __sanitizer_finish_switch_fiber(asanCallerFake_,
                                    &asanSelfBottom_, &asanSelfSize_);
#endif
    xyo_fiber_switch(&regs_, &detail::gSchedulerRegs);
}

// Same translation unit as Fiber::yield permits inlining its chain dispatch.
extern "C" void xyo_yield_fast(Fiber* f) {
    f->yield();
}

void* Fiber::suspendedSp() const {
#if defined(XYO_WII)
    return nullptr;
#elif defined(__aarch64__)
    return reinterpret_cast<void*>(regs_.sp);
#else
    return reinterpret_cast<void*>(regs_.rsp);
#endif
}

void Fiber::reset(FiberEntry entry, void* arg) {
    entry_ = entry;
    entryArg_ = arg;
    finished_ = false;
    started_ = false;
    asanCallerFake_ = nullptr;
    asanSelfBottom_ = nullptr;
    asanSelfSize_ = 0;
    regs_ = FiberRegs{};
#if defined(XYO_WII)
    // Interpreter-only; no native context to reset.
#elif defined(__aarch64__)
    regs_.sp = reinterpret_cast<uintptr_t>(stackBase_) & ~uintptr_t(15);
    regs_.x30 = reinterpret_cast<uintptr_t>(&xyo_fiber_trampoline);
    regs_.x19 = reinterpret_cast<uintptr_t>(this);
#else
    uintptr_t sp = reinterpret_cast<uintptr_t>(stackBase_) - 8;
    regs_.rsp = sp;
    regs_.rip = reinterpret_cast<uint64_t>(&xyo_fiber_trampoline);
    regs_.r15 = reinterpret_cast<uint64_t>(this);
#endif
}

void Fiber::run() {
    // The trampoline may allocate a Fiber ahead of its first resume.
    // Only an activation that has actually entered native code can
    // contain unrooted LLVM SSA. Count it from entry until the native
    // function returns, including all intervening yields.
    const bool protectedActivation = protectNativeSsaAtGc();
    if (protectedActivation)
        gUnsafeNativeFibers.fetch_add(1, std::memory_order_acq_rel);
    started_ = true;
    entry_(entryArg_);
    if (protectedActivation)
        gUnsafeNativeFibers.fetch_sub(1, std::memory_order_acq_rel);
}

namespace {
std::vector<Fiber*> gFiberPool;
std::mutex gFiberPoolMtx;   // 並列 worker からの acquire/release を直列化
std::atomic<size_t> gLeasedNativeFibers{0};
std::atomic<size_t> gFiberlessAcquires{0};
constexpr size_t kFiberPoolCap = 64;

// Fibers leased and returned on one OS thread need no cross-thread lock.
// Keep this strictly opt-in until the cost and OS-thread recycling semantics
// are measured. Thread exit unmaps idle stacks rather than leaking mappings.
bool localFiberPoolEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("XYO_FIBER_LOCAL_POOL");
        return v && v[0] == '1';
    }();
    return enabled;
}
struct LocalFiberPool {
    static constexpr size_t kCapacity = 16;
    std::array<Fiber*, kCapacity> fibers{};
    size_t count = 0;
    ~LocalFiberPool() {
        while (count != 0) delete fibers[--count];
    }
};
thread_local LocalFiberPool gLocalFibers;
}

Fiber* FiberPool::acquire(FiberEntry entry, void* arg) {
    // Detect unsupported native paths rather than silently reporting a
    // fiber-free implementation while still acquiring native stacks.
    static const bool diagnostic = [] {
        const char* v = std::getenv("XYO_JIT_FIBERLESS");
        return v && v[0] == '1';
    }();
    if (diagnostic)
        gFiberlessAcquires.fetch_add(1, std::memory_order_relaxed);
    if (localFiberPoolEnabled() && gLocalFibers.count != 0) {
        Fiber* f = gLocalFibers.fibers[--gLocalFibers.count];
        f->reset(entry, arg);
        gLeasedNativeFibers.fetch_add(1, std::memory_order_release);
        return f;
    }
    std::lock_guard<std::mutex> lk(gFiberPoolMtx);
    while (!gFiberPool.empty()) {
        Fiber* f = gFiberPool.back();
        gFiberPool.pop_back();
        if (f->stackBase()) {
            f->reset(entry, arg);
            gLeasedNativeFibers.fetch_add(1, std::memory_order_release);
            return f;
        }
        delete f;   // mmap 失敗品はプールしない
    }
    Fiber* f = new Fiber(entry, arg);
    gLeasedNativeFibers.fetch_add(1, std::memory_order_release);
    return f;
}

size_t FiberPool::activeCount() {
    return gLeasedNativeFibers.load(std::memory_order_acquire);
}

size_t FiberPool::fiberlessAcquires() {
    return gFiberlessAcquires.load(std::memory_order_acquire);
}

size_t FiberPool::unsafeCount() {
    return gUnsafeNativeFibers.load(std::memory_order_acquire);
}

void FiberPool::release(Fiber* f) {
    if (!f) return;
    // A terminated or never-started Fiber has no live native activation.
    // A *suspended* Fiber is now unreachable, including in ASan's
    // intentional stack-leak path, so remove its last GC veto.
    if (protectNativeSsaAtGc() && f->started() && !f->finished())
        gUnsafeNativeFibers.fetch_sub(1, std::memory_order_acq_rel);
    gLeasedNativeFibers.fetch_sub(1, std::memory_order_acq_rel);
#ifdef XYO_ASAN
    // ASan 下では中断状態の fiber を再利用すると、そのスタック範囲に
    // 紐付く fakestack が「実行中」扱いのまま残り、再利用後のフレームが
    // stack-use-after-scope と誤判定される。munmap でアドレス範囲が
    // 再利用されても同じ問題が起き得るため、未完了 fiber は意図的に
    // リークさせる（ASan ビルドのみ。完了 fiber は通常通り再利用可）。
    if (!f->finished()) return;
#endif
    if (localFiberPoolEnabled() && f->stackBase() &&
        gLocalFibers.count < LocalFiberPool::kCapacity) {
        gLocalFibers.fibers[gLocalFibers.count++] = f;
        return;
    }
    std::lock_guard<std::mutex> lk(gFiberPoolMtx);
    if (gFiberPool.size() < kFiberPoolCap) gFiberPool.push_back(f);
    else delete f;
}

} // namespace xyo

#if defined(__EMSCRIPTEN__) || defined(__wasm32__) || defined(XYO_WII)
// WASM/Wii: アセンブリによるコンテキストスイッチは存在しない。Phase A は
// interp 専用で fiber は実行されないが、リンクのためシンボルだけ用意する。
// 呼ばれたら即 abort（サイレントにハングさせない）。
extern "C" void xyo_fiber_switch(xyo::FiberRegs*, xyo::FiberRegs*) {
    std::fprintf(stderr,
                 "xyo: fiber switch is unavailable in WASM (interp-only "
                 "build; JIT wasm needs Phase C)\n");
    std::abort();
}
extern "C" void xyo_fiber_trampoline() {
    std::fprintf(stderr, "xyo: fiber trampoline is unavailable in WASM\n");
    std::abort();
}
#endif

/// ファイバの実体のエントリ。戻らない —— 終了時はスケジューラへ最終スイッチ。
extern "C" [[noreturn]] void xyo_fiber_main(xyo::Fiber* f) {
    f->run();
    f->finished_ = true;
#ifndef XYO_ASAN
    if (xyo::Fiber* next = xyo::detail::chainNext()) {
        xyo::detail::gCurrentFiber = next;
        xyo_fiber_switch(&f->regs_, next->regs());
        __builtin_unreachable();
    }
#endif
    xyo::detail::gCurrentFiber = nullptr;
#ifdef XYO_ASAN
    __sanitizer_finish_switch_fiber(f->asanCallerFake_,
                                    &f->asanSelfBottom_, &f->asanSelfSize_);
#endif
    xyo_fiber_switch(&f->regs_, &xyo::detail::gSchedulerRegs);
    __builtin_unreachable();
}
