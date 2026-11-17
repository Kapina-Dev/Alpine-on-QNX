    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/*
 * A guest that opens files until it runs out must get EMFILE at the
 * RLIMIT_NOFILE it was told, never a number in the emulator's hidden range
 * (which it could not close), and the emulator's own fds keep working.
 */
_start:
    /* e = eventfd2(0, EFD_NONBLOCK | EFD_CLOEXEC) */
    mov r0, #0
    ldr r1, =0x80800
    movw r7, #356
    svc #0
    cmp r0, #0
    blt fail_setup
    mov r4, r0

    /* r5 = RLIMIT_NOFILE soft limit (prlimit64) */
    mov r0, #0
    mov r1, #7
    mov r2, #0
    ldr r3, =old_limit
    movw r7, #369
    svc #0
    cmp r0, #0
    bne fail_setup
    ldr r3, =old_limit
    ldr r5, [r3]

    /* open("/dev/null") until it fails; every fd must be < limit */
    mov r6, #0
open_loop:
    add r6, r6, #1
    movw r1, #4096
    cmp r6, r1
    bgt fail_bound
    ldr r0, =null_path
    mov r1, #0
    mov r2, #0
    mov r7, #5
    svc #0
    cmp r0, #0
    blt open_failed
    cmp r0, r5
    bge fail_hidden_number
    b open_loop
open_failed:
    cmn r0, #24
    bne fail_not_emfile

    /* the eventfd still works */
    mov r0, r4
    ldr r1, =one
    mov r2, #8
    mov r7, #4
    svc #0
    cmp r0, #8
    bne fail_eventfd

    mov r0, #0
    b exit_guest

fail_setup: mov r0, #170; b exit_guest
fail_hidden_number: mov r0, #171; b exit_guest
fail_bound: mov r0, #172; b exit_guest
fail_not_emfile: mov r0, #173; b exit_guest
fail_eventfd: mov r0, #174
exit_guest:
    mov r7, #1
    svc #0
1:  b 1b
    .size _start, .-_start
    .ltorg

    .section .rodata
    .align 3
one: .word 1, 0
null_path: .asciz "/dev/null"

    .section .bss
    .align 3
old_limit: .space 16
