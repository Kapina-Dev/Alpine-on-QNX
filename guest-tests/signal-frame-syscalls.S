    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

_start:
    mov r0, #10
    ldr r1, =usr1_action
    mov r2, #0
    mov r3, #8
    mov r7, #174
    svc #0
    cmp r0, #0
    bne fail_action

    ldr r4, =0x44556677
    mov r7, #20
    svc #0
    mov r1, #10
    mov r7, #37
    svc #0
    cmp r0, #0
    bne fail_kill
    ldr r0, =0x44556677
    cmp r4, r0
    bne fail_restore
    ldr r0, =usr1_count
    ldr r0, [r0]
    cmp r0, #1
    bne fail_usr1_count
    mov r0, #2
    mov r1, #0
    ldr r2, =outside_mask
    mov r3, #8
    mov r7, #175
    svc #0
    ldr r0, =outside_mask
    ldr r0, [r0]
    tst r0, #(1 << 9)
    bne fail_mask_restore

    ldr r0, =altstack_request
    mov r1, #0
    mov r7, #186
    svc #0
    cmp r0, #0
    bne fail_altstack
    mov r0, #12
    ldr r1, =usr2_action
    mov r2, #0
    mov r3, #8
    mov r7, #174
    svc #0
    mov r7, #20
    svc #0
    mov r1, #12
    mov r7, #37
    svc #0
    ldr r0, =usr2_count
    ldr r0, [r0]
    cmp r0, #1
    bne fail_plain

    mov r0, #32
    ldr r1, =rt_action
    mov r2, #0
    mov r3, #8
    mov r7, #174
    svc #0
    mov r7, #224
    svc #0
    mov r1, #32
    mov r7, #238
    svc #0
    cmp r0, #0
    bne fail_tkill
    ldr r0, =rt_count
    ldr r0, [r0]
    cmp r0, #1
    bne fail_rt_count

    mov r0, #4
    ldr r1, =ill_action
    mov r2, #0
    mov r3, #8
    mov r7, #174
    svc #0
    .word 0xffffffff
