// x86-64 Darwin (Mach-O) SysV fiber switch. Offsets match FiberRegs.
.text
.p2align 4
.globl _xyo_fiber_switch
_xyo_fiber_switch:
    movq %rbx,  0(%rdi)
    movq %rbp,  8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax
    movq %rax, 48(%rdi)
    movq (%rsp), %rax
    movq %rax, 56(%rdi)

    movq  0(%rsi), %rbx
    movq  8(%rsi), %rbp
    movq 16(%rsi), %r12
    movq 24(%rsi), %r13
    movq 32(%rsi), %r14
    movq 40(%rsi), %r15
    movq 48(%rsi), %rsp
    movq 56(%rsi), %rax
    jmpq *%rax

.p2align 4
.globl _xyo_fiber_trampoline
_xyo_fiber_trampoline:
    movq %r15, %rdi
    movq %rsp, %rbp
    jmp _xyo_fiber_main
