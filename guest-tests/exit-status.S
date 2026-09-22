    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function
_start:
    mov r0, #37
    mov r7, #1
    svc #0
1:
    b 1b
    .size _start, .-_start
