/*
 * Drives LINUXEMU_DIAGNOSTICS across fork and execve. Unknown syscall 999
 * returns ENOSYS and is reported: twice before fork, once in the fork
 * child, three times after the parent re-execs itself. Exit 0 on success.
 */
    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

_start:
    ldr r0, [sp]
    ldr r1, [sp, #4]
    ldr r2, =self_path
    str r1, [r2]
    cmp r0, #1
    bgt exec_image

    bl unknown_call
    bl unknown_call

    mov r7, #2              /* fork */
    svc #0
    cmp r0, #0
    blt fail_1
    bne parent
    bl unknown_call         /* fork child */
    mov r0, #0
    b exit_guest

parent:
    ldr r1, =child_status   /* wait4(pid, &status, 0, 0) */
    mov r2, #0
    mov r3, #0
    mov r7, #114
    svc #0
    cmp r0, #0
    ble fail_2
    ldr r0, =child_status
    ldr r0, [r0]
    cmp r0, #0
    bne fail_3

    ldr r0, =self_path      /* execve(argv[0], {argv[0], "child"}, {}) */
    ldr r0, [r0]
    ldr r1, =child_argv
    str r0, [r1]
    ldr r2, =empty_envp
    mov r7, #11
    svc #0
    mov r0, #9
    b exit_guest

exec_image:
    bl unknown_call
    bl unknown_call
    bl unknown_call
    mov r0, #0
    b exit_guest

unknown_call:
    movw r7, #999
    svc #0
    bx lr

fail_1: mov r0, #1; b exit_guest
fail_2: mov r0, #2; b exit_guest
fail_3: mov r0, #3; b exit_guest
exit_guest:
    mov r7, #248            /* exit_group */
    svc #0
1:  b 1b
    .size _start, .-_start
    .ltorg

    .section .data
    .align 2
self_path: .word 0
child_status: .word 0
child_argv: .word 0, child_argument, 0
child_argument: .asciz "child"
    .align 2
empty_envp: .word 0
