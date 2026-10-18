// xyo-cpp: x86-64 SysV ファイバコンテキストスイッチ。
// FiberRegs レイアウト (fiber.hpp と一致):
//   +0  rbx
//   +8  rbp
//   +16 r12
//   +24 r13
//   +32 r14
//   +40 r15
//   +48 rsp
//   +56 rip

.text
.globl xyo_fiber_switch
.type  xyo_fiber_switch, @function
// void xyo_fiber_switch(FiberRegs* from /*rdi*/, FiberRegs* to /*rsi*/)
xyo_fiber_switch:
    movq %rbx,  0(%rdi)
    movq %rbp,  8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax         // call が積んだ retaddr 分を除いた復帰時 rsp
    movq %rax, 48(%rdi)
    movq (%rsp), %rax          // 戻りアドレスを rip として保存
    movq %rax, 56(%rdi)

    movq  0(%rsi), %rbx
    movq  8(%rsi), %rbp
    movq 16(%rsi), %r12
    movq 24(%rsi), %r13
    movq 32(%rsi), %r14
    movq 40(%rsi), %r15
    movq 48(%rsi), %rsp
    movq 56(%rsi), %rax
    jmpq *%rax                 // 保存された rip へ（新ファイバでは trampoline）
.size xyo_fiber_switch, .-xyo_fiber_switch

.globl xyo_fiber_trampoline
.type  xyo_fiber_trampoline, @function
// 初回 resume 時の入口。r15 に Fiber* が初期化済み。
xyo_fiber_trampoline:
    movq %r15, %rdi
    movq %rsp, %rbp            // 不定なら aligned な base FP にしておく
    jmp xyo_fiber_main         // tail call; 戻らない
.size xyo_fiber_trampoline, .-xyo_fiber_trampoline

.section .note.GNU-stack,"",@progbits
