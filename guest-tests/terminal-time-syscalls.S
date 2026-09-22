    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

_start:
    mov r0, #0
    ldr r1, =time64
    movw r7, #403
    svc #0
    cmp r0, #0
    bne fail_clock64
    ldr r0, =time64
    ldr r1, [r0]
    cmp r1, #0
    beq fail_clock64
    ldr r1, [r0, #8]
    ldr r2, =1000000000
    cmp r1, r2
    bhs fail_clock64

    mov r0, #1
    ldr r1, =time32
    movw r7, #263
    svc #0
    cmp r0, #0
    bne fail_clock32
    ldr r0, =time32
    ldr r1, [r0]
    cmp r1, #0
    beq fail_clock32

    ldr r0, =timeval32
    mov r1, #0
    mov r7, #78
    svc #0
    cmp r0, #0
    bne fail_gettimeofday
    ldr r0, =timeval32
    ldr r1, [r0]
    cmp r1, #0
    beq fail_gettimeofday

    mov r0, #1
    ldr r1, =resolution64
    movw r7, #406
    svc #0
    cmp r0, #0
    bne fail_resolution

    ldr r0, =short_sleep
    mov r1, #0
    mov r7, #162
    svc #0
    cmp r0, #0
    bne fail_sleep

    mov r0, #1
    mov r1, #0
    ldr r2, =zero_time32
    mov r3, #0
    movw r7, #265
    svc #0
    cmp r0, #0
    bne fail_clock_sleep32

    mov r0, #1
    mov r1, #0
    ldr r2, =zero_time64
    mov r3, #0
    movw r7, #407
    svc #0
    cmp r0, #0
    bne fail_clock_sleep64

    ldr r0, =pipe_fds
    mov r1, #0
    movw r7, #359
    svc #0
    cmp r0, #0
    bne fail_pipe

    ldr r4, =pipe_fds
    ldr r5, [r4]
    ldr r6, [r4, #4]
    ldr r0, =poll_write
    str r6, [r0]
    mov r1, #4
    strh r1, [r0, #4]
    mov r1, #1
    mov r2, #0
    mov r7, #168
    svc #0
    cmp r0, #1
    bne fail_poll_write
    ldr r0, =poll_write
    ldrh r1, [r0, #6]
    tst r1, #4
    beq fail_poll_write

    mov r0, r6
    ldr r1, =one_byte
    mov r2, #1
    mov r7, #4
    svc #0
    cmp r0, #1
    bne fail_write

    ldr r0, =poll_read
    str r5, [r0]
    mov r1, #1
    strh r1, [r0, #4]
    mov r1, #1
    ldr r2, =zero_time32
    mov r3, #0
    mov r4, #8
    movw r7, #336
    svc #0
    cmp r0, #1
    bne fail_ppoll
    ldr r0, =poll_read
    ldrh r1, [r0, #6]
    tst r1, #1
    beq fail_ppoll

    ldr r0, =poll_write
    mov r1, #0
    strh r1, [r0, #6]
    mov r1, #1
    ldr r2, =zero_time64
    mov r3, #0
    mov r4, #8
    movw r7, #414
    svc #0
    cmp r0, #1
    bne fail_ppoll64

    ldr r0, =read_set
    mov r1, #1
    lsl r1, r1, r5
    str r1, [r0]
    add r0, r5, #1
    ldr r1, =read_set
    mov r2, #0
    mov r3, #0
    ldr r4, =zero_time32
    movw r7, #142
    svc #0
    cmp r0, #1
    bne fail_select

    ldr r1, =read_set
    mov r0, #1
    lsl r0, r0, r5
    str r0, [r1]
    ldr r0, =old_select
    add r2, r5, #1
    str r2, [r0]
    mov r7, #82
    svc #0
    cmp r0, #1
    bne fail_old_select

    ldr r0, =write_set
    mov r1, #1
    lsl r1, r1, r6
    str r1, [r0]
    add r0, r6, #1
    mov r1, #0
    ldr r2, =write_set
    mov r3, #0
    ldr r4, =zero_time32
    mov r5, #0
    movw r7, #335
    svc #0
    cmp r0, #1
    bne fail_pselect


    ldr r0, =write_set
    mov r1, #1
    lsl r1, r1, r6
    str r1, [r0]
    add r0, r6, #1
    mov r1, #0
    ldr r2, =write_set
    mov r3, #0
    ldr r4, =zero_time64
    mov r5, #0
    movw r7, #413
    svc #0
    cmp r0, #1
    bne fail_pselect64

    mov r0, #0
    b exit_guest
fail_clock64: mov r0, #71; b exit_guest
fail_clock32: mov r0, #72; b exit_guest
fail_gettimeofday: mov r0, #73; b exit_guest
fail_resolution: mov r0, #74; b exit_guest
fail_sleep: mov r0, #75; b exit_guest
fail_clock_sleep32: mov r0, #82; b exit_guest
fail_clock_sleep64: mov r0, #83; b exit_guest
fail_pipe: mov r0, #76; b exit_guest
fail_poll_write: mov r0, #77; b exit_guest
fail_write: mov r0, #78; b exit_guest
fail_ppoll: mov r0, #79; b exit_guest
fail_ppoll64: mov r0, #84; b exit_guest
fail_select: mov r0, #80; b exit_guest
fail_old_select: mov r0, #85; b exit_guest
fail_pselect: mov r0, #81; b exit_guest
fail_pselect64: mov r0, #86
exit_guest:
    mov r7, #1
    svc #0
1:  b 1b
    .size _start, .-_start
    .ltorg

    .section .data
    .align 3
time64: .space 16
resolution64: .space 16
time32: .space 8
timeval32: .space 8
short_sleep: .word 0, 1000000
zero_time32: .word 0, 0
zero_time64: .word 0, 0, 0, 0
pipe_fds: .space 8
poll_write: .space 8
poll_read: .space 8
read_set: .space 32
write_set: .space 32
old_select: .word 0, read_set, 0, 0, zero_time32
one_byte: .byte 65
