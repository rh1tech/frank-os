/*
 * pshell_setjmp.S — ARM Thumb-2 setjmp/longjmp for FRANK OS pshell
 *
 * jmp_buf layout (11 words = 44 bytes):
 *   [0] r4  [1] r5  [2] r6  [3] r7  [4] r8  [5] r9
 *   [6] r10 [7] r11 [8] sp  [9] lr  [10] reserved
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

    .syntax unified
    .cpu    cortex-m33
    .thumb

    .global setjmp
    .type   setjmp, %function
    .thumb_func
setjmp:
    /* r0 = jmp_buf pointer */
    stmia   r0!, {r4-r7}       /* save r4-r7 */
    mov     r1, r8
    mov     r2, r9
    mov     r3, r10
    stmia   r0!, {r1-r3}       /* save r8-r10 */
    mov     r1, r11
    mov     r2, sp
    mov     r3, lr
    stmia   r0!, {r1-r3}       /* save r11, sp, lr */
    movs    r0, #0              /* return 0 */
    bx      lr
    .size   setjmp, .-setjmp

    .global longjmp
    .type   longjmp, %function
    .thumb_func
longjmp:
    /* r0 = jmp_buf pointer, r1 = return value */
    /* If val == 0, force it to 1 (POSIX requirement) */
    movs    r2, r1
    cmp     r2, #0
    bne     1f
    movs    r2, #1
1:
    /* Restore r4-r7 */
    ldmia   r0!, {r4-r7}
    /* Restore r8-r10 */
    ldmia   r0!, {r1, r3, r5}  /* temp regs for mov to high regs */
    mov     r8, r1
    mov     r9, r3
    mov     r10, r5
    /* Restore r11, sp, lr */
    ldmia   r0!, {r1, r3, r5}
    mov     r11, r1
    mov     sp, r3
    mov     lr, r5
    /* Return val (in r2) */
    mov     r0, r2
    bx      lr
    .size   longjmp, .-longjmp
