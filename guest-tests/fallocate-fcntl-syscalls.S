    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/*
 * fallocate and the fcntl64 additions.  Each failing check exits with its
 * own number (10..18); 9 is a setup failure.  The file is
 * /tmp/linuxemu-falloc.
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
    /* setup: create the file, empty */
    ldr r0, =path
    ldr r1, =0x242
    ldr r2, =0x180
    sys 5
    cmp r0, #0
    blt f9
    mov r8, r0
    /* the hidden-range number is the RLIMIT_NOFILE value */
    mov r0, #0
    mov r1, #7
    mov r2, #0
    ldr r3, =old_limit
    sys 369
    want 0, f9
    ldr r3, =old_limit
    ldr r10, [r3]

    /* 10: fallocate(fd, 0, 0, 8192) = 0 and the size is at least 8192 */
    mov r0, r8
    mov r1, #0
    mov r2, #0
    mov r3, #0
    ldr r4, =8192
    mov r5, #0
    sys 352
    want 0, f10
    mov r0, r8
    ldr r1, =statbuf
    sys 197
    want 0, f10
    ldr r0, =statbuf
    ldr r0, [r0, #48]
    ldr r1, =8192
    cmp r0, r1
    blo f10

    /* 11: KEEP_SIZE and punch hole (mode 3) = -EOPNOTSUPP */
    mov r0, r8
    mov r1, #1
    mov r2, #0
    mov r3, #0
    mov r4, #4096
    mov r5, #0
    sys 352
    wanterr 95, f11
    mov r0, r8
    mov r1, #3
    mov r2, #0
    mov r3, #0
    mov r4, #4096
    mov r5, #0
    sys 352
    wanterr 95, f11

    /* 12: a negative offset (either word pattern) or a zero or negative
       length = -EINVAL */
    mov r0, r8
    mov r1, #0
    mvn r2, #0
    mvn r3, #0
    mov r4, #4096
    mov r5, #0
    sys 352
    wanterr 22, f12
    mov r0, r8
    mov r1, #0
    mov r2, #0
    mov r3, #0x80000000
    mov r4, #4096
    mov r5, #0
    sys 352
    wanterr 22, f12
    mov r0, r8
    mov r1, #0
    mov r2, #0
    mov r3, #0
    mov r4, #0
    mov r5, #0
    sys 352
    wanterr 22, f12
    mov r0, r8
    mov r1, #0
    mov r2, #0
    mov r3, #0
    mov r4, #4096
    mov r5, #0x80000000
    sys 352
    wanterr 22, f12

    /* 13: a hidden-range fd = -EBADF */
    mov r0, r10
    mov r1, #0
    mov r2, #0
    mov r3, #0
    mov r4, #4096
    mov r5, #0
    sys 352
    wanterr 9, f13

    /* 14: F_GETPIPE_SZ = P > 0, F_SETPIPE_SZ P = P, P * 64 = -EPERM */
    ldr r0, =pipefds
    sys 42
    want 0, f14
    ldr r0, =pipefds
    ldr r6, [r0]
    mov r0, r6
    ldr r1, =1032
    mov r2, #0
    sys 221
    cmp r0, #0
    ble f14
    mov r9, r0
    mov r0, r6
    ldr r1, =1031
    mov r2, r9
    sys 221
    cmp r0, r9
    bne f14
    mov r0, r6
    ldr r1, =1031
    lsl r2, r9, #6
    sys 221
    wanterr 1, f14
    /* a regular file has no pipe size: -EBADF */
    mov r0, r8
    ldr r1, =1032
    mov r2, #0
    sys 221
    wanterr 9, f14

    /* 15: F_SETOWN getpid, F_GETOWN: either the pid back, or -EINVAL
       from both (QNX has no owner support) */
    sys 20
    mov r11, r0
    mov r0, r8
    mov r1, #8
    mov r2, r11
    sys 221
    mov r4, r0
    mov r0, r8
    mov r1, #9
    mov r2, #0
    sys 221
    cmp r4, #0
    bne 1f
    cmp r0, r11
    bne f15
    b 2f
1:  cmn r4, #22
    bne f15
    cmn r0, #22
    bne f15
2:

    /* 16: F_OFD_SETLK write lock on bytes 0..9 = 0; a nonzero pid in the
       request = -EINVAL; a second open of the file in a fork child sees
       the lock with F_OFD_GETLK */
    ldr r0, =lock
    mov r1, #1
    strh r1, [r0]
    mov r1, #0
    strh r1, [r0, #2]
    mov r1, #0
    str r1, [r0, #8]
    str r1, [r0, #12]
    mov r1, #10
    str r1, [r0, #16]
    mov r1, #0
    str r1, [r0, #20]
    str r1, [r0, #24]
    mov r0, r8
    mov r1, #37
    ldr r2, =lock
    sys 221
    want 0, f16
    ldr r0, =lock
    mov r1, #7
    str r1, [r0, #24]
    mov r0, r8
    mov r1, #36
    ldr r2, =lock
    sys 221
    wanterr 22, f16
    ldr r0, =lock
    mov r1, #0
    str r1, [r0, #24]
    sys 2
    cmp r0, #0
    blt f16
    beq child16
    mov r4, r0
    mov r0, r4
    ldr r1, =status
    mov r2, #0
    mov r3, #0
    sys 114
    ldr r0, =status
    ldr r0, [r0]
    cmp r0, #0
    bne f16
    b 3f
child16:
    ldr r0, =path
    ldr r1, =2
    mov r2, #0
    sys 5
    cmp r0, #0
    blt cfail1
    mov r6, r0
    ldr r5, =lock
    mov r1, #1
    strh r1, [r5]
    mov r0, r6
    mov r1, #36
    mov r2, r5
    sys 221
    cmp r0, #0
    bne cfail2
    ldrh r0, [r5]
    cmp r0, #1
    bne cfail3
    mov r0, #0
    b cexit
cfail1: mov r0, #1
    b cexit
cfail2: mov r0, #2
    b cexit
cfail3: mov r0, #3
cexit:
    sys 1
3:

    /* 17: F_ADD_SEALS and F_GET_SEALS = -EINVAL */
    mov r0, r8
    ldr r1, =1033
    mov r2, #1
    sys 221
    wanterr 22, f17
    mov r0, r8
    ldr r1, =1034
    mov r2, #0
    sys 221
    wanterr 22, f17

    /* 18: a huge offset reaches QNX with its high word: the call fails
       fast.  A dropped high word would be offset 0xffffff01, which
       succeeds after a very long zero fill (QNX writes the blocks).
       Offset 2^63-1-0xff, length 0xff: neither negative nor overflowing. */
    mov r0, r8
    mov r1, #0
    mvn r2, #0xfe
    ldr r3, =0x7fffffff
    mov r4, #0xff
    mov r5, #0
    sys 352
    cmp r0, #0
    beq f18
    cmn r0, #9
    beq f18
    ldr r0, =path
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
f18: mov r0, #18
exit_guest:
    mov r7, #1
    svc #0
1:  b 1b
    .size _start, .-_start
    .ltorg

    .section .rodata
path: .asciz "/tmp/linuxemu-falloc"

    .section .bss
    .align 3
statbuf: .space 128
lock: .space 32
pipefds: .space 8
status: .space 8
old_limit: .space 16
