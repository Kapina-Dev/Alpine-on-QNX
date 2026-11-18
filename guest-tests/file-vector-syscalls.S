    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/*
 * preadv, pwritev, preadv2, pwritev2 and sendfile64.  Each failing check
 * exits with its own number (10..24); 9 is a setup failure.  The file is
 * /tmp/linuxemu-vec: 64 bytes of pattern, later 124 bytes.
 */
    .macro sys nr
    movw r7, #\nr
    svc #0
    .endm
    .macro want value, fail
    cmp r0, #\value
    bne \fail
    .endm
    .macro wanterr value, fail
    cmn r0, #\value
    bne \fail
    .endm

_start:
    /* setup: create the file with 64 bytes, position 0 */
    ldr r0, =path
    ldr r1, =0x242
    ldr r2, =0x180
    sys 5
    cmp r0, #0
    blt f9
    mov r8, r0
    mov r0, r8
    ldr r1, =pattern
    mov r2, #64
    sys 4
    want 64, f9
    mov r0, r8
    mov r1, #0
    mov r2, #0
    sys 19
    want 0, f9
    ldr r0, =path_out
    ldr r1, =0x242
    ldr r2, =0x180
    sys 5
    cmp r0, #0
    blt f9
    mov r9, r0
    /* the hidden-range number is the RLIMIT_NOFILE value */
    mov r0, #0
    mov r1, #7
    mov r2, #0
    ldr r3, =old_limit
    sys 369
    want 0, f9
    ldr r3, =old_limit
    ldr r10, [r3]

    /* 10: pwritev(fd, iov3, 3, 100, 0) = 24, position still 0 */
    mov r0, r8
    ldr r1, =wiov
    mov r2, #3
    mov r3, #100
    mov r4, #0
    sys 362
    want 24, f10
    mov r0, r8
    mov r1, #0
    mov r2, #1
    sys 19
    want 0, f10

    /* 11: preadv(fd, iov3, 3, 100, 0) = 24 with matching bytes */
    mov r0, r8
    ldr r1, =riov
    mov r2, #3
    mov r3, #100
    mov r4, #0
    sys 361
    want 24, f11
    ldr r0, =rbuf
    ldr r1, =wdata
    mov r2, #24
    bl memeq
    want 1, f11
    mov r0, r8
    mov r1, #0
    mov r2, #1
    sys 19
    want 0, f11

    /* 12: preadv at 120 of a 124-byte file = 4 */
    mov r0, r8
    ldr r1, =riov
    mov r2, #3
    mov r3, #120
    mov r4, #0
    sys 361
    want 4, f12

    /* 13: preadv2 offset -1 reads at the position and advances it by 8 */
    mov r0, r8
    ldr r1, =riov1
    mov r2, #1
    mvn r3, #0
    mvn r4, #0
    mov r5, #0
    sys 392
    want 8, f13
    ldr r0, =rbuf2
    ldr r1, =pattern
    mov r2, #8
    bl memeq
    want 1, f13
    mov r0, r8
    mov r1, #0
    mov r2, #1
    sys 19
    want 8, f13

    /* 14: pwritev2 with RWF_NOWAIT (8) and RWF_HIPRI (1) = -EOPNOTSUPP */
    mov r0, r8
    ldr r1, =wiov
    mov r2, #3
    mov r3, #0
    mov r4, #0
    mov r5, #8
    sys 393
    wanterr 95, f14
    mov r0, r8
    ldr r1, =wiov
    mov r2, #3
    mov r3, #0
    mov r4, #0
    mov r5, #1
    sys 393
    wanterr 95, f14

    /* 15: iovcnt 1025 and -1 = -EINVAL */
    mov r0, r8
    ldr r1, =riov
    ldr r2, =1025
    mov r3, #0
    mov r4, #0
    sys 361
    wanterr 22, f15
    mov r0, r8
    ldr r1, =riov
    mvn r2, #0
    mov r3, #0
    mov r4, #0
    sys 361
    wanterr 22, f15

    /* 16: negative offset (hi = 0xffffffff) = -EINVAL */
    mov r0, r8
    ldr r1, =riov
    mov r2, #3
    mov r3, #5
    mvn r4, #0
    sys 361
    wanterr 22, f16

    /* 17: offset 5 GiB moves no bytes (EOF, or the error QNX gives for a
       read that far out; see large-offset-syscalls) */
    mov r0, r8
    ldr r1, =riov
    mov r2, #3
    ldr r3, =0x40000000
    mov r4, #1
    sys 361
    cmp r0, #0
    bgt f17

    /* 18: a hidden-range fd = -EBADF (preadv and pwritev) */
    mov r0, r10
    ldr r1, =riov
    mov r2, #3
    mov r3, #0
    mov r4, #0
    sys 361
    wanterr 9, f18
    mov r0, r10
    ldr r1, =wiov
    mov r2, #3
    mov r3, #0
    mov r4, #0
    sys 362
    wanterr 9, f18

    /* 19: sendfile64(out, in, &off=10, 20) = 20, off = 30, in pos kept */
    mov r0, r8
    mov r1, #5
    mov r2, #0
    sys 19
    want 5, f19
    ldr r1, =offset
    mov r2, #10
    str r2, [r1]
    mov r2, #0
    str r2, [r1, #4]
    mov r0, r9
    mov r1, r8
    ldr r2, =offset
    mov r3, #20
    sys 239
    want 20, f19
    ldr r1, =offset
    ldr r2, [r1]
    cmp r2, #30
    bne f19
    ldr r2, [r1, #4]
    cmp r2, #0
    bne f19
    mov r0, r8
    mov r1, #0
    mov r2, #1
    sys 19
    want 5, f19

    /* 20: sendfile64(out, in, NULL, 20) = 20, in position 5 -> 25 */
    mov r0, r9
    mov r1, r8
    mov r2, #0
    mov r3, #20
    sys 239
    want 20, f20
    mov r0, r8
    mov r1, #0
    mov r2, #1
    sys 19
    want 25, f20

    /* 21: a hidden in or out fd = -EBADF */
    mov r0, r9
    mov r1, r10
    mov r2, #0
    mov r3, #20
    sys 239
    wanterr 9, f21
    mov r0, r10
    mov r1, r8
    mov r2, #0
    mov r3, #20
    sys 239
    wanterr 9, f21

    /* 22: a NULL iov with count 1 = -EFAULT, all four calls */
    mov r0, r8
    mov r1, #0
    mov r2, #1
    mov r3, #0
    mov r4, #0
    sys 361
    wanterr 14, f22
    mov r0, r8
    mov r1, #0
    mov r2, #1
    mov r3, #0
    mov r4, #0
    sys 362
    wanterr 14, f22
    mov r0, r8
    mov r1, #0
    mov r2, #1
    mov r3, #0
    mov r4, #0
    mov r5, #0
    sys 392
    wanterr 14, f22
    mov r0, r8
    mov r1, #0
    mov r2, #1
    mov r3, #0
    mov r4, #0
    mov r5, #0
    sys 393
    wanterr 14, f22

    /* 23: sendfile64 from a pipe input = -EINVAL */
    ldr r0, =pipefds
    sys 42
    want 0, f23
    ldr r3, =pipefds
    ldr r1, [r3]
    mov r0, r9
    mov r2, #0
    mov r3, #20
    sys 239
    wanterr 22, f23

    /* 24: a closed, non-hidden in or out fd = -EBADF */
    ldr r0, =path_out
    mov r1, #2
    mov r2, #0
    sys 5
    cmp r0, #0
    blt f24
    mov r6, r0
    mov r0, r6
    sys 6
    mov r0, r9
    mov r1, r6
    mov r2, #0
    mov r3, #20
    sys 239
    wanterr 9, f24
    mov r0, r6
    mov r1, r8
    mov r2, #0
    mov r3, #20
    sys 239
    wanterr 9, f24

    ldr r0, =path
    sys 10
    ldr r0, =path_out
    sys 10
    mov r0, #0
    b exit_guest

f9:  mov r0, #9;  b exit_guest
f10: mov r0, #10; b exit_guest
f11: mov r0, #11; b exit_guest
f12: mov r0, #12; b exit_guest
f13: mov r0, #13; b exit_guest
f14: mov r0, #14; b exit_guest
f15: mov r0, #15; b exit_guest
f16: mov r0, #16; b exit_guest
f17: mov r0, #17; b exit_guest
f18: mov r0, #18; b exit_guest
f19: mov r0, #19; b exit_guest
f20: mov r0, #20; b exit_guest
f21: mov r0, #21; b exit_guest
f22: mov r0, #22; b exit_guest
f23: mov r0, #23; b exit_guest
f24: mov r0, #24
exit_guest:
    mov r7, #1
    svc #0
1:  b 1b

/* r0 = 1 when the r2 bytes at r0 and r1 match */
memeq:
    ldrb r3, [r0], #1
    ldrb r12, [r1], #1
    cmp r3, r12
    bne 1f
    subs r2, r2, #1
    bne memeq
    mov r0, #1
    bx lr
1:  mov r0, #0
    bx lr
    .size _start, .-_start
    .ltorg

    .section .rodata
path: .asciz "/tmp/linuxemu-vec"
path_out: .asciz "/tmp/linuxemu-vec-out"
pattern: .ascii "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
wdata: .ascii "0123456789abcdefghijklmn"

    .section .data
    .align 3
wiov: .word wdata, 8, wdata+8, 8, wdata+16, 8
riov: .word rbuf, 8, rbuf+8, 8, rbuf+16, 8
riov1: .word rbuf2, 8

    .section .bss
    .align 3
rbuf: .space 24
rbuf2: .space 8
offset: .space 8
pipefds: .space 8
old_limit: .space 16
