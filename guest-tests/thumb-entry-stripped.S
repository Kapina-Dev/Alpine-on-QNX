    .syntax unified
    .arch armv7-a
    .thumb
    .section .text
    .global _start
    .thumb_func
    .type _start, %function

/* Phase 4 part 3: the thumb-entry program with an unwind entry, linked and
   then stripped (no mapping symbols). The loader decodes it: e_entry's
   Thumb bit is the evidence, .ARM.exidx the function start. ip names an
   invalid QNX call before each svc, in case one runs unpatched; then the
   udf ends the program with a signal instead of looping. */
_start:
    .fnstart
    .cantunwind
    movs r0, #1
    ldr r1, =message
    movs r2, #message_length
    movs r7, #4              @ write
    movw ip, #0x7fff
    svc #0
    movs r0, #0
    movs r7, #1              @ exit
    movw ip, #0x7fff
    svc #0
    .inst.n 0xde00           @ udf #0 (this assembler has no udf)
    .fnend
    .size _start, .-_start
    .ltorg

    .section .rodata
message:
    .ascii "thumb-entry ok\n"
    .equ message_length, .-message
