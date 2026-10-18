// xyo-cpp: x86-64 Win64 ファイバコンテキストスイッチ。
// FiberRegs レイアウト (fiber.hpp と一致):
//   +0   rbx
//   +8   rbp
//   +16  rdi
//   +24  rsi
//   +32  r12
//   +40  r13
//   +48  r14
//   +56  r15
//   +64  rsp
//   +72  rip
//   +80  xmm6-xmm15 (movdqu, 16B×10)
// Win64 ABI: args rcx=from, rdx=to。callee-saved に rdi/rsi/xmm6-15 が
// SysV より多い点に注意。

.text
.globl xyo_fiber_switch
// void xyo_fiber_switch(FiberRegs* from /*rcx*/, FiberRegs* to /*rdx*/)
xyo_fiber_switch:
    movq %rbx,  0(%rcx)
    movq %rbp,  8(%rcx)
    movq %rdi, 16(%rcx)
    movq %rsi, 24(%rcx)
    movq %r12, 32(%rcx)
    movq %r13, 40(%rcx)
    movq %r14, 48(%rcx)
    movq %r15, 56(%rcx)
    leaq 8(%rsp), %rax         // call が積んだ retaddr 分を除いた復帰時 rsp
    movq %rax, 64(%rcx)
    movq (%rsp), %rax          // 戻りアドレスを rip として保存
    movq %rax, 72(%rcx)
    movdqu %xmm6,   80(%rcx)
    movdqu %xmm7,   96(%rcx)
    movdqu %xmm8,  112(%rcx)
    movdqu %xmm9,  128(%rcx)
    movdqu %xmm10, 144(%rcx)
    movdqu %xmm11, 160(%rcx)
    movdqu %xmm12, 176(%rcx)
    movdqu %xmm13, 192(%rcx)
    movdqu %xmm14, 208(%rcx)
    movdqu %xmm15, 224(%rcx)

    movq  0(%rdx), %rbx
    movq  8(%rdx), %rbp
    movq 16(%rdx), %rdi
    movq 24(%rdx), %rsi
    movq 32(%rdx), %r12
    movq 40(%rdx), %r13
    movq 48(%rdx), %r14
    movq 56(%rdx), %r15
    movdqu  80(%rdx), %xmm6
    movdqu  96(%rdx), %xmm7
    movdqu 112(%rdx), %xmm8
    movdqu 128(%rdx), %xmm9
    movdqu 144(%rdx), %xmm10
    movdqu 160(%rdx), %xmm11
    movdqu 176(%rdx), %xmm12
    movdqu 192(%rdx), %xmm13
    movdqu 208(%rdx), %xmm14
    movdqu 224(%rdx), %xmm15
    movq 64(%rdx), %rsp
    movq 72(%rdx), %rax
    jmpq *%rax                 // 保存された rip へ（新ファイバでは trampoline）

.globl xyo_fiber_trampoline
// 初回 resume 時の入口。r15 に Fiber* が初期化済み。
// Win64 の第1引数は rcx。rsp%16==8（call 直後相当）で到着する。
xyo_fiber_trampoline:
    movq %r15, %rcx
    movq %rsp, %rbp
    jmp xyo_fiber_main         // tail call; 戻らない
