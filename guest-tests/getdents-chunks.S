    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/*
 * getdents64 resumes correctly across calls and leaves no emulator fd
 * behind: reading /etc in 64-byte chunks yields the same byte total as one
 * big read on a separate fd, and after the reads the lowest free number is
 * still the one right after the two directory fds.
 */
_start:
    /* a = openat(AT_FDCWD, "/etc", O_RDONLY | O_DIRECTORY) */
    mvn r0, #99
    ldr r1, =etc_path
    mov r2, #0x4000
    mov r3, #0
    movw r7, #322
    svc #0
    cmp r0, #0
    blt fail_open
    mov r4, r0
    /* b = second fd on the same directory */
    mvn r0, #99
    ldr r1, =etc_path
    mov r2, #0x4000
    mov r3, #0
    movw r7, #322
    svc #0
    cmp r0, #0
    blt fail_open
    mov r5, r0

    /* r8 = total of one big read on b */
    mov r0, r5
    ldr r1, =big_buffer
    mov r2, #8192
    movw r7, #217
    svc #0
    cmp r0, #0
    ble fail_big
    mov r8, r0

    /* r9 = total of 64-byte chunk reads on a (a 64-byte record may not fit
       a long name: -EINVAL ends early and fails the comparison) */
    mov r9, #0
    mov r10, #0
chunk:
    add r10, r10, #1
    cmp r10, #4096
    beq fail_loop
    mov r0, r4
    ldr r1, =small_buffer
    mov r2, #256
    movw r7, #217
    svc #0
    cmp r0, #0
    blt fail_chunk
    beq chunks_done
    add r9, r9, r0
    b chunk
chunks_done:
    cmp r8, r9
    bne fail_total

    /* the lowest free number must be max(a, b) + 1: no hidden stream fd */
    mov r0, r4
    mov r7, #41
    svc #0
    add r1, r5, #1
    cmp r0, r1
    bne fail_leak

    mov r0, #0
    b exit_guest

fail_open: mov r0, #160; b exit_guest
fail_big: mov r0, #161; b exit_guest
fail_chunk: mov r0, #162; b exit_guest
fail_total: mov r0, #163; b exit_guest
fail_leak: mov r0, #164; b exit_guest
fail_loop: mov r0, #165
exit_guest:
    mov r7, #1
    svc #0
1:  b 1b
    .size _start, .-_start
    .ltorg

    .section .rodata
etc_path: .asciz "/etc"

    .section .bss
    .align 3
big_buffer: .space 8192
small_buffer: .space 256
