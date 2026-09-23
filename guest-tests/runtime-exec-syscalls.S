    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

_start:
    mov r7, #20
    svc #0
    mov r6, r0

    mov r0, #0
    mov r1, #0x1000
    mov r2, #3
    mov r3, #0x22
    mvn r4, #0
    mov r5, #0
    mov r7, #192
    svc #0
    cmp r0, #0
    bmi fail_mmap
    mov r4, r0

    ldr r0, =0xe3a07014
    str r0, [r4]
    ldr r0, =0xef000000
    str r0, [r4, #4]
    ldr r0, =0xe12fff1e
    str r0, [r4, #8]

    mov r0, r4
    mov r1, #0x1000
    mov r2, #5
    mov r7, #125
    svc #0
    cmp r0, #0
    bne fail_mprotect

    blx r4
    cmp r0, r6
    bne fail_execute

    mov r0, r4
    mov r1, #0x1000
    mov r7, #91
    svc #0
    cmp r0, #0
    bne fail_munmap

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
fail_munmap:
    mov r0, #64
exit_guest:
    mov r7, #1
    svc #0
1:
    b 1b
    .size _start, .-_start
    .ltorg
