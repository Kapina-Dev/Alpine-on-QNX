    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/*
 * Reproduce musl's detached-thread __unmapself sequence.  The child removes
 * the mmap-backed stack containing Linuxemu's active QNX signal frame, then
 * exits without touching the stack again.  clear_child_tid tells the parent
 * that native-stack retirement and the deferred physical unmap completed.
 */
_start:
    mov r0, #0
    ldr r1, =0x00040000
    mov r2, #3
    mov r3, #0x22
    mvn r4, #0
    mov r5, #0
    mov r7, #192
    svc #0
    cmp r0, #0
    bmi fail_mmap
    mov r8, r0
    mov r9, r1

    add r1, r8, r9
    sub r1, r1, #16
    ldr r0, =0x003d0f00
    ldr r2, =child_tid
    mov r3, #0
    ldr r4, =child_tid
    mov r7, #120
    svc #0
    cmp r0, #0
    beq child_start
    bmi fail_clone

parent_wait_exit:
    ldr r4, =child_tid
    ldr r2, [r4]
    cmp r2, #0
    beq pass
    mov r0, r4
    mov r1, #128
    mov r3, #0
    mov r4, #0
    mov r5, #0
    mov r7, #240
    svc #0
    cmp r0, #0
    beq parent_wait_exit
    cmn r0, #11
    beq parent_wait_exit
    b fail_wait

child_start:
    mov r0, r8
    mov r1, r9
    mov r7, #91
    svc #0
    cmp r0, #0
    bne child_fail
    mov r0, #0
    mov r7, #1
    svc #0
    b .

child_fail:
    ldr r1, =child_failure
    mov r0, #1
    str r0, [r1]
    mov r0, #0
    mov r7, #1
    svc #0
    b .

pass:
    ldr r0, =child_failure
    ldr r0, [r0]
    cmp r0, #0
    bne fail_child
    mov r0, #0
    b exit_guest

fail_mmap: mov r0, #151; b exit_guest
fail_clone: mov r0, #152; b exit_guest
fail_wait: mov r0, #153; b exit_guest
fail_child: mov r0, #154
exit_guest:
    mov r7, #1
    svc #0
    b .
    .size _start, .-_start
    .ltorg

    .section .data
    .align 2
child_tid:
    .word 0
child_failure:
    .word 0
