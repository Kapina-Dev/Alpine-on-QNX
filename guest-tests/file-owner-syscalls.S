    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/*
 * fchmod, faccessat2, chown32/lchown32/fchownat, mknod and mknodat.  Each
 * failing check exits with its own number (10..18); 9 is a setup failure.
 * Files live under /tmp/linuxemu-owner*; all are removed on exit.
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
    bl cleanup
    ldr r0, =path
    ldr r1, =0x242
    ldr r2, =0x180
    sys 5
    cmp r0, #0
    blt f9
    mov r8, r0
    ldr r0, =path
    ldr r1, =linkpath
    sys 83
    want 0, f9
    ldr r0, =dirpath
    ldr r1, =0x1c0
    sys 39
    want 0, f9
    ldr r0, =dirpath
    mov r1, #0
    mov r2, #0
    sys 5
    cmp r0, #0
    blt f9
    mov r9, r0
    mov r0, #0
    mov r1, #7
    mov r2, #0
    ldr r3, =old_limit
    sys 369
    want 0, f9
    ldr r3, =old_limit
    ldr r10, [r3]
    sys 199
    mov r4, r0
    sys 200
    mov r5, r0
    ldr r3, =ids
    str r4, [r3]
    str r5, [r3, #4]

    /* 10: fchmod(fd, 0600) then fstat mode & 0777 == 0600; hidden = EBADF */
    mov r0, r8
    ldr r1, =0x1ed
    sys 94
    want 0, f10
    mov r0, r8
    ldr r1, =0x180
    sys 94
    want 0, f10
    mov r0, r8
    ldr r1, =statbuf
    sys 197
    want 0, f10
    ldr r1, =statbuf
    ldr r0, [r1, #16]
    ldr r2, =0x1ff
    and r0, r0, r2
    ldr r2, =0x180
    cmp r0, r2
    bne f10
    mov r0, r10
    ldr r1, =0x180
    sys 94
    wanterr 9, f10

    /* 11: faccessat2: plain and AT_EACCESS succeed; flag 0x1 = EINVAL */
    ldr r0, =-100
    ldr r1, =path
    mov r2, #4
    mov r3, #0
    sys 439
    want 0, f11
    ldr r0, =-100
    ldr r1, =path
    mov r2, #4
    ldr r3, =0x200
    sys 439
    want 0, f11
    ldr r0, =-100
    ldr r1, =path
    mov r2, #4
    mov r3, #1
    sys 439
    wanterr 22, f11
    ldr r0, =-100
    ldr r1, =path
    mov r2, #4
    ldr r3, =0x100
    sys 439
    want 0, f11

    /* 12: chown32 to own ids, and to -1/-1 */
    ldr r0, =path
    ldr r3, =ids
    ldr r1, [r3]
    ldr r2, [r3, #4]
    sys 212
    want 0, f12
    ldr r0, =path
    mvn r1, #0
    mvn r2, #0
    sys 212
    want 0, f12

    /* 13: chown32 to 0:0 as the ordinary user = -1 (EPERM) */
    ldr r0, =path
    mov r1, #0
    mov r2, #0
    sys 212
    wanterr 1, f13

    /* 14: lchown32 on a symlink and fchownat NOFOLLOW, own ids */
    ldr r0, =linkpath
    ldr r3, =ids
    ldr r1, [r3]
    ldr r2, [r3, #4]
    sys 198
    want 0, f14
    ldr r0, =-100
    ldr r1, =linkpath
    ldr r3, =ids
    ldr r2, [r3]
    ldr r3, [r3, #4]
    ldr r4, =0x100
    sys 325
    want 0, f14
    /* fchown32 on the fd works, a hidden fd is EBADF */
    mov r0, r8
    ldr r3, =ids
    ldr r1, [r3]
    ldr r2, [r3, #4]
    sys 207
    want 0, f14
    mov r0, r10
    mvn r1, #0
    mvn r2, #0
    sys 207
    wanterr 9, f14
    /* fchownat AT_EMPTY_PATH on the fd */
    mov r0, r8
    ldr r1, =emptypath
    mvn r2, #0
    mvn r3, #0
    ldr r4, =0x1000
    sys 325
    want 0, f14

    /* 15: mknod S_IFIFO|0600 and stat shows S_IFIFO */
    ldr r0, =fifopath
    ldr r1, =0x1180
    mov r2, #0
    sys 14
    want 0, f15
    ldr r0, =fifopath
    ldr r1, =statbuf
    sys 195
    want 0, f15
    ldr r1, =statbuf
    ldr r0, [r1, #16]
    and r0, r0, #0xf000
    cmp r0, #0x1000
    bne f15

    /* 16: mknod S_IFREG|0600 creates an empty file */
    ldr r0, =regpath
    ldr r1, =0x8180
    mov r2, #0
    sys 14
    want 0, f16
    ldr r0, =regpath
    ldr r1, =statbuf
    sys 195
    want 0, f16
    ldr r1, =statbuf
    ldr r0, [r1, #16]
    and r0, r0, #0xf000
    cmp r0, #0x8000
    bne f16
    ldr r0, [r1, #48]
    cmp r0, #0
    bne f16

    /* 17: S_IFCHR and S_IFSOCK = -1 (EPERM) */
    ldr r0, =chrpath
    ldr r1, =0x2180
    ldr r2, =0x101
    sys 14
    wanterr 1, f17
    ldr r0, =sockpath
    ldr r1, =0xc180
    mov r2, #0
    sys 14
    wanterr 1, f17

    /* 18: mknodat(dirfd, "rel", S_IFIFO|0600) resolves against dirfd */
    mov r0, r9
    ldr r1, =relname
    ldr r2, =0x1180
    mov r3, #0
    sys 324
    want 0, f18
    ldr r0, =dirrel
    ldr r1, =statbuf
    sys 195
    want 0, f18
    ldr r1, =statbuf
    ldr r0, [r1, #16]
    and r0, r0, #0xf000
    cmp r0, #0x1000
    bne f18

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
    mov r4, r0
    bl cleanup
    mov r0, r4
    mov r7, #1
    svc #0
1:  b 1b

cleanup:
    push {r4, lr}
    ldr r0, =path
    sys 10
    ldr r0, =linkpath
    sys 10
    ldr r0, =fifopath
    sys 10
    ldr r0, =regpath
    sys 10
    ldr r0, =chrpath
    sys 10
    ldr r0, =sockpath
    sys 10
    ldr r0, =dirrel
    sys 10
    ldr r0, =dirpath
    sys 40
    pop {r4, pc}
    .size _start, .-_start
    .ltorg

    .section .rodata
path: .asciz "/tmp/linuxemu-owner"
linkpath: .asciz "/tmp/linuxemu-owner-link"
fifopath: .asciz "/tmp/linuxemu-owner-fifo"
regpath: .asciz "/tmp/linuxemu-owner-reg"
chrpath: .asciz "/tmp/linuxemu-owner-chr"
sockpath: .asciz "/tmp/linuxemu-owner-sock"
dirpath: .asciz "/tmp/linuxemu-owner-dir"
dirrel: .asciz "/tmp/linuxemu-owner-dir/rel"
relname: .asciz "rel"
emptypath: .asciz ""

    .section .bss
    .align 3
statbuf: .space 128
old_limit: .space 16
ids: .space 8
