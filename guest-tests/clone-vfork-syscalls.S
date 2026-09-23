    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

_start:
    ldr r0, =0x00004111
    ldr r1, =child_stack_end
    mov r2, #0
    mov r3, #0
    mov r4, #0
    mov r7, #120
    svc #0
    cmp r0, #0
    beq child
    bmi fail_clone
    mov r4, r0
    mov r0, r4
    ldr r1, =child_status
    mov r2, #0
    mov r3, #0
    mov r7, #114
    svc #0
    cmp r0, r4
    bne fail_wait
    ldr r1, =child_status
    ldr r1, [r1]
    movw r2, #0x2a00
    cmp r1, r2
    bne fail_status
    mov r0, #0
    b exit_guest
child:
    mov r0, #42
    mov r7, #1
    svc #0
    b .
fail_clone:
    mov r0, #161
    b exit_guest
fail_wait:
    mov r0, #162
    b exit_guest
fail_status:
    mov r0, #163
exit_guest:
    mov r7, #1
    svc #0
    b .
    .size _start, .-_start
    .ltorg

    .section .data
    .align 2
child_status:
    .word 0

    .section .bss
    .align 4
child_stack:
    .space 65536
child_stack_end:
