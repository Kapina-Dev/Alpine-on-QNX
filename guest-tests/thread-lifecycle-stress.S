    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

_start:
    ldr r0, =0x33333000
    movw r7, #5
    movt r7, #15
    svc #0
    cmp r0, #0
    bne fail_tls
    mov r8, #0
    movw r9, #2000

create_next:
    ldr r6, =0x44000000
    add r6, r6, r8, lsl #4
    ldr r0, =0x003d0f00
    ldr r1, =stress_stack_end
    ldr r2, =stress_tid
    mov r3, r6
    ldr r4, =stress_tid
    mov r7, #120
    svc #0
    cmp r0, #0
    beq stress_child
    bmi fail_clone

wait_for_exit:
    ldr r4, =stress_tid
    ldr r2, [r4]
    cmp r2, #0
    beq child_retired
    mov r0, r4
    mov r1, #128
    mov r3, #0
    mov r4, #0
    mov r5, #0
    mov r7, #240
    svc #0
    cmp r0, #0
    beq wait_for_exit
    cmn r0, #11
    beq wait_for_exit
    b fail_wait

child_retired:
    add r8, r8, #1
    cmp r8, r9
    bne create_next
    ldr r0, =completed_threads
    ldr r0, [r0]
    cmp r0, r9
    bne fail_count
    mrc p15, 0, r0, c13, c0, 3
    ldr r1, =0x33333000
    cmp r0, r1
    bne fail_tls
    mov r0, #0
    b exit_guest

stress_child:
    mrc p15, 0, r0, c13, c0, 3
    cmp r0, r6
    bne child_fail
    mov r7, #224
    svc #0
    ldr r1, =stress_tid
    ldr r1, [r1]
    cmp r0, r1
    bne child_fail
    ldr r1, =completed_threads
1:
    ldrex r2, [r1]
    add r2, r2, #1
    strex r3, r2, [r1]
    cmp r3, #0
    bne 1b
    dmb ish
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

fail_tls: mov r0, #141; b exit_guest
fail_clone: mov r0, #142; b exit_guest
fail_wait: mov r0, #144; b exit_guest
fail_count: mov r0, #145
exit_guest:
    mov r7, #1
    svc #0
    b .
    .size _start, .-_start
    .ltorg

    .section .data
    .align 2
stress_tid:
    .word 0
completed_threads:
    .word 0
child_failure:
    .word 0

    .section .bss
    .align 4
stress_stack:
    .space 262144
stress_stack_end:
