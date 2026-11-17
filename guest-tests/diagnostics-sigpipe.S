/* One unknown syscall (reported), then write to a pipe whose read end is
   closed: the kernel ends the process with SIGPIPE (default action), and
   the report must still be written. */
    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function
_start:
    movw r7, #999
    svc #0
    ldr r0, =pipe_fds       /* pipe(fds) */
    mov r7, #42
    svc #0
    cmp r0, #0
    bne fail
    ldr r0, =pipe_fds       /* close(read end) */
    ldr r0, [r0]
    mov r7, #6
    svc #0
    ldr r0, =pipe_fds       /* write(write end, "x", 1) -> SIGPIPE */
    ldr r0, [r0, #4]
    ldr r1, =byte
    mov r2, #1
    mov r7, #4
    svc #0
fail:
    mov r0, #9              /* still alive: fail */
    mov r7, #1
    svc #0
1:  b 1b
    .size _start, .-_start
    .ltorg

    .section .data
    .align 2
pipe_fds: .word 0, 0
byte: .ascii "x"
