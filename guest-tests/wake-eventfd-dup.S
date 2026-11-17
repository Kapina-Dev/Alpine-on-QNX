    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/*
 * Wake eventfd on the record table: the dup family works, the write end is
 * not a guest-visible low number, the object survives closing the original
 * number, and dup2 over a duplicate turns that number into the new file.
 */
_start:
    /* eventfd2(0, EFD_NONBLOCK | EFD_CLOEXEC) */
    mov r0, #0
    ldr r1, =0x80800
    movw r7, #356
    svc #0
    cmp r0, #0
    blt fail_eventfd
    mov r4, r0

    /* d = dup(e): must work and take the next number (no hidden peer there) */
    mov r0, r4
    mov r7, #41
    svc #0
    cmp r0, #0
    blt fail_dup
    mov r5, r0
    add r1, r4, #1
    cmp r5, r1
    bne fail_visible_peer

    /* write(d, 1) then read(e) */
    mov r0, r5
    ldr r1, =one
    mov r2, #8
    mov r7, #4
    svc #0
    cmp r0, #8
    bne fail_write_dup
    mov r0, r4
    ldr r1, =buffer
    mov r2, #8
    mov r7, #3
    svc #0
    cmp r0, #8
    bne fail_read_original

    /* close(e); the duplicate still works on its own */
    mov r0, r4
    mov r7, #6
    svc #0
    mov r0, r5
    ldr r1, =one
    mov r2, #8
    mov r7, #4
    svc #0
    cmp r0, #8
    bne fail_write_after_close
    mov r0, r5
    ldr r1, =buffer
    mov r2, #8
    mov r7, #3
    svc #0
    cmp r0, #8
    bne fail_read_after_close

    /* pipe2(p, O_NONBLOCK); dup2(p[0], d): d is now a plain pipe */
    ldr r0, =pipe_fds
    mov r1, #0x800
    movw r7, #359
    svc #0
    cmp r0, #0
    bne fail_pipe
    ldr r6, =pipe_fds
    ldr r0, [r6]
    mov r1, r5
    mov r7, #63
    svc #0
    cmp r0, r5
    bne fail_pipe
    ldr r0, [r6, #4]
    ldr r1, =pipe_bytes
    mov r2, #8
    mov r7, #4
    svc #0
    cmp r0, #8
    bne fail_pipe
    mov r0, r5
    ldr r1, =buffer
    mov r2, #8
    mov r7, #3
    svc #0
    cmp r0, #8
    bne fail_replaced_read
    mov r0, r5
    ldr r1, =buffer
    mov r2, #8
    mov r7, #3
    svc #0
    cmn r0, #11
    bne fail_replaced_empty

    mov r0, #0
    b exit_guest

fail_eventfd: mov r0, #140; b exit_guest
fail_visible_peer: mov r0, #141; b exit_guest
fail_dup: mov r0, #142; b exit_guest
fail_write_dup: mov r0, #143; b exit_guest
fail_read_original: mov r0, #144; b exit_guest
fail_write_after_close: mov r0, #145; b exit_guest
fail_read_after_close: mov r0, #146; b exit_guest
fail_replaced_read: mov r0, #147; b exit_guest
fail_replaced_empty: mov r0, #148; b exit_guest
fail_pipe: mov r0, #149
exit_guest:
    mov r7, #1
    svc #0
1:  b 1b
    .size _start, .-_start
    .ltorg

    .section .rodata
    .align 3
one: .word 1, 0
pipe_bytes: .ascii "pipedata"

    .section .bss
    .align 3
buffer: .space 8
pipe_fds: .space 8
