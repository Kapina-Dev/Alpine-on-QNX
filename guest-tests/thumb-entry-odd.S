    .syntax unified
    .arch armv7-a
    .thumb
    .section .text
    .global _start
    .thumb_func
    .type _start, %function

/* A Thumb entry point whose .text ends on a halfword (size 2 mod 4, no
   literal pool to pad it): it must load and run (Phase 4 part 2). */
_start:
    movs r0, #1
    movw r1, #:lower16:message
    movt r1, #:upper16:message
    movs r2, #message_length
    movs r7, #4              @ write
    svc #0
    movs r0, #0
    movs r7, #1              @ exit
    svc #0
1:
    b 1b
    .size _start, .-_start

    .section .rodata
message:
    .ascii "thumb-entry-odd ok\n"
    .equ message_length, .-message
