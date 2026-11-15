# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#
# Minimal x86-64 glibc entry point for the target helper.  The helper is
# linked with -nostdlib against the target rootfs libc/libdl (2.25); this
# avoids the host crt objects and the host glibc 2.4x startup.

        .text
        .globl _start
        .type _start,@function
_start:
        xorl    %ebp, %ebp
        movq    %rdx, %r9               /* rtld_fini */
        popq    %rsi                    /* argc */
        movq    %rsp, %rdx              /* argv */
        andq    $-16, %rsp
        pushq   %rax                    /* alignment */
        pushq   %rsp                    /* stack_end */
        xorl    %r8d, %r8d              /* fini */
        xorl    %ecx, %ecx              /* init */
        leaq    main(%rip), %rdi
        call    __libc_start_main@PLT
        hlt
        .size _start, .-_start

        .section .note.GNU-stack,"",@progbits
