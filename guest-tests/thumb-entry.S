    .syntax unified
    .arch armv7-a
    .thumb
    .section .text
    .global _start
    .thumb_func
    .type _start, %function

/* A Thumb entry point (e_entry bit 0 set) starts in Thumb state; its Thumb
   svc instructions are patched by the static loader (Phase 4 part 2). */
_start:
    movs r0, #1
    ldr r1, =message
    movs r2, #message_length
    movs r7, #4              @ write
    svc #0
    movs r0, #0
    movs r7, #1              @ exit
    svc #0
1:
    b 1b
    .size _start, .-_start
    .ltorg

    .section .rodata
message:
    .ascii "thumb-entry ok\n"
    .equ message_length, .-message
