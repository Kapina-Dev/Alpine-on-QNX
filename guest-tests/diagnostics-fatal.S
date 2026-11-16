/* One unknown syscall (reported), then a fatal null load: the report must
   still be written from the fatal-signal path (Linuxemu exits 125). */
    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function
_start:
    movw r7, #999
    svc #0
    mov r0, #0
    ldr r0, [r0]
    mov r7, #1
    svc #0
1:  b 1b
    .size _start, .-_start
