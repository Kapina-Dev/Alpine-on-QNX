    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

_start:
    ldr r0, =0x55555000
    movw r7, #5
    movt r7, #15
    svc #0
    cmp r0, #0
    bne fail_tls
    mov r8, #0

spawn_thread:
    mov r6, r8
    ldr r1, =thread_stacks
    add r0, r8, #1
    add r1, r1, r0, lsl #17
    ldr r2, =thread_tids
    add r2, r2, r8, lsl #2
    ldr r3, =0x66000000
    add r3, r3, r8, lsl #12
    mov r4, r2
    ldr r0, =0x003d0f00
    mov r7, #120
    svc #0
    cmp r0, #0
    beq worker_start
    bmi fail_clone
    add r8, r8, #1
    cmp r8, #4
    bne spawn_thread

    mov r8, #0
wait_thread:
    ldr r4, =thread_tids
    add r4, r4, r8, lsl #2
wait_one:
    ldr r2, [r4]
    cmp r2, #0
    beq next_thread
    mov r0, r4
    mov r1, #128
    mov r3, #0
    mov r4, #0
    mov r5, #0
    mov r7, #240
    svc #0
    ldr r4, =thread_tids
    add r4, r4, r8, lsl #2
    cmp r0, #0
    beq wait_one
    cmn r0, #11
    beq wait_one
    b fail_wait
next_thread:
    add r8, r8, #1
    cmp r8, #4
    bne wait_thread
    ldr r0, =child_failure
    ldr r0, [r0]
    cmp r0, #0
    bne fail_child
    ldr r0, =shared_counter
    ldr r0, [r0]
    movw r1, #2000
    cmp r0, r1
    bne fail_counter
    mrc p15, 0, r0, c13, c0, 3
    ldr r1, =0x55555000
    cmp r0, r1
    bne fail_tls
    mov r0, #0
    b exit_guest

worker_start:
    mrc p15, 0, r0, c13, c0, 3
    ldr r1, =0x66000000
    add r1, r1, r6, lsl #12
    cmp r0, r1
    bne worker_fail
    mov r10, #0
    movw r11, #500
worker_loop:
    ldr r4, =futex_mutex
lock_retry:
    ldrex r0, [r4]
    cmp r0, #0
    bne lock_wait
    mov r0, #1
    strex r1, r0, [r4]
    cmp r1, #0
    bne lock_retry
    dmb ish
    ldr r1, =shared_counter
    ldr r0, [r1]
    add r0, r0, #1
    str r0, [r1]
    dmb ish
    mov r0, #0
    str r0, [r4]
    mov r0, r4
    mov r1, #129
    mov r2, #1
    mov r3, #0
    mov r4, #0
    mov r5, #0
    mov r7, #240
    svc #0
    add r10, r10, #1
    cmp r10, r11
    bne worker_loop
    mov r0, #0
    mov r7, #1
    svc #0
    b .

lock_wait:
    clrex
    mov r0, r4
    mov r1, #128
    mov r2, #1
    mov r3, #0
    mov r4, #0
    mov r5, #0
    mov r7, #240
    svc #0
    cmp r0, #0
    beq worker_loop
    cmn r0, #11
    beq worker_loop
worker_fail:
    ldr r1, =child_failure
    mov r0, #1
    str r0, [r1]
    mov r0, #0
    mov r7, #1
    svc #0
    b .

fail_tls: mov r0, #151; b exit_guest
fail_clone: mov r0, #152; b exit_guest
fail_wait: mov r0, #153; b exit_guest
fail_child: mov r0, #154; b exit_guest
fail_counter: mov r0, #155
exit_guest:
    mov r7, #1
    svc #0
    b .
    .size _start, .-_start
    .ltorg

    .section .data
    .align 2
thread_tids:
    .word 0, 0, 0, 0
futex_mutex:
    .word 0
shared_counter:
    .word 0
child_failure:
    .word 0

    .section .bss
    .align 4
thread_stacks:
    .space 524288
