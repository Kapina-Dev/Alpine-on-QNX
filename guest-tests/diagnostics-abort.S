/* One unknown syscall (reported), then tgkill(self, SIGABRT) with the
   default action, as abort() does: the process dies by SIGABRT, and the
   report must be written before it does. */
    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function
_start:
    movw r7, #999
    svc #0
    mov r7, #20             /* getpid */
    svc #0
    mov r4, r0
    mov r7, #224            /* gettid */
    svc #0
    mov r1, r0
    mov r0, r4
    mov r2, #6              /* SIGABRT */
    movw r7, #268           /* tgkill */
    svc #0
    mov r0, #9              /* still alive: fail */
    mov r7, #1
    svc #0
1:  b 1b
    .size _start, .-_start