after_ill:
    ldr r0, =ill_count
    ldr r0, [r0]
    cmp r0, #1
    bne fail_ill

    mov r0, #20
    mov r7, #20
    svc #0
    ldr r1, =parent_pid
    str r0, [r1]
    mov r0, #0
    ldr r1, =usr1_mask
    mov r2, #0
    mov r3, #8
    mov r7, #175
    svc #0
    mov r7, #2
    svc #0
    cmp r0, #0
    beq interrupt_child
    bmi fail_interrupt
    ldr r0, =empty_mask
    mov r1, #8
    mov r7, #179
    svc #0
    cmn r0, #4
    bne fail_interrupt
    ldr r0, =usr1_count
    ldr r0, [r0]
    cmp r0, #2
    bne fail_interrupt
    mov r0, #-1
    ldr r1, =child_status
    mov r2, #0
    mov r3, #0
    mov r7, #114
    svc #0
    cmp r0, #0
    ble fail_interrupt

    mov r0, #1
    ldr r1, =usr1_mask
    mov r2, #0
    mov r3, #8
    mov r7, #175
    svc #0
    ldr r0, =usr1_action
    ldr r1, =0x10000004
    str r1, [r0, #4]
    mov r0, #10
    ldr r1, =usr1_action
    mov r2, #0
    mov r3, #8
    mov r7, #174
    svc #0
    ldr r0, =restart_pipe
    mov r7, #42
    svc #0
    cmp r0, #0
    bne fail_restart
    mov r7, #2
    svc #0
    cmp r0, #0
    beq restart_child
    bmi fail_restart
    ldr r0, =restart_pipe
    ldr r0, [r0]
    ldr r1, =restart_byte
    mov r2, #1
    mov r7, #3
    svc #0
    cmp r0, #1
    bne fail_restart
    ldr r0, =usr1_count
    ldr r0, [r0]
    cmp r0, #3
    bne fail_restart
    mov r0, #-1
    ldr r1, =child_status
    mov r2, #0
    mov r3, #0
    mov r7, #114
    svc #0
    cmp r0, #0
    ble fail_restart

    mov r0, #0
    mov r7, #1
    svc #0

usr1_handler:
    cmp r0, #10
    bne handler_fail
    ldr r3, [r1]
    cmp r3, #10
    bne handler_fail
    mov r0, #2
    mov r1, #0
    ldr r2, =inside_mask
    mov r3, #8
    mov r7, #175
    svc #0
    ldr r0, =inside_mask
    ldr r0, [r0]
    tst r0, #(1 << 9)
    beq handler_fail
    ldr r0, =usr1_count
    ldr r1, [r0]
    add r1, r1, #1
    str r1, [r0]
    mov r4, #0
    bx lr

interrupt_child:
    ldr r0, =200000
interrupt_delay:
    subs r0, r0, #1
    bne interrupt_delay
    ldr r0, =parent_pid
    ldr r0, [r0]
    mov r1, #10
    mov r7, #37
    svc #0
    mov r0, #0
    mov r7, #1
    svc #0

restart_child:
    ldr r0, =200000
restart_signal_delay:
    subs r0, r0, #1
    bne restart_signal_delay
    ldr r0, =parent_pid
    ldr r0, [r0]
    mov r1, #10
    mov r7, #37
    svc #0
    ldr r0, =400000
restart_write_delay:
    subs r0, r0, #1
    bne restart_write_delay
    ldr r0, =restart_pipe
    ldr r0, [r0, #4]
    ldr r1, =restart_value
    mov r2, #1
    mov r7, #4
    svc #0
    mov r0, #0
    mov r7, #1
    svc #0

usr2_handler:
    cmp r0, #12
    bne handler_fail
    ldr r0, =altstack_memory
    cmp sp, r0
    blo handler_fail
    add r0, r0, #8192
    cmp sp, r0
    bhs handler_fail
    ldr r0, =usr2_count
    mov r1, #1
    str r1, [r0]
    bx lr

rt_handler:
    cmp r0, #32
    bne handler_fail
    ldr r0, =rt_count
    mov r1, #1
    str r1, [r0]
    bx lr

ill_handler:
    cmp r0, #4
    bne handler_fail
    ldr r0, [r2, #92]
    add r0, r0, #4
    str r0, [r2, #92]
    ldr r0, =ill_count
    mov r1, #1
    str r1, [r0]
    bx lr

handler_fail:
    mov r0, #90
    mov r7, #1
    svc #0
fail_action: mov r0, #1; b exit_failure
fail_kill: mov r0, #2; b exit_failure
fail_restore: mov r0, #3; b exit_failure
fail_usr1_count: mov r0, #4; b exit_failure
fail_mask_restore: mov r0, #5; b exit_failure
fail_plain: mov r0, #6; b exit_failure
fail_tkill: mov r0, #7; b exit_failure
fail_rt_count: mov r0, #8; b exit_failure
fail_ill: mov r0, #9; b exit_failure
fail_altstack: mov r0, #10; b exit_failure
fail_interrupt: mov r0, #11; b exit_failure
fail_restart: mov r0, #12
exit_failure:
    mov r7, #1
    svc #0

    .section .data
    .align 3
usr1_action: .word usr1_handler, 4, 0, 0, 0
usr2_action: .word usr2_handler, 0x08000000, 0, 0, 0
rt_action: .word rt_handler, 4, 0, 0, 0
ill_action: .word ill_handler, 4, 0, 0, 0
usr1_count: .word 0
usr2_count: .word 0
rt_count: .word 0
ill_count: .word 0
inside_mask: .word 0, 0
outside_mask: .word 0, 0
usr1_mask: .word (1 << 9), 0
empty_mask: .word 0, 0
parent_pid: .word 0
child_status: .word 0
restart_pipe: .word -1, -1
restart_byte: .byte 0
restart_value: .byte 0x5a
altstack_request: .word altstack_memory, 0, 8192
    .align 3
altstack_memory: .space 8192
