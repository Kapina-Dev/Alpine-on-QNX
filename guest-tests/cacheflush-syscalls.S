    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/* ARM private cacheflush (0x0f0002). Exit codes name the failing check;
   0 is success. */

_start:
    /* map an RWX anonymous page into r4 */
    mov r0, #0
    mov r1, #4096
    mov r2, #7
    mov r3, #0x22
    mvn r4, #0
    mov r5, #0
    mov r7, #192
    svc #0
    cmn r0, #4096
    bhi fail9
    mov r4, r0

    /* 10: write "mov r0,#42; bx lr", cacheflush returns 0 */
    ldr r1, =insn42
    ldr r2, [r1]
    str r2, [r4]
    ldr r2, [r1, #4]
    str r2, [r4, #4]
    mov r0, r4
    add r1, r4, #8
    mov r2, #0
    ldr r7, =0x0f0002
    svc #0
    cmp r0, #0
    bne fail10

    /* 11: calling the page returns 42 */
    blx r4
    cmp r0, #42
    bne fail11

    /* 12: rewrite to 43, flush, call returns 43 */
    ldr r1, =insn43
    ldr r2, [r1]
    str r2, [r4]
    mov r0, r4
    add r1, r4, #8
    mov r2, #0
    ldr r7, =0x0f0002
    svc #0
    cmp r0, #0
    bne fail12
    blx r4
    cmp r0, #43
    bne fail12

    /* 13: start > end -> EINVAL */
    add r0, r4, #8
    mov r1, r4
    mov r2, #0
    ldr r7, =0x0f0002
    svc #0
    cmn r0, #22
    bne fail13

    /* 14: flags 1 -> EINVAL */
    mov r0, r4
    add r1, r4, #8
    mov r2, #1
    ldr r7, =0x0f0002
    svc #0
    cmn r0, #22
    bne fail14

    /* 15: range that is not guest memory -> EINVAL */
    mov r0, #0x1000
    mov r1, #0x2000
    mov r2, #0
    ldr r7, =0x0f0002
    svc #0
    cmn r0, #22
    bne fail15

    mov r0, #0
    b done
fail9:  mov r0, #9
    b done
fail10: mov r0, #10
    b done
fail11: mov r0, #11
    b done
fail12: mov r0, #12
    b done
fail13: mov r0, #13
    b done
fail14: mov r0, #14
    b done
fail15: mov r0, #15
done:
    mov r7, #1
    svc #0

    .ltorg
    .section .data
    .align 2
insn42: .word 0xe3a0002a
        .word 0xe12fff1e
insn43: .word 0xe3a0002b
