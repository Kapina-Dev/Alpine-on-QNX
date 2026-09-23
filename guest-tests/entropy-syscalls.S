    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

_start:
    ldr r0, =random_bytes
    mov r1, #32
    mov r2, #0
    movw r7, #384
    svc #0
    cmp r0, #32
    bne fail_blocking

    ldr r0, =random_bytes
    ldm r0, {r1-r4}
    orrs r1, r1, r2
    orrs r1, r1, r3
    orrs r1, r1, r4
    beq fail_zero

    add r0, #32
    mov r1, #32
    mov r2, #1
    movw r7, #384
    svc #0
    cmp r0, #32
    bne fail_nonblocking

    ldr r0, =random_bytes
    mov r1, #1
    mov r2, #4
    movw r7, #384
    svc #0
    cmn r0, #22
    bne fail_flags

    mov r0, #0
    b exit_guest
fail_blocking:
    mov r0, #121
    b exit_guest
fail_zero:
    mov r0, #122
    b exit_guest
fail_nonblocking:
    mov r0, #123
    b exit_guest
fail_flags:
    mov r0, #124
exit_guest:
    mov r7, #1
    svc #0
1:  b 1b
    .size _start, .-_start
    .ltorg

    .section .bss
    .align 2
random_bytes:
    .space 64
