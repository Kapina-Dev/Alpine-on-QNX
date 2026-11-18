    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/*
 * statx, statfs64 and fstatfs64.  Each failing check exits with its own
 * number (10..18); 9 is a setup failure.  Files: /tmp/linuxemu-statx and
 * the symlink /tmp/linuxemu-statx-link.
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
    /* setup: a 37-byte file, a symlink to it, the hidden-range number */
    ldr r0, =path
    ldr r1, =0x242
    ldr r2, =0x180
    sys 5
    cmp r0, #0
    blt f9
    mov r8, r0
    mov r0, r8
    ldr r1, =data
    mov r2, #37
    sys 4
    want 37, f9
    ldr r0, =linkpath
    sys 10
    ldr r0, =path
    ldr r1, =linkpath
    sys 83
    want 0, f9
    mov r0, #0
    mov r1, #7
    mov r2, #0
    ldr r3, =old_limit
    sys 369
    want 0, f9
    ldr r3, =old_limit
    ldr r10, [r3]

    /* 10: statx on /tmp: basic stats, no btime, a directory */
    ldr r0, =-100
    ldr r1, =tmppath
    mov r2, #0
    ldr r3, =0x7ff
    ldr r4, =sxbuf
    sys 397
    want 0, f10
    ldr r1, =sxbuf
    ldr r0, [r1]
    ldr r2, =0x7ff
    and r3, r0, r2
    cmp r3, r2
    bne f10
    tst r0, #0x800
    bne f10
    ldrh r0, [r1, #28]
    and r0, r0, #0xf000
    cmp r0, #0x4000
    bne f10

    /* 11: regular file: size 37, ino and nlink match fstatat64 */
    ldr r0, =-100
    ldr r1, =path
    mov r2, #0
    ldr r3, =0x7ff
    ldr r4, =sxbuf
    sys 397
    want 0, f11
    ldr r0, =-100
    ldr r1, =path
    ldr r2, =statbuf
    mov r3, #0
    sys 327
    want 0, f11
    ldr r1, =sxbuf
    ldr r2, =statbuf
    ldr r0, [r1, #40]
    cmp r0, #37
    bne f11
    ldr r0, [r1, #44]
    cmp r0, #0
    bne f11
    ldr r0, [r1, #32]
    ldr r3, [r2, #96]
    cmp r0, r3
    bne f11
    ldr r0, [r1, #16]
    ldr r3, [r2, #20]
    cmp r0, r3
    bne f11

    /* 12: AT_EMPTY_PATH equals fstat64 on ino and size */
    mov r0, r8
    ldr r1, =emptypath
    ldr r2, =0x1000
    ldr r3, =0x7ff
    ldr r4, =sxbuf
    sys 397
    want 0, f12
    mov r0, r8
    ldr r1, =statbuf
    sys 197
    want 0, f12
    ldr r1, =sxbuf
    ldr r2, =statbuf
    ldr r0, [r1, #32]
    ldr r3, [r2, #96]
    cmp r0, r3
    bne f12
    ldr r0, [r1, #40]
    ldr r3, [r2, #48]
    cmp r0, r3
    bne f12

    /* 13: AT_SYMLINK_NOFOLLOW on the symlink gives S_IFLNK */
    ldr r0, =-100
    ldr r1, =linkpath
    mov r2, #0x100
    ldr r3, =0x7ff
    ldr r4, =sxbuf
    sys 397
    want 0, f13
    ldr r1, =sxbuf
    ldrh r0, [r1, #28]
    and r0, r0, #0xf000
    cmp r0, #0xa000
    bne f13

    /* 14: unknown flag bits EINVAL; the sync-type bits are accepted */
    ldr r0, =-100
    ldr r1, =path
    ldr r2, =0x80000000
    ldr r3, =0x7ff
    ldr r4, =sxbuf
    sys 397
    wanterr 22, f14
    ldr r0, =-100
    ldr r1, =path
    ldr r2, =0x2000
    ldr r3, =0x7ff
    ldr r4, =sxbuf
    sys 397
    want 0, f14
    ldr r0, =-100
    ldr r1, =path
    ldr r2, =0x4000
    ldr r3, =0x7ff
    ldr r4, =sxbuf
    sys 397
    want 0, f14

    /* 15: a NULL buffer is EFAULT */
    ldr r0, =-100
    ldr r1, =path
    mov r2, #0
    ldr r3, =0x7ff
    mov r4, #0
    sys 397
    wanterr 14, f15

    /* 16: AT_EMPTY_PATH on a hidden fd is EBADF */
    mov r0, r10
    ldr r1, =emptypath
    ldr r2, =0x1000
    ldr r3, =0x7ff
    ldr r4, =sxbuf
    sys 397
    wanterr 9, f16

    /* 17: statfs64 accepts 88 and 84 (guard intact), rejects 80 */
    ldr r0, =tmppath
    mov r1, #88
    ldr r2, =sfbuf
    sys 266
    want 0, f17
    ldr r1, =sfbuf
    ldr r0, =0xdeadbeef
    str r0, [r1, #84]
    ldr r0, =tmppath
    mov r1, #84
    ldr r2, =sfbuf
    sys 266
    want 0, f17
    ldr r1, =sfbuf
    ldr r0, [r1, #84]
    ldr r2, =0xdeadbeef
    cmp r0, r2
    bne f17
    ldr r0, =tmppath
    mov r1, #80
    ldr r2, =sfbuf
    sys 266
    wanterr 22, f17

    /* 18: fstatfs64 accepts 84 */
    mov r0, r8
    mov r1, #84
    ldr r2, =sfbuf
    sys 267
    want 0, f18

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
path: .asciz "/tmp/linuxemu-statx"
linkpath: .asciz "/tmp/linuxemu-statx-link"
tmppath: .asciz "/tmp"
emptypath: .asciz ""
data: .asciz "0123456789012345678901234567890123456"

    .section .bss
    .align 3
statbuf: .space 128
sxbuf: .space 256
sfbuf: .space 96
old_limit: .space 16
