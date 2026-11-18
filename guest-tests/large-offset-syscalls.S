    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/*
 * 64-bit file offsets must reach the host intact.  The device filesystems
 * (qnx6 and exFAT alike) cap a file near 2 GiB, so no file can hold data
 * past 4 GiB.  Instead the file is small and holds a marker byte at 4096:
 * an offset of 4 GiB + 4096 whose high word is dropped lands on that marker,
 * while the intact offset is past EOF (a read there gives 0 or the error QNX
 * returns, never a byte).  Each failing check exits with its own
 * number (10..20); 9 is a setup failure.  File: /tmp/linuxemu-large, 4097
 * bytes: 'A'..'H' at 0 and the marker 0x5A at 4096.
 */
    .macro sys nr
    movw r7, #\nr
    svc #0
    .endm
    .macro want value, fail
    cmp r0, #\value
    bne \fail
    .endm

_start:
    ldr r0, =path
    ldr r1, =0x242
    ldr r2, =0x180
    sys 5
    cmp r0, #0
    blt f9
    mov r8, r0
    ldr r0, =path_out
    ldr r1, =0x242
    ldr r2, =0x180
    sys 5
    cmp r0, #0
    blt f9
    mov r9, r0
    /* pwrite64(fd, "ABCDEFGH", 8, 0) and pwrite64(fd, marker, 1, 4096) */
    mov r0, r8
    ldr r1, =pattern
    mov r2, #8
    mov r3, #0
    mov r4, #0
    mov r5, #0
    sys 181
    want 8, f9
    mov r0, r8
    ldr r1, =marker
    mov r2, #1
    mov r3, #0
    mov r4, #4096
    mov r5, #0
    sys 181
    want 1, f9
    mov r0, r8
    ldr r1, =statbuf
    sys 197
    want 0, f9
    ldr r1, =statbuf
    ldr r2, [r1, #48]
    ldr r3, [r1, #52]
    ldr r4, =4097
    cmp r2, r4
    bne f9
    cmp r3, #0
    bne f9

    /* 10: preadv at 4 GiB + 4096 moves no bytes (EOF, or the error QNX gives for
       a read that far out), never the marker */
    mov r0, r8
    ldr r1, =riov
    mov r2, #1
    mov r3, #4096
    mov r4, #1
    sys 361
    cmp r0, #0
    bgt f10

    /* 11: pread64 at 4 GiB + 4096 moves no bytes */
    ldr r1, =rbuf
    mov r0, #0
    strb r0, [r1]
    mov r0, r8
    mov r2, #1
    mov r3, #0
    mov r4, #4096
    mov r5, #1
    sys 180
    cmp r0, #0
    bgt f11

    /* 12: control, preadv at 4096 = the marker */
    mov r0, r8
    ldr r1, =riov
    mov r2, #1
    mov r3, #4096
    mov r4, #0
    sys 361
    want 1, f12
    ldr r1, =rbuf
    ldrb r2, [r1]
    cmp r2, #0x5a
    bne f12

    /* 13: pwritev at 4 GiB + 4096 fails (this filesystem's EFBIG); a
       dropped high word would overwrite the marker and succeed */
    mov r0, r8
    ldr r1, =wiov
    mov r2, #1
    mov r3, #4096
    mov r4, #1
    sys 362
    cmp r0, #0
    bge f13

    /* 14: pwrite64 at 4 GiB + 4096 fails the same way */
    mov r0, r8
    ldr r1, =pattern
    mov r2, #1
    mov r3, #0
    mov r4, #4096
    mov r5, #1
    sys 181
    cmp r0, #0
    bge f14

    /* 15: the marker survived checks 13 and 14 */
    mov r0, r8
    ldr r1, =rbuf
    mov r2, #1
    mov r3, #0
    mov r4, #4096
    mov r5, #0
    sys 180
    want 1, f15
    ldr r1, =rbuf
    ldrb r2, [r1]
    cmp r2, #0x5a
    bne f15

    /* 16: pread64 at 2 GiB + 4096 (bit 31 set) moves no bytes */
    mov r0, r8
    ldr r1, =rbuf
    mov r2, #1
    mov r3, #0
    ldr r4, =0x80001000
    mov r5, #0
    sys 180
    cmp r0, #0
    bgt f16

    /* 17: _llseek to 2 GiB + 4096 returns exactly that position, or fails
       where the filesystem refuses the offset */
    mov r0, r8
    mov r1, #0
    ldr r2, =0x80001000
    ldr r3, =res
    mov r4, #0
    sys 140
    cmp r0, #0
    blt 4f
    ldr r3, =res
    ldr r1, [r3]
    ldr r2, [r3, #4]
    ldr r4, =0x80001000
    cmp r1, r4
    bne f17
    cmp r2, #0
    bne f17
4:
    /* 18: _llseek to 4 GiB + 4096 either fails (this filesystem) or returns
       exactly 4 GiB + 4096; a dropped high word would return 4096 */
    mov r0, r8
    mov r1, #1
    mov r2, #4096
    ldr r3, =res
    mov r4, #0
    sys 140
    cmp r0, #0
    blt 1f
    ldr r3, =res
    ldr r1, [r3]
    ldr r2, [r3, #4]
    cmp r1, #4096
    bne f18
    cmp r2, #1
    bne f18
1:
    /* 19: ftruncate64 with a negative 64-bit length (high word 0x80000000)
       fails and leaves the size alone; a dropped high word would resize the
       file to 8192.  (A real 4 GiB length is not tried: QNX zero-fills about
       2 GiB before failing it.) */
    mov r0, r8
    mov r1, #0
    mov r2, #8192
    ldr r3, =0x80000000
    sys 194
    cmp r0, #0
    bge f19
    mov r0, r8
    ldr r1, =statbuf
    sys 197
    want 0, f19
    ldr r1, =statbuf
    ldr r2, [r1, #48]
    ldr r3, [r1, #52]
    ldr r4, =4097
    cmp r2, r4
    bne f19
    cmp r3, #0
    bne f19
    /* 20: sendfile64 from 4 GiB + 4096 moves no bytes and leaves the offset */
    ldr r3, =offset
    mov r1, #4096
    str r1, [r3]
    mov r1, #1
    str r1, [r3, #4]
    mov r0, r9
    mov r1, r8
    mov r2, r3
    mov r3, #16
    sys 239
    cmp r0, #0
    bgt f20
    ldr r3, =offset
    ldr r1, [r3]
    ldr r2, [r3, #4]
    cmp r1, #4096
    bne f20
    cmp r2, #1
    bne f20

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
f20: mov r0, #20
exit_guest:
    mov r7, #1
    svc #0
1:  b 1b
    .size _start, .-_start
    .ltorg

    .section .rodata
path: .asciz "/tmp/linuxemu-large"
path_out: .asciz "/tmp/linuxemu-large-out"
pattern: .ascii "ABCDEFGH"
marker: .byte 0x5a

    .section .data
    .align 3
wiov: .word pattern, 1
riov: .word rbuf, 1

    .section .bss
    .align 3
rbuf: .space 8
res: .space 8
offset: .space 8
statbuf: .space 104
