/*
 * xyo-cpp: x86-64 System V decimal formatter for signed 64-bit integers.
 *
 * size_t xyo_i64_to_ascii_asm(int64_t value, char out[32])
 *   rdi = value, rsi = out, rax = bytes written (no trailing NUL).
 *
 * Formats all int64_t values, including INT64_MIN. No heap allocation,
 * libc calls, SSE state changes, or callee-saved register modifications.
 * Uses 2-digit chunks; the reverse scratch area is out[0..31] itself.
 * Input and output must not alias (input is passed by value).
 *
 * Only linked on Linux x86-64 when XYO_ASM_NUM_TO_STR=ON.
 */
    .intel_syntax noprefix
    .text
    .p2align 4
    .globl xyo_i64_to_ascii_asm
    .type xyo_i64_to_ascii_asm, @function
xyo_i64_to_ascii_asm:
    mov rax, rdi
    xor r9d, r9d
    test rax, rax
    jns .Lpositive
    neg rax
    mov r9d, 1
.Lpositive:
    # The common 1- and 2-digit cases write directly to the destination,
    # avoiding a reverse buffer and rep movsb.
    cmp rax, 100
    jae .Llarge
    cmp rax, 10
    jae .Ltwo
    add al, '0'
    test r9d, r9d
    jnz .Lnegative_one
    mov BYTE PTR [rsi], al
    mov eax, 1
    ret
.Lnegative_one:
    mov BYTE PTR [rsi], '-'
    mov BYTE PTR [rsi + 1], al
    mov eax, 2
    ret
.Ltwo:
    lea r11, [rip + .Lpairs]
    lea rcx, [rax + rax]
    mov dx, WORD PTR [r11 + rcx]
    test r9d, r9d
    jnz .Lnegative_two
    mov WORD PTR [rsi], dx
    mov eax, 2
    ret
.Lnegative_two:
    mov BYTE PTR [rsi], '-'
    mov WORD PTR [rsi + 1], dx
    mov eax, 3
    ret

.Llarge:
    lea r10, [rsi + 32]
    lea r11, [rip + .Lpairs]
    # Only the <=2^53 range goes through jsnum::toString's exact-int path.
    # For this range, ceil(2^68/100) gives an exact quotient for /100.
    # Larger uint64 magnitudes retain the universal DIV implementation,
    # including INT64_MIN, for the standalone formatter's complete ABI.
    movabs r8, 9007199254740992
    cmp rax, r8
    ja .Lfallback_init
    movabs r8, 0x28f5c28f5c28f5c3
.Lfast_loop:
    cmp rax, 100
    jb .Llast
    mov rcx, rax
    mul r8
    shr rdx, 4
    mov rax, rdx
    imul rdx, rdx, 100
    sub rcx, rdx
    lea rcx, [rcx + rcx]
    sub r10, 2
    mov dx, WORD PTR [r11 + rcx]
    mov WORD PTR [r10], dx
    jmp .Lfast_loop
.Lfallback_init:
    mov r8d, 100
.Lfallback_loop:
    cmp rax, 100
    jb .Llast
    xor edx, edx
    div r8
    lea rcx, [rdx + rdx]
    sub r10, 2
    mov dx, WORD PTR [r11 + rcx]
    mov WORD PTR [r10], dx
    jmp .Lfallback_loop
.Llast:
    cmp rax, 10
    jb .Llast_one
    lea rcx, [rax + rax]
    sub r10, 2
    mov dx, WORD PTR [r11 + rcx]
    mov WORD PTR [r10], dx
    jmp .Lsign
.Llast_one:
    sub r10, 1
    add al, '0'
    mov BYTE PTR [r10], al
.Lsign:
    test r9d, r9d
    jz .Lcopy
    sub r10, 1
    mov BYTE PTR [r10], '-'
.Lcopy:
    lea rcx, [rsi + 32]
    sub rcx, r10
    mov rax, rcx
    mov rdi, rsi
    mov rsi, r10
    cld
    rep movsb
    ret
    .size xyo_i64_to_ascii_asm, .-xyo_i64_to_ascii_asm

    .section .rodata
    .p2align 4
.Lpairs:
    .ascii "00010203040506070809"
    .ascii "10111213141516171819"
    .ascii "20212223242526272829"
    .ascii "30313233343536373839"
    .ascii "40414243444546474849"
    .ascii "50515253545556575859"
    .ascii "60616263646566676869"
    .ascii "70717273747576777879"
    .ascii "80818283848586878889"
    .ascii "90919293949596979899"
    .section .note.GNU-stack,"",@progbits
