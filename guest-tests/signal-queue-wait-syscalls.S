    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

_start:
    mov r7, #20
    svc #0
    ldr r1, =parent_pid
    str r0, [r1]

    mov r0, #32
    ldr r1, =rt_action
    mov r2, #0
    mov r3, #8
    mov r7, #174
    svc #0
    cmp r0, #0
    bne fail_action

    mov r0, #10
    ldr r1, =usr1_action
    mov r2, #0
    mov r3, #8
    mov r7, #174
    svc #0
    cmp r0, #0
    bne fail_action

    /* Queue three instances of Linux realtime signal 32 while it is blocked. */
    mov r0, #0
    ldr r1, =rt_mask
    mov r2, #0
    mov r3, #8
    mov r7, #175
    svc #0
    cmp r0, #0
    bne fail_rt_queue
    mov r5, #3
queue_rt_loop:
    ldr r0, =parent_pid
    ldr r0, [r0]
    mov r1, #32
    mov r7, #37
    svc #0
    cmp r0, #0
    bne fail_rt_queue
    subs r5, r5, #1
    bne queue_rt_loop
    mov r0, #1
    ldr r1, =rt_mask
    mov r2, #0
    mov r3, #8
    mov r7, #175
    svc #0
    cmp r0, #0
    bne fail_rt_queue
    mov r5, #6
drain_rt_loop:
    mov r7, #20
    svc #0
    subs r5, r5, #1
    bne drain_rt_loop
    ldr r0, =rt_count
    ldr r0, [r0]
    cmp r0, #3
    bne fail_rt_queue
    ldr r0, =rt_bad_info
    ldr r0, [r0]
    cmp r0, #0
    bne fail_rt_info

    /* Keep SIGUSR1 blocked normally; ppoll's temporary empty mask must make
       the child signal interrupt the wait without a transition race. */
    mov r0, #0
    ldr r1, =usr1_mask
    mov r2, #0
    mov r3, #8
    mov r7, #175
    svc #0
    mov r7, #2
    svc #0
    cmp r0, #0
    beq ppoll_child
    bmi fail_ppoll
    mov r0, #0
    mov r1, #0
    ldr r2, =one_second
    ldr r3, =empty_mask
    mov r4, #8
    mov r7, #336
    svc #0
    cmn r0, #4
    bne fail_ppoll_result
    ldr r0, =usr1_count
    ldr r0, [r0]
    cmp r0, #1
    bne fail_ppoll_count
    mov r0, #-1
    ldr r1, =child_status
    mov r2, #0
    mov r3, #0
    mov r7, #114
    svc #0
    cmp r0, #0
    ble fail_ppoll

    /* Repeat through pselect6, which passes the mask and its size indirectly. */
    mov r7, #2
    svc #0
    cmp r0, #0
    beq pselect_child
    bmi fail_pselect
    mov r0, #0
    mov r1, #0
    mov r2, #0
    mov r3, #0
    ldr r4, =one_second
    ldr r5, =pselect_mask_argument
    ldr r7, =335
    svc #0
    cmn r0, #4
    bne fail_pselect_result
    ldr r0, =usr1_count
    ldr r0, [r0]
    cmp r0, #2
    bne fail_pselect_count
    mov r0, #-1
    ldr r1, =child_status
    mov r2, #0
    mov r3, #0
    mov r7, #114
    svc #0
    cmp r0, #0
    ble fail_pselect

    mov r0, #0
    mov r7, #1
    svc #0

ppoll_child:
    ldr r6, =350000
ppoll_delay:
    subs r6, r6, #1
    bne ppoll_delay
    b send_usr1

pselect_child:
    ldr r6, =350000
pselect_delay:
    subs r6, r6, #1
    bne pselect_delay
send_usr1:
    ldr r0, =parent_pid
    ldr r0, [r0]
    mov r1, #10
    mov r7, #37
    svc #0
    mov r0, #0
    mov r7, #1
    svc #0

rt_handler:
    cmp r0, #32
    bne handler_fail
    ldr r2, [r1, #12]
    ldr r3, =parent_pid
    ldr r3, [r3]
    cmp r2, r3
    beq rt_info_ok
    ldr r2, =rt_bad_info
    mov r3, #1
    str r3, [r2]
rt_info_ok:
    ldr r2, =rt_count
    ldr r3, [r2]
    add r3, r3, #1
    str r3, [r2]
    bx lr

usr1_handler:
    cmp r0, #10
    bne handler_fail
    ldr r2, =usr1_count
    ldr r3, [r2]
    add r3, r3, #1
    str r3, [r2]
    bx lr

handler_fail:
    mov r0, #90
    mov r7, #1
    svc #0
fail_action: mov r0, #1; b exit_failure
fail_rt_queue: mov r0, #2; b exit_failure
fail_rt_info: mov r0, #3; b exit_failure
fail_ppoll: mov r0, #4; b exit_failure
fail_pselect: mov r0, #5
    b exit_failure
fail_ppoll_result: mov r0, #6; b exit_failure
fail_ppoll_count: mov r0, #7; b exit_failure
fail_pselect_result: mov r0, #8; b exit_failure
fail_pselect_count: mov r0, #9
exit_failure:
    mov r7, #1
    svc #0

    .section .data
    .align 3
rt_action: .word rt_handler, 4, 0, 0, 0
usr1_action: .word usr1_handler, 4, 0, 0, 0
rt_mask: .word 0x80000000, 0
usr1_mask: .word (1 << 9), 0
empty_mask: .word 0, 0
pselect_mask_argument: .word empty_mask, 8
one_second: .word 1, 0
parent_pid: .word 0
child_status: .word 0
rt_count: .word 0
rt_bad_info: .word 0
usr1_count: .word 0
