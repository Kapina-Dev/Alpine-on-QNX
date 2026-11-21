    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/* Generated code without a system call, in anonymous memory made
   executable: it runs, and data words that look like svc #0 or a TPIDRURO
   read are left alone (Phase 4 part 1). 61 mmap, 62 mprotect, 63 the code
   did not return 42, 65 a data word changed. */
_start:
    mov r0, #0
    mov r1, #0x1000
    mov r2, #3
    mov r3, #0x22
    mvn r4, #0
    mov r5, #0
    mov r7, #192
    svc #0
    cmn r0, #4096
    bhi fail_mmap
    mov r4, r0

    ldr r0, =0xe3a0002a      @ mov r0, #42
    str r0, [r4]
    ldr r0, =0xe12fff1e      @ bx lr
    str r0, [r4, #4]
    ldr r0, =0xef000000      @ data: svc #0
    str r0, [r4, #64]
    ldr r0, =0xee1d0f70      @ data: TPIDRURO read
    str r0, [r4, #68]

    mov r0, r4
    mov r1, #0x1000
    mov r2, #5
    mov r7, #125
    svc #0
    cmp r0, #0
    bne fail_mprotect

    blx r4
    cmp r0, #42
    bne fail_execute

    ldr r0, [r4, #64]
    ldr r1, =0xef000000
    cmp r0, r1
    bne fail_data
    ldr r0, [r4, #68]
    ldr r1, =0xee1d0f70
    cmp r0, r1
    bne fail_data

    mov r0, #0
    b exit_guest
fail_mmap:
    mov r0, #61
    b exit_guest
fail_mprotect:
    mov r0, #62
    b exit_guest
fail_execute:
    mov r0, #63
    b exit_guest
fail_data:
    mov r0, #65
exit_guest:
    mov r7, #1
    svc #0
1:
    b 1b
    .size _start, .-_start
    .ltorg
