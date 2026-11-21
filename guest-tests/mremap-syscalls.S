    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/*
 * mremap for private anonymous read-write memory.  Each failing check exits
 * with its own number (10..19); 9 is a setup failure.  Run with any argument
 * (and LINUXEMU_PATCH_ANONYMOUS=1), check 18 expects the patched-memory
 * refusal instead of the default behaviour.
 */
    .macro sys nr
    movw r7, #\nr
    svc #0
    .endm
    .macro mapanon len, prot
    mov r0, #0
    movw r1, #\len
    mov r2, #\prot
    mov r3, #0x22
    mvn r4, #0
    mov r5, #0
    sys 192
    .endm

_start:
    ldr r11, [sp]            @ argc: > 1 selects the patched mode of check 18
    /* 10: shrink 3 pages to 1 in place; page 0 keeps its pattern and the
       freed page is available to a hinted mmap again */
    mapanon 12288, 3
    cmn r0, #4096
    bhi f9
    mov r8, r0
    ldr r1, =0x11111111
    str r1, [r8]
    ldr r1, =0x22222222
    movw r2, #4096
    str r1, [r8, r2]
    mov r0, r8
    movw r1, #12288
    movw r2, #4096
    mov r3, #0
    sys 163
    cmp r0, r8
    bne f10
    ldr r1, [r8]
    ldr r2, =0x11111111
    cmp r1, r2
    bne f10
    add r0, r8, #4096
    movw r1, #4096
    mov r2, #3
    mov r3, #0x22
    mvn r4, #0
    mov r5, #0
    sys 192
    add r1, r8, #4096
    cmp r0, r1
    bne f10

    /* 11: grow 1 page to 4 with MAYMOVE; data kept, new pages zero */
    mapanon 4096, 3
    cmn r0, #4096
    bhi f9
    mov r9, r0
    ldr r1, =0x33333333
    str r1, [r9]
    mov r0, r9
    movw r1, #4096
    movw r2, #16384
    mov r3, #1
    sys 163
    cmn r0, #4096
    bhi f11
    mov r10, r0
    ldr r1, [r10]
    ldr r2, =0x33333333
    cmp r1, r2
    bne f11
    mov r4, #1
1:  movw r2, #4096
    mul r3, r4, r2
    ldr r1, [r10, r3]
    cmp r1, #0
    bne f11
    add r4, r4, #1
    cmp r4, #4
    blt 1b

    /* 12: growing without MAYMOVE into an occupied range is ENOMEM */
    mapanon 12288, 3
    cmn r0, #4096
    bhi f9
    mov r8, r0
    movw r1, #4096
    movw r2, #8192
    mov r3, #0
    sys 163
    cmn r0, #12
    bne f12

    /* 13: RX mapping, MREMAP_FIXED and MREMAP_DONTUNMAP are EINVAL */
    mapanon 4096, 3
    cmn r0, #4096
    bhi f9
    mov r6, r0
    movw r1, #4096
    mov r2, #5
    sys 125
    cmp r0, #0
    bne f9
    mov r0, r6
    movw r1, #4096
    movw r2, #8192
    mov r3, #1
    sys 163
    cmn r0, #22
    bne f13
    mov r0, r8
    movw r1, #4096
    movw r2, #4096
    mov r3, #2
    mov r4, #0
    sys 163
    cmn r0, #22
    bne f13
    mov r0, r8
    movw r1, #4096
    movw r2, #4096
    mov r3, #4
    mov r4, #0
    sys 163
    cmn r0, #22
    bne f13

    /* 14: a file-backed private mapping is EINVAL */
    ldr r0, =devzero
    mov r1, #0
    mov r2, #0
    sys 5
    cmn r0, #4096
    bhi f9
    mov r6, r0
    mov r0, #0
    movw r1, #4096
    mov r2, #3
    mov r3, #2
    mov r4, r6
    mov r5, #0
    sys 192
    cmn r0, #4096
    bhi f9
    movw r1, #4096
    movw r2, #8192
    mov r3, #1
    sys 163
    cmn r0, #22
    bne f14

    /* 15: an unaligned old address is EINVAL */
    add r0, r8, #1
    movw r1, #4096
    movw r2, #4096
    mov r3, #0
    sys 163
    cmn r0, #22
    bne f15

    /* 16: guaranteed move: the page after the shrunk mapping is taken */
    mapanon 8192, 3
    cmn r0, #4096
    bhi f9
    mov r8, r0
    ldr r1, =0x44444444
    str r1, [r8]
    movw r1, #8192
    movw r2, #4096
    mov r3, #0
    sys 163
    cmp r0, r8
    bne f16
    add r0, r8, #4096
    movw r1, #4096
    mov r2, #3
    mov r3, #0x22
    mvn r4, #0
    mov r5, #0
    sys 192
    add r1, r8, #4096
    cmp r0, r1
    bne f16
    mov r0, r8
    movw r1, #4096
    movw r2, #8192
    mov r3, #1
    sys 163
    cmn r0, #4096
    bhi f16
    cmp r0, r8
    beq f16
    mov r9, r0
    ldr r1, [r9]
    ldr r2, =0x44444444
    cmp r1, r2
    bne f16
    movw r2, #4096
    ldr r1, [r9, r2]
    cmp r1, #0
    bne f16

    /* 17: in-place grow: the freed page after a shrink is reused */
    mapanon 8192, 3
    cmn r0, #4096
    bhi f9
    mov r8, r0
    movw r1, #8192
    movw r2, #4096
    mov r3, #0
    sys 163
    cmp r0, r8
    bne f17
    mov r0, r8
    movw r1, #4096
    movw r2, #8192
    mov r3, #1
    sys 163
    cmp r0, r8
    bne f17

    /* 18: memory flipped RW -> RX -> RW.  Default: anonymous memory is not
       patched, so it is still plain and may move (19: its svc-like word
       changed).  Patched mode: the patched page is refused (EINVAL). */
    mapanon 4096, 3
    cmn r0, #4096
    bhi f9
    mov r8, r0
    mov r1, #0xef000000
    str r1, [r8]
    ldr r1, =0xe12fff1e
    str r1, [r8, #4]
    mov r0, r8
    movw r1, #4096
    mov r2, #5
    sys 125
    cmp r0, #0
    bne f9
    mov r0, r8
    movw r1, #4096
    mov r2, #3
    sys 125
    cmp r0, #0
    bne f9
    mov r0, r8
    movw r1, #4096
    movw r2, #8192
    mov r3, #1
    sys 163
    cmp r11, #1
    bgt 18f
    cmn r0, #4096
    bhi f18
    ldr r1, [r0]
    ldr r2, =0xef000000
    cmp r1, r2
    bne f19
    b 19f
18: cmn r0, #22
    bne f18
19:
    mov r0, #0
    b done
f9: mov r0, #9
    b done
f10: mov r0, #10
    b done
f11: mov r0, #11
    b done
f12: mov r0, #12
    b done
f13: mov r0, #13
    b done
f14: mov r0, #14
    b done
f15: mov r0, #15
    b done
f16: mov r0, #16
    b done
f17: mov r0, #17
    b done
f18: mov r0, #18
    b done
f19: mov r0, #19
done:
    sys 1

    .ltorg
    .section .rodata
devzero:
    .asciz "/dev/zero"
