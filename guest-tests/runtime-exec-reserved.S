    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/* A JIT's reservation: anonymous memory mapped PROT_NONE, committed
   straight to read-execute with mprotect, then written and flipped again.
   61 mmap, 62 the PROT_NONE -> RX mprotect failed, 63 RW mprotect failed,
   64 second RX mprotect failed, 65 the code did not return 42. */
_start:
    mov r0, #0
    mov r1, #0x2000
    mov r2, #0               @ PROT_NONE
    mov r3, #0x22
    mvn r4, #0
    mov r5, #0
    mov r7, #192
    svc #0
    cmn r0, #4096
    bhi fail_mmap
    mov r8, r0

    mov r0, r8
    mov r1, #0x2000
    mov r2, #5
    mov r7, #125
    svc #0
    cmp r0, #0
    bne fail_rx1

    mov r0, r8
    mov r1, #0x2000
    mov r2, #3
    mov r7, #125
    svc #0
    cmp r0, #0
    bne fail_rw

    ldr r0, =0xe3a0002a      @ mov r0, #42
    str r0, [r8]
    ldr r0, =0xe12fff1e      @ bx lr
    str r0, [r8, #4]

    mov r0, r8
    mov r1, #0x2000
    mov r2, #5
    mov r7, #125
    svc #0
    cmp r0, #0
    bne fail_rx2

    blx r8
    cmp r0, #42
    bne fail_run

    mov r0, #0
    b exit_guest
fail_mmap:
    mov r0, #61
    b exit_guest
fail_rx1:
    mov r0, #62
    b exit_guest
fail_rw:
    mov r0, #63
    b exit_guest
fail_rx2:
    mov r0, #64
    b exit_guest
fail_run:
    mov r0, #65
exit_guest:
    mov r7, #1
    svc #0
1:
    b 1b
    .size _start, .-_start
    .ltorg
