    .syntax unified
    .arch armv7-a
    .arm
    .section .text
    .global _start
    .type _start, %function

/* Signal handlers in Thumb and ARM state (Phase 4 part 2).
   SIGUSR1: a Thumb handler with a Thumb sa_restorer, raised from ARM code.
   SIGUSR2: an ARM handler with Linuxemu's own restorer, raised from Thumb
   code, which must get Thumb state back after the handler.
   81 rt_sigaction, 82 the Thumb handler didn't run, 83 the ARM handler
   didn't run, 84 the Thumb caller didn't return 7. */
_start:
    ldr r0, =action
    ldr r1, =thumb_handler + 1
    str r1, [r0]
    ldr r1, =0x04000004      @ SA_RESTORER | SA_SIGINFO
    str r1, [r0, #4]
    ldr r1, =thumb_restorer + 1
    str r1, [r0, #8]
    mov r1, r0
    mov r0, #10              @ SIGUSR1
    mov r2, #0
    mov r3, #8
    mov r7, #174             @ rt_sigaction
    svc #0
    cmp r0, #0
    bne fail_action

    mov r7, #20              @ getpid
    svc #0
    mov r1, #10
    mov r7, #37              @ kill
    svc #0
    ldr r0, =flag
    ldr r0, [r0]
    cmp r0, #1
    bne fail_thumb_handler

    ldr r0, =action
    ldr r1, =arm_handler
    str r1, [r0]
    mov r1, #0
    str r1, [r0, #4]         @ no flags: Linuxemu's restorer
    str r1, [r0, #8]
    mov r1, r0
    mov r0, #12              @ SIGUSR2
    mov r2, #0
    mov r3, #8
    mov r7, #174
    svc #0
    cmp r0, #0
    bne fail_action

    ldr r4, =thumb_raise + 1
    blx r4
    cmp r0, #7
    bne fail_thumb_return
    ldr r0, =flag
    ldr r0, [r0]
    cmp r0, #2
    bne fail_arm_handler

    mov r0, #0
    b exit_guest

/* ARM handler: flag = 2. */
arm_handler:
    ldr r1, =flag
    mov r2, #2
    str r2, [r1]
    bx lr

fail_action:
    mov r0, #81
    b exit_guest
fail_thumb_handler:
    mov r0, #82
    b exit_guest
fail_arm_handler:
    mov r0, #83
    b exit_guest
fail_thumb_return:
    mov r0, #84
exit_guest:
    mov r7, #1
    svc #0
1:
    b 1b
    .size _start, .-_start
    .ltorg

    .thumb
/* Thumb handler: flag = 1. */
thumb_handler:
    ldr r1, =flag
    movs r2, #1
    str r2, [r1]
    bx lr

thumb_restorer:
    movs r7, #173            @ rt_sigreturn
    svc #0

/* Raise SIGUSR2 from Thumb code, then return 7 to the ARM caller. */
thumb_raise:
    push {r4, lr}
    movs r7, #20             @ getpid
    svc #0
    movs r1, #12
    movs r7, #37             @ kill
    svc #0
    movs r0, #7
    pop {r4, pc}
    .ltorg

    .data
    .balign 4
flag:
    .word 0
action:
    .word 0, 0, 0, 0, 0
