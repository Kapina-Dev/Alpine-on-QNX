    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function
_start:
    mov r0, #1
    ldr r1, =message
    mov r2, #message_length
    mov r7, #4
    svc #0

    mov r0, #0
    mov r7, #1
    svc #0
1:
    b 1b
    .size _start, .-_start

    .section .rodata
message:
    .ascii "hello from Linux ARM guest\n"
    .equ message_length, .-message
