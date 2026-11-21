    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/* execve(argv[1], &argv[1], envp): runs another guest through an execve
   (used to check that LINUXEMU_* settings survive it). 70: execve
   returned. */
_start:
    ldr r3, [sp]             @ argc
    add r1, sp, #8           @ &argv[1]
    ldr r0, [r1]             @ argv[1]
    add r2, sp, r3, lsl #2
    add r2, r2, #8           @ envp = &argv[argc + 1]
    mov r7, #11
    svc #0
    mov r0, #70
    mov r7, #1
    svc #0
1:
    b 1b
    .size _start, .-_start
