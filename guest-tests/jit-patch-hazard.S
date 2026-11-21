    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/* Regression test since Phase 4 part 1 (run-linuxemu-smoke expects 0): a
   JIT's data in anonymous memory made executable must not be rewritten.
   Exit 20: a data word changed when the
   RW page flipped RX (hazard present). Exit 21: an mprotect failed in the
   5000-flip loop (the iteration is printed in decimal on stderr).
   Exit 0: neither happened. Exit 1: setup failed. A changed word does not
   stop the run: the loop still executes, so both results are measured.
   Exit 21 wins when both happen; "data-changed" is printed on stderr. */

_start:
    mov r9, #0
    /* RW anonymous page into r4 */
    mov r0, #0
    mov r1, #4096
    mov r2, #3
    mov r3, #0x22
    mvn r4, #0
    mov r5, #0
    mov r7, #192
    svc #0
    cmn r0, #4096
    bhi fail1
    mov r4, r0

    /* literal pool: svc word, mrc TPIDRURO word */
    ldr r2, =0xef000000
    str r2, [r4]
    ldr r2, =0xee1d0f70
    str r2, [r4, #4]
    mov r0, r4
    mov r1, #4096
    mov r2, #5
    mov r7, #125
    svc #0
    cmp r0, #0
    bne fail1
    ldr r2, [r4]
    ldr r3, =0xef000000
    cmp r2, r3
    bne changed
    ldr r2, [r4, #4]
    ldr r3, =0xee1d0f70
    cmp r2, r3
    bne changed
afterchk:

    /* loop of RW->RX flips over one page holding one svc word */
    mov r6, #0
loop:
    mov r0, r4
    mov r1, #4096
    mov r2, #3
    mov r7, #125
    svc #0
    cmp r0, #0
    bne failrw
    ldr r2, =0xef000000
    str r2, [r4]
    mov r0, r4
    mov r1, #4096
    mov r2, #5
    mov r7, #125
    svc #0
    cmp r0, #0
    bne failrw
    add r6, r6, #1
    ldr r2, =5000
    cmp r6, r2
    blt loop
    mov r0, #0
    cmp r9, #0
    movne r0, #20
    b done

changed:
    mov r9, #1
    mov r0, #2
    ldr r1, =msg
    mov r2, #13
    mov r7, #4
    svc #0
    b afterchk
fail1:
    mov r0, #1
    b done
failrw:
    /* print iteration r6 in decimal to stderr */
    ldr r5, =numbuf+15
    mov r2, #10
    strb r2, [r5]
    mov r0, r6
digit:
    mov r3, #0
    mov r8, r0
dloop:
    cmp r8, #10
    blt ddone
    sub r8, r8, #10
    add r3, r3, #1
    b dloop
ddone:
    add r8, r8, #'0'
    sub r5, r5, #1
    strb r8, [r5]
    movs r0, r3
    bne digit
    ldr r1, =numbuf+16
    sub r2, r1, r5
    mov r1, r5
    mov r0, #2
    mov r7, #4
    svc #0
    mov r0, #21
done:
    mov r7, #1
    svc #0

    .ltorg
    .section .data
msg: .ascii "data-changed\n"
    .align 2
numbuf: .space 16
