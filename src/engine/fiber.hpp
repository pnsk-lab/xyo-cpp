// xyo-cpp: スタックフルファイバ — Scratch スレッドの yield 実装。
//
// design.md §3: JIT コンパイル済みスクリプトもインタプリタも
// 「任意の呼び出し深さから yield できる」ことが必須要件のため、
// コルーチンではなく独自のコンテキストスイッチを使う。
//
// FiberRegs のレイアウトは fiber_<arch>.S と厳密に一致させること。
#pragma once

#include <cstddef>
#include <cstdint>

namespace xyo { class Fiber; }

/// ファイバの C++ 側エントリ（asm trampoline から tail-call される。戻らない）
extern "C" [[noreturn]] void xyo_fiber_main(xyo::Fiber*);

namespace xyo {

#if defined(__x86_64__) && defined(_WIN32)
// Win64 AMD64: callee-saved は rbx, rbp, rdi, rsi, r12-r15 + xmm6-15
// + rsp + rip（fiber_x86_64_win64.S と厳密一致）。
struct FiberRegs {
    uint64_t rbx, rbp, rdi, rsi, r12, r13, r14, r15, rsp, rip;
    uint64_t xmm[20];   // xmm6-xmm15（16B×10、movdqu で保存）
};
#elif defined(__x86_64__)
// SysV AMD64: callee-saved は rbx, rbp, r12-r15 + rsp + rip。
struct FiberRegs {
    uint64_t rbx, rbp, r12, r13, r14, r15, rsp, rip;
};
#elif defined(__aarch64__)
struct FiberRegs {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29, x30;
    uint64_t sp;
    uint64_t d8, d9, d10, d11, d12, d13, d14, d15;
};
#elif defined(__wasm32__) || defined(__EMSCRIPTEN__)
// WASM（Emscripten）: ネイティブの context switch は不可能。Phase A は
// interp 専用で fiber は実行されないため、コンパイル・リンクを通すための
// 最小スタブ。実際に resume/yield が呼ばれると abort する（fiber.cpp 参照）。
// 本物の Asyncify fiber（emscripten_fiber_*）は JIT の WASM 対応
// （Phase C の WASM モジュール化 JIT）で導入する。
struct FiberRegs {
    uint64_t rsp, rip, r15;
};
#else
#   error "fiber: unsupported arch (implement fiber_<arch>.S + FiberRegs)"
#endif

class Fiber;
extern "C" void xyo_fiber_switch(FiberRegs* from, FiberRegs* to);
extern "C" void xyo_fiber_trampoline();

using FiberEntry = void (*)(void* arg);

class Fiber {
public:
    /// スレッドに紐付くファイバ。初回 resume で entry(arg) から実行開始。
    /// スタックは mmap で reserve+commit-on-demand、guard page 付き。
    Fiber(FiberEntry entry, void* arg, size_t stackReserve = 1 << 20);
    ~Fiber();

    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;

    /// 呼び出し側（スケジューラ）コンテキストへ切り替えて入る。
    /// 戻ってきた時点でファイバは yield/kill 済み。
    void resume();

    /// ファイバ内からスケジューラへ yield。
    void yield();

    bool finished() const { return finished_; }

    /// GC root 走査用: 中断中の使用済みスタック区間 [sp, stackBase)。
    void* suspendedSp() const;
    void* stackBase() const { return stackBase_; }
    FiberRegs* regs() { return &regs_; }

    /// プール再利用: 新しい entry で初期状態に戻す。スタック領域はそのまま。
    void reset(FiberEntry entry, void* arg);

private:
    FiberRegs regs_{};
    void* stackBase_ = nullptr;   // スタック底（最終端、下方向成長）
    size_t stackReserve_ = 0;
    FiberEntry entry_ = nullptr;
    void* entryArg_ = nullptr;
    bool started_ = false;
    bool finished_ = false;
    // ASan fiber switch プロトコル用（非 ASan ビルドでも保持・未使用）
    void* asanCallerFake_ = nullptr;      // 呼出し側の fake stack save
    const void* asanSelfBottom_ = nullptr;
    size_t asanSelfSize_ = 0;

    friend void ::xyo_fiber_main(Fiber*);
    void run();
};

/// 現在実行中のファイバ（yield/scan の自己解決用）。TLS。
Fiber* currentFiber();

/// ファイバプール — broadcast/clone 等でスレッドを大量生成するワークロードの
/// mmap/munmap コストを削減する。ランタイムは単一スレッド前提。
struct FiberPool {
    /// プールから取得（なければ new）。stackBase_ が無効なものは捨てて再作成。
    static Fiber* acquire(FiberEntry entry, void* arg);
    /// 返却（容量超過分は delete）。nullptr 可。
    static void release(Fiber* f);
};

namespace detail {
/// スケジューラ側コンテキスト（resume の復帰先）。
extern thread_local FiberRegs gSchedulerRegs;
extern thread_local Fiber* gCurrentFiber;
} // namespace detail

} // namespace xyo
