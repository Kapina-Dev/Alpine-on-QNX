    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

_start:
    ldr r0, =0x11111000
    movw r7, #5
    movt r7, #15
    svc #0
    cmp r0, #0
    bne fail_main_tls_set
    mrc p15, 0, r0, c13, c0, 3
    ldr r1, =0x11111000
    cmp r0, r1
    bne fail_main_tls_read

    ldr r4, =futex_word
    mov r0, #1
    str r0, [r4]
    mov r0, r4
    mov r1, #128
    mov r2, #0
    mov r3, #0
    mov r4, #0
    mov r5, #0
    mov r7, #240
    svc #0
    cmn r0, #11
    bne fail_eagain

    ldr r4, =futex_word
    mov r0, #0
    str r0, [r4]
    mov r0, r4
    mov r1, #128
    mov r2, #0
    ldr r3, =short_timeout
    mov r4, #0
    mov r5, #0
    mov r7, #240
    svc #0
    cmn r0, #110
    bne fail_timeout

    ldr r0, =0x013d0f00
    ldr r1, =child_stack_end
    ldr r2, =parent_tid
    ldr r3, =0x22222000
    ldr r4, =child_tid
    mov r7, #120
    svc #0
    cmp r0, #0
    beq child_start
    bmi fail_clone
    ldr r1, =parent_tid
    ldr r1, [r1]
    cmp r0, r1
    bne fail_parent_tid

parent_wait_ready:
    ldr r4, =futex_word
    ldr r2, [r4]
    cmp r2, #1
    beq parent_release_child
    mov r0, r4
    mov r1, #128
    mov r2, #0
    mov r3, #0
    mov r4, #0
    mov r5, #0
    mov r7, #240
    svc #0
    cmp r0, #0
    beq parent_wait_ready
    cmn r0, #11
    beq parent_wait_ready
    b fail_ready_wait

parent_release_child:
    ldr r4, =go_word
    mov r0, #1
    str r0, [r4]
    dmb ish
    mov r0, r4
    mov r1, #129
    mov r2, #1
    mov r3, #0
    mov r4, #0
    mov r5, #0
    mov r7, #240
    svc #0

parent_wait_exit:
    ldr r4, =child_tid
    ldr r2, [r4]
    cmp r2, #0
    beq parent_verify
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
    b fail_exit_wait

parent_verify:
    ldr r0, =child_failure
    ldr r0, [r0]
    cmp r0, #0
    bne fail_child
    mrc p15, 0, r0, c13, c0, 3
    ldr r1, =0x11111000
    cmp r0, r1
    bne fail_main_tls_after
    mov r0, #0
    b exit_guest

child_start:
    mrc p15, 0, r0, c13, c0, 3
    ldr r1, =0x22222000
    cmp r0, r1
    bne child_fail_tls
    mov r7, #224
    svc #0
    ldr r1, =child_tid
    ldr r2, [r1]
    cmp r0, r2
    bne child_fail_tid
    ldr r1, =parent_tid
    ldr r1, [r1]
    cmp r0, r1
    bne child_fail_tid

    ldr r4, =futex_word
    mov r0, #1
    str r0, [r4]
    dmb ish
    mov r0, r4
    mov r1, #129
    mov r2, #1
    mov r3, #0
    mov r4, #0
    mov r5, #0
    mov r7, #240
    svc #0

child_wait_go:
    ldr r4, =go_word
    ldr r2, [r4]
    cmp r2, #1
    beq child_exit
    mov r0, r4
    mov r1, #128
    mov r2, #0
    mov r3, #0
    mov r4, #0
    mov r5, #0
    mov r7, #240
    svc #0
    cmp r0, #0
    beq child_wait_go
    cmn r0, #11
    beq child_wait_go
    mov r0, #3
    b child_record_failure
child_fail_tls:
    mov r0, #1
    b child_record_failure
child_fail_tid:
    mov r0, #2
child_record_failure:
    ldr r1, =child_failure
    str r0, [r1]
child_exit:
    mov r0, #0
    mov r7, #1
    svc #0
    b .

fail_main_tls_set: mov r0, #131; b exit_guest
fail_main_tls_read: mov r0, #132; b exit_guest
fail_eagain: mov r0, #133; b exit_guest
fail_timeout: mov r0, #134; b exit_guest
fail_clone: mov r0, #135; b exit_guest
fail_parent_tid: mov r0, #136; b exit_guest
fail_ready_wait: mov r0, #137; b exit_guest
fail_exit_wait: mov r0, #138; b exit_guest
fail_child: mov r0, #139; b exit_guest
fail_main_tls_after: mov r0, #140
exit_guest:
    mov r7, #1
    svc #0
    b .
    .size _start, .-_start
    .ltorg

    .section .data
    .align 3
short_timeout:
    .word 0, 10000000
futex_word:
    .word 0
go_word:
    .word 0
parent_tid:
    .word 0
child_tid:
    .word 0
child_failure:
    .word 0

    .section .bss
    .align 4
child_stack:
    .space 262144
child_stack_end:
