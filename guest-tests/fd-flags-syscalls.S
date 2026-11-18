    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/*
 * Flag and edge cases for pipe2, dup3 and accept4.  Each failing check
 * exits with its own number (10..17).  Check 12 only asserts -EINVAL for
 * pipe2(O_DIRECT); the runner checks the diagnostics report for the
 * pipe2-packet line.
 */
_start:
    /* 10: pipe2(fds, O_NONBLOCK|O_CLOEXEC) */
    ldr r0, =fds
    ldr r1, =0x80800
    movw r7, #359
    svc #0
    cmp r0, #0
    bne f10
    ldr r6, =fds
    mov r4, #0
c10:
    ldr r0, [r6, r4, lsl #2]
    mov r1, #3
    mov r2, #0
    movw r7, #221
    svc #0
    tst r0, #0x800
    beq f10
    ldr r0, [r6, r4, lsl #2]
    mov r1, #1
    mov r2, #0
    movw r7, #221
    svc #0
    cmp r0, #1
    bne f10
    add r4, r4, #1
    cmp r4, #2
    blt c10

    /* 11: pipe2(x, 1) -> -EINVAL */
    ldr r0, =tmp
    mov r1, #1
    movw r7, #359
    svc #0
    cmn r0, #22
    bne f11

    /* 12: pipe2(x, O_DIRECT) -> -EINVAL */
    ldr r0, =tmp
    ldr r1, =0x10000
    movw r7, #359
    svc #0
    cmn r0, #22
    bne f12

    /* 13: dup3(a, a, 0) and dup3(a, b, 1) -> -EINVAL */
    ldr r6, =fds
    ldr r0, [r6]
    mov r1, r0
    mov r2, #0
    movw r7, #358
    svc #0
    cmn r0, #22
    bne f13
    ldr r0, [r6]
    mov r1, #50
    mov r2, #1
    movw r7, #358
    svc #0
    cmn r0, #22
    bne f13

    /* 14: dup3(p[0], fds[1], O_CLOEXEC) onto an open fd */
    ldr r0, =pipe_b
    mov r1, #0
    movw r7, #359
    svc #0
    cmp r0, #0
    bne f14
    ldr r5, =pipe_b
    ldr r6, =fds
    ldr r0, [r5]
    ldr r1, [r6, #4]
    ldr r2, =0x80000
    movw r7, #358
    svc #0
    ldr r1, [r6, #4]
    cmp r0, r1
    bne f14
    ldr r0, [r6, #4]
    mov r1, #1
    mov r2, #0
    movw r7, #221
    svc #0
    cmp r0, #1
    bne f14
    ldr r0, [r5, #4]
    ldr r1, =one
    mov r2, #1
    mov r7, #4
    svc #0
    cmp r0, #1
    bne f14
    ldr r6, =fds
    ldr r0, [r6, #4]
    ldr r1, =buffer
    mov r2, #1
    mov r7, #3
    svc #0
    cmp r0, #1
    bne f14

    /* 15: dup3 onto a wake eventfd, 200 times; its hidden end must be
       released each time or eventfd2 runs out of hidden numbers. */
    mov r4, #200
c15:
    mov r0, #0
    ldr r1, =0x80800
    movw r7, #356
    svc #0
    cmp r0, #0
    blt f15
    mov r6, r0
    ldr r5, =pipe_b
    ldr r0, [r5]
    mov r1, r6
    mov r2, #0
    movw r7, #358
    svc #0
    cmp r0, r6
    bne f15
    mov r0, r6
    mov r7, #6
    svc #0
    subs r4, r4, #1
    bne c15

    /* 16: hidden base -> -EBADF both ways */
    mov r0, #0
    mov r1, #7
    mov r2, #0
    ldr r3, =old_limit
    movw r7, #369
    svc #0
    cmp r0, #0
    bne f16
    ldr r3, =old_limit
    ldr r8, [r3]
    ldr r5, =pipe_b
    ldr r0, [r5]
    mov r1, r8
    mov r2, #0
    movw r7, #358
    svc #0
    cmn r0, #9
    bne f16
    mov r0, r8
    ldr r5, =pipe_b
    ldr r1, [r5]
    mov r2, #0
    movw r7, #358
    svc #0
    cmn r0, #9
    bne f16

    /* 17: accept4 */
    mov r0, #2
    mov r1, #1
    mov r2, #0
    movw r7, #281
    svc #0
    cmp r0, #0
    blt f17
    mov r4, r0
    ldr r1, =addr
    mov r2, #16
    movw r7, #282
    mov r0, r4
    svc #0
    cmp r0, #0
    bne f17
    mov r0, r4
    mov r1, #4
    movw r7, #284
    svc #0
    cmp r0, #0
    bne f17
    ldr r1, =addr
    ldr r2, =addrlen
    mov r0, r4
    movw r7, #286
    svc #0
    cmp r0, #0
    bne f17
    mov r0, #2
    mov r1, #1
    mov r2, #0
    movw r7, #281
    svc #0
    cmp r0, #0
    blt f17
    mov r5, r0
    ldr r1, =addr
    mov r2, #16
    movw r7, #283
    svc #0
    cmp r0, #0
    bne f17
    mov r0, r4
    mov r1, #0
    mov r2, #0
    ldr r3, =0x80800
    movw r7, #366
    svc #0
    cmp r0, #0
    blt f17
    mov r6, r0
    mov r1, #3
    mov r2, #0
    movw r7, #221
    svc #0
    tst r0, #0x800
    beq f17
    mov r0, r6
    mov r1, #1
    mov r2, #0
    movw r7, #221
    svc #0
    cmp r0, #1
    bne f17
    mov r0, r4
    mov r1, #0
    mov r2, #0
    mov r3, #1
    movw r7, #366
    svc #0
    cmn r0, #22
    bne f17

    mov r0, #0
    b exit_guest

f10: mov r0, #10; b exit_guest
f11: mov r0, #11; b exit_guest
f12: mov r0, #12; b exit_guest
f13: mov r0, #13; b exit_guest
f14: mov r0, #14; b exit_guest
f15: mov r0, #15; b exit_guest
f16: mov r0, #16; b exit_guest
f17: mov r0, #17
exit_guest:
    mov r7, #1
    svc #0
1:  b 1b
    .size _start, .-_start
    .ltorg

    .section .rodata
one: .ascii "x"

    .section .data
    .align 3
addr: .hword 2
    .byte 0, 0
    .byte 127, 0, 0, 1
    .space 8
addrlen: .word 16

    .section .bss
    .align 3
fds: .space 8
pipe_b: .space 8
tmp: .space 8
old_limit: .space 16
buffer: .space 8
