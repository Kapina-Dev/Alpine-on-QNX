/*
 * Process syscalls that Android's bionic needs during libc start-up:
 * personality, sched_getscheduler, prctl (PR_SET_NAME, PR_GET_NAME,
 * PR_SET_VMA) and rt_tgsigqueueinfo, plus what execve keeps. Exit status 0
 * means every check passed; any other value names the first failing check.
 */
    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

_start:
    /* argc > 1: this is the re-exec'd child; check what execve kept. */
    ldr r0, [sp]
    ldr r1, [sp, #4]
    ldr r2, =self_path
    str r1, [r2]
    cmp r0, #1
    bgt exec_child

    /* The thread starts named after the exec'd file, truncated to 15. */
    mov r0, #16
    ldr r1, =name_buffer
    mov r7, #172
    svc #0
    cmp r0, #0
    bne fail_19
    ldr r0, =name_buffer
    ldr r1, =initial_name
    bl compare16
    cmp r0, #0
    bne fail_19

    /* personality: query, set PER_LINUX32, query again. */
    mvn r0, #0
    mov r7, #136
    svc #0
    cmp r0, #0
    bne fail_1
    mov r0, #8
    mov r7, #136
    svc #0
    cmp r0, #0
    bne fail_2
    mvn r0, #0
    mov r7, #136
    svc #0
    cmp r0, #8
    bne fail_3

    /* sched_getscheduler: self (0), own tid, invalid pid. */
    mov r0, #0
    mov r7, #157
    svc #0
    cmp r0, #0
    bne fail_4
    mov r7, #224
    svc #0
    ldr r1, =own_tid
    str r0, [r1]
    mov r7, #157
    svc #0
    cmp r0, #0
    bne fail_5
    mvn r0, #0
    mov r7, #157
    svc #0
    cmn r0, #22
    bne fail_6

    /* prctl PR_SET_NAME truncates to 15 bytes; PR_GET_NAME returns 16. */
    mov r0, #15
    ldr r1, =long_name
    mov r7, #172
    svc #0
    cmp r0, #0
    bne fail_7
    mov r0, #16
    ldr r1, =name_buffer
    mov r7, #172
    svc #0
    cmp r0, #0
    bne fail_8
    ldr r0, =name_buffer
    ldr r1, =expected_name
    mov r2, #4
compare_name:
    ldr r3, [r0], #4
    ldr r4, [r1], #4
    cmp r3, r4
    bne fail_9
    subs r2, r2, #1
    bne compare_name

    /* PR_SET_VMA is unsupported (EINVAL), as is an unknown option. */
    ldr r0, =0x53564d41
    mov r1, #0
    ldr r2, =name_buffer
    mov r3, #16
    ldr r4, =long_name
    mov r7, #172
    svc #0
    cmn r0, #22
    bne fail_10
    mvn r0, #0x80000000
    mov r7, #172
    svc #0
    cmn r0, #22
    bne fail_11

    /* rt_tgsigqueueinfo to the calling thread carries Linux siginfo. */
    mov r0, #10
    ldr r1, =usr1_action
    mov r2, #0
    mov r3, #8
    mov r7, #174
    svc #0
    cmp r0, #0
    bne fail_12
    mov r7, #20
    svc #0
    ldr r1, =own_pid
    str r0, [r1]
    ldr r1, =queued_info
    str r0, [r1, #12]
    ldr r1, =own_tid
    ldr r1, [r1]
    mov r2, #10
    ldr r3, =queued_info
    movw r7, #363
    svc #0
    cmp r0, #0
    bne fail_13
    ldr r0, =handler_ran
    ldr r0, [r0]
    cmp r0, #1
    bne fail_14
    ldr r0, =seen_code
    ldr r0, [r0]
    cmn r0, #1
    bne fail_14
    ldr r0, =seen_value
    ldr r0, [r0]
    ldr r1, =0x1234
    cmp r0, r1
    bne fail_15
    ldr r0, =seen_pid
    ldr r0, [r0]
    ldr r1, =own_pid
    ldr r1, [r1]
    cmp r0, r1
    bne fail_15

    /* Invalid tgid is EINVAL; signal 0 only checks the target exists;
       a missing thread is ESRCH. */
    mov r0, #0
    ldr r1, =own_tid
    ldr r1, [r1]
    mov r2, #10
    ldr r3, =queued_info
    movw r7, #363
    svc #0
    cmn r0, #22
    bne fail_16
    ldr r0, =own_pid
    ldr r0, [r0]
    ldr r1, =own_tid
    ldr r1, [r1]
    mov r2, #0
    ldr r3, =queued_info
    movw r7, #363
    svc #0
    cmp r0, #0
    bne fail_17
    ldr r0, =own_pid
    ldr r0, [r0]
    ldr r1, =0x7ffffff0
    mov r2, #10
    ldr r3, =queued_info
    movw r7, #363
    svc #0
    cmn r0, #3
    bne fail_18

    /* execve keeps the personality (still 8) and renames the thread after
       the new file. */
    ldr r0, =self_path
    ldr r0, [r0]
    ldr r1, =child_argv
    str r0, [r1]
    ldr r2, =empty_envp
    mov r7, #11
    svc #0
    b fail_20

exec_child:
    mvn r0, #0
    mov r7, #136
    svc #0
    cmp r0, #8
    bne fail_21
    mov r0, #16
    ldr r1, =name_buffer
    mov r7, #172
    svc #0
    cmp r0, #0
    bne fail_22
    ldr r0, =name_buffer
    ldr r1, =initial_name
    bl compare16
    cmp r0, #0
    bne fail_22
    mov r0, #0
    b exit_guest

/* r0 = 0 when the 16 bytes at r0 and r1 match. */
compare16:
    mov r2, #4
1:  ldr r3, [r0], #4
    ldr r12, [r1], #4
    cmp r3, r12
    bne 2f
    subs r2, r2, #1
    bne 1b
    mov r0, #0
    bx lr
2:  mov r0, #1
    bx lr

fail_1: mov r0, #1; b exit_guest
fail_2: mov r0, #2; b exit_guest
fail_3: mov r0, #3; b exit_guest
fail_4: mov r0, #4; b exit_guest
fail_5: mov r0, #5; b exit_guest
fail_6: mov r0, #6; b exit_guest
fail_7: mov r0, #7; b exit_guest
fail_8: mov r0, #8; b exit_guest
fail_9: mov r0, #9; b exit_guest
fail_10: mov r0, #10; b exit_guest
fail_11: mov r0, #11; b exit_guest
fail_12: mov r0, #12; b exit_guest
fail_13: mov r0, #13; b exit_guest
fail_14: mov r0, #14; b exit_guest
fail_15: mov r0, #15; b exit_guest
fail_16: mov r0, #16; b exit_guest
fail_17: mov r0, #17; b exit_guest
fail_18: mov r0, #18; b exit_guest
fail_19: mov r0, #19; b exit_guest
fail_20: mov r0, #20; b exit_guest
fail_21: mov r0, #21; b exit_guest
fail_22: mov r0, #22; b exit_guest
exit_guest:
    mov r7, #1
    svc #0
1:  b 1b
    .size _start, .-_start

usr1_handler:
    ldr r0, [r1, #8]
    ldr r2, =seen_code
    str r0, [r2]
    ldr r0, [r1, #12]
    ldr r2, =seen_pid
    str r0, [r2]
    ldr r0, [r1, #20]
    ldr r2, =seen_value
    str r0, [r2]
    ldr r2, =handler_ran
    mov r0, #1
    str r0, [r2]
    bx lr
    .ltorg

    .section .data
    .align 2
usr1_action: .word usr1_handler, 4, 0, 0, 0
long_name: .asciz "abcdefghijklmnopqrst"
    .align 2
expected_name: .ascii "abcdefghijklmno"
    .byte 0
    .align 2
queued_info:
    .word 10, 0, -1, 0, 0, 0x1234
    .space 104
initial_name: .ascii "android-process"
    .byte 0
    .align 2
self_path: .word 0
child_argv: .word 0, child_argument, 0
child_argument: .asciz "child"
    .align 2
empty_envp: .word 0
own_pid: .word 0
own_tid: .word 0
handler_ran: .word 0
seen_code: .word 0
seen_pid: .word 0
seen_value: .word 0

    .section .bss
    .align 2
name_buffer: .space 16
