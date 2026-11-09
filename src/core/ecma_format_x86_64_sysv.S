/*
 * Format already-converted shortest decimal digits as ECMAScript Number text.
 *
 * size_t xyo_ecma_format_asm(const char* digits, uint32_t digits_len,
 *                            int32_t decimal_point, char out[64]);
 *
 * SysV x86-64: rdi=digits, esi=digits_len (1..17),
 *               edx=decimal_point, rcx=output
 *
 * Preconditions: ASCII decimal digits, k in [1,17]; exponent from the
 * shortest scientific conversion of a finite, positive, nonzero double.
 * Output <= 64 bytes; result is byte length, no trailing NUL.
 *
 * The caller owns shortest-round-trip digit generation and exceptional
 * value handling. No floating-point arithmetic or allocations here.
 */
    .intel_syntax noprefix
    .text
    .p2align 4
    .globl xyo_ecma_format_asm
    .type xyo_ecma_format_asm, @function
xyo_ecma_format_asm:
    push rbx
    mov rbx, rcx                # output base, callee-saved
    mov r8, rcx                 # output cursor
    mov r9, rdi                 # digit data
    mov r10d, esi              # number of digits
    mov r11d, edx              # position of decimal point

    # Branch 1: digits + trailing zeroes, 1 <= k <= n <= 21.
    cmp r11d, r10d
    jl .Lfraction_or_scientific
    cmp r11d, 21
    jg .Lscientific
    mov rsi, r9
    mov rdi, r8
    mov ecx, r10d
    rep movsb
    mov r8, rdi
    mov ecx, r11d
    sub ecx, r10d
    mov al, '0'
    rep stosb
    mov r8, rdi
    jmp .Lfinish

.Lfraction_or_scientific:
    # Branch 2: 0 < n <= 21, some significant digits before '.'.
    test r11d, r11d
    jle .Lzero_prefix_or_scientific
    cmp r11d, 21
    jg .Lscientific
    mov rsi, r9
    mov rdi, r8
    mov ecx, r11d
    rep movsb
    mov BYTE PTR [rdi], '.'
    inc rdi
    mov ecx, r10d
    sub ecx, r11d
    rep movsb
    mov r8, rdi
    jmp .Lfinish

.Lzero_prefix_or_scientific:
    # Branch 3: -5 <= n <= 0, 0.000...<digits>.
    cmp r11d, -5
    jl .Lscientific
    mov WORD PTR [r8], 0x2e30      # "0."
    add r8, 2
    mov rdi, r8
    mov ecx, r11d
    neg ecx
    mov al, '0'
    rep stosb
    mov rsi, r9
    mov ecx, r10d
    rep movsb
    mov r8, rdi
    jmp .Lfinish

.Lscientific:
    # d[0] [ "." d[1:] ] "e" sign decimalExponent
    mov al, BYTE PTR [r9]
    mov BYTE PTR [r8], al
    inc r8
    cmp r10d, 1
    je .Lexp
    mov BYTE PTR [r8], '.'
    inc r8
    lea rsi, [r9 + 1]
    mov rdi, r8
    mov ecx, r10d
    dec ecx
    rep movsb
    mov r8, rdi
.Lexp:
    mov BYTE PTR [r8], 'e'
    inc r8
    mov eax, r11d
    dec eax
    test eax, eax
    js .Lnegative_exp
    mov BYTE PTR [r8], '+'
    inc r8
    jmp .Lexp_digits
.Lnegative_exp:
    neg eax
    mov BYTE PTR [r8], '-'
    inc r8
.Lexp_digits:
    # Actual Number exponents fit in [-324, 308], but handle 0..999.
    lea r9, [rip + .Ldecimal_pairs]
    cmp eax, 100
    jb .Lexp_lt100
    xor edx, edx
    mov ecx, 100
    div ecx
    add al, '0'
    mov BYTE PTR [r8], al
    inc r8
    mov eax, edx
    # Always emit two padded digits after hundreds.
    lea ecx, [rax + rax]
    mov dx, WORD PTR [r9 + rcx]
    mov WORD PTR [r8], dx
    add r8, 2
    jmp .Lfinish
.Lexp_lt100:
    cmp eax, 10
    jb .Lexp_single
    lea ecx, [rax + rax]
    mov dx, WORD PTR [r9 + rcx]
    mov WORD PTR [r8], dx
    add r8, 2
    jmp .Lfinish
.Lexp_single:
    add al, '0'
    mov BYTE PTR [r8], al
    inc r8
.Lfinish:
    sub r8, rbx
    mov rax, r8
    pop rbx
    ret
    .size xyo_ecma_format_asm, .-xyo_ecma_format_asm

/*
 * Given a positive finite nonzero shortest scientific representation from
 * std::to_chars(double, scientific), parse the digits and exponent AND
 * output ECMAScript text in ASM. This eliminates C++ scientific parsing.
 *
 * size_t xyo_ecma_from_scientific_asm(
 *     const char* scientific, uint32_t length, char out[64]);
 *
 * Only accepts canonical scientific strings, e.g. "1.23e+04".
 * Contract is intentionally narrow; caller has validated the source.
 */
    .p2align 4
    .globl xyo_ecma_from_scientific_asm
    .type xyo_ecma_from_scientific_asm, @function
xyo_ecma_from_scientific_asm:
    push rbx
    sub rsp, 48                    # 16-aligned before call
    mov rbx, rdx                   # output
    lea r8, [rdi + rsi]            # end of scientific input
    lea r9, [rsp]                  # scratch significant digits [0..17]
    xor r10d, r10d                 # digit count
.Lscan_mantissa:
    cmp rdi, r8
    jae .Lend_scan                 # scientific input always contains 'e'
    mov al, BYTE PTR [rdi]
    inc rdi
    cmp al, 'e'
    je .Lend_scan
    cmp al, 'E'
    je .Lend_scan
    cmp al, '.'
    je .Lscan_mantissa
    mov BYTE PTR [r9 + r10], al
    inc r10d
    jmp .Lscan_mantissa
.Lend_scan:
    xor esi, esi                   # negative exponent flag
    cmp rdi, r8
    jae .Lexp_done
    mov al, BYTE PTR [rdi]
    cmp al, '-'
    jne .Lplus_exp
    mov esi, 1
    inc rdi
    jmp .Lexp_begin
.Lplus_exp:
    cmp al, '+'
    jne .Lexp_begin
    inc rdi
.Lexp_begin:
    xor r11d, r11d                # exponent as positive int
.Lexp_loop:
    cmp rdi, r8
    jae .Lexp_end
    imul r11d, r11d, 10
    movzx eax, BYTE PTR [rdi]
    sub eax, '0'
    add r11d, eax
    inc rdi
    jmp .Lexp_loop
.Lexp_end:
    test esi, esi
    jz .Lexp_done
    neg r11d
.Lexp_done:
    inc r11d                        # decimal point = scientific exp + 1
    lea rdi, [rsp]
    mov esi, r10d
    mov edx, r11d
    mov rcx, rbx
    call xyo_ecma_format_asm
    add rsp, 48
    pop rbx
    ret
    .size xyo_ecma_from_scientific_asm, .-xyo_ecma_from_scientific_asm

    .section .rodata
    .p2align 4
.Ldecimal_pairs:
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
