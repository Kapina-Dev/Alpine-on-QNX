    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/*
 * A guest closes every fd below its RLIMIT_NOFILE (the daemon / subprocess
 * close-all loop).  Emulator-owned fds must survive it, and numbers at or
 * above the reported limit are not the guest's: close, dup2 onto them and
 * poll on them fail the way Linux fails numbers above the limit.
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

    /* prlimit64(0, RLIMIT_NOFILE, 0, &old); r5 = old.current */
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

    /* close(n) for 3 <= n < limit, n != e */
    mov r6, #3
close_loop:
    cmp r6, r5
    bge closed
    cmp r6, r4
    beq close_next
    mov r0, r6
    mov r7, #6
    svc #0
close_next:
    add r6, r6, #1
    b close_loop
closed:

    /* the eventfd still works */
    mov r0, r4
    ldr r1, =one
    mov r2, #8
    mov r7, #4
    svc #0
    cmp r0, #8
    bne fail_write
    mov r0, r4
    ldr r1, =buffer
    mov r2, #8
    mov r7, #3
    svc #0
    cmp r0, #8
    bne fail_read

    /* close(limit) -> -EBADF */
    mov r0, r5
    mov r7, #6
    svc #0
    cmn r0, #9
    bne fail_close_limit

    /* dup2(e, limit) -> -EBADF */
    mov r0, r4
    mov r1, r5
    mov r7, #63
    svc #0
    cmn r0, #9
    bne fail_dup2_limit

    /* poll({limit, POLLIN}, 1, 0): POLLNVAL (0x20) in revents */
    ldr r3, =poll_entry
    str r5, [r3]
    mov r0, r3
    mov r1, #1
    mov r2, #0
    mov r7, #168
    svc #0
    ldr r3, =poll_entry
    ldrh r0, [r3, #6]
    tst r0, #0x20
    beq fail_poll_limit

    mov r0, #0
    b exit_guest

fail_setup: mov r0, #150; b exit_guest
fail_write: mov r0, #151; b exit_guest
fail_read: mov r0, #152; b exit_guest
fail_close_limit: mov r0, #153; b exit_guest
fail_dup2_limit: mov r0, #154; b exit_guest
fail_poll_limit: mov r0, #155
exit_guest:
    mov r7, #1
    svc #0
1:  b 1b
    .size _start, .-_start
    .ltorg

    .section .rodata
    .align 3
one: .word 1, 0

    .section .data
    .align 3
poll_entry: .word 0
    .hword 1, 0

    .section .bss
    .align 3
old_limit: .space 16
buffer: .space 8
