    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function
_start:
    movw r7, #999
    svc #0
    cmn r0, #38
    moveq r0, #0
    movne r0, #1
    mov r7, #1
    svc #0
1:
    b 1b
    .size _start, .-_start
