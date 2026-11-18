    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/* rt_sigaction on signals QNX lacks (16, 48-64): query and store only.
   Exit codes name the failing check; 0 is success. */

_start:
    /* 10: query of signal 16 succeeds with handler 0 */
    ldr r1, =old
    mov r2, #0xff
    str r2, [r1]
    mov r0, #16
    mov r2, r1
    mov r1, #0
    mov r3, #8
    mov r7, #174
    svc #0
    cmp r0, #0
    bne fail10
    ldr r1, =old
    ldr r0, [r1]
    cmp r0, #0
    bne fail10

    /* 11: query of every signal 1..64 succeeds */
    mov r4, #1
loop11:
    mov r0, r4
    mov r1, #0
    ldr r2, =old
    mov r3, #8
    mov r7, #174
    svc #0
    cmp r0, #0
    bne fail11
    add r4, r4, #1
    cmp r4, #64
    ble loop11

    /* 12: set SIG_IGN on 50, then query returns 1 */
    mov r0, #50
    ldr r1, =ign_action
    mov r2, #0
    mov r3, #8
    mov r7, #174
    svc #0
    cmp r0, #0
    bne fail12
    mov r0, #50
    mov r1, #0
    ldr r2, =old
    mov r3, #8
    mov r7, #174
    svc #0
    cmp r0, #0
    bne fail12
    ldr r1, =old
    ldr r0, [r1]
    cmp r0, #1
    bne fail12

    /* 13: 9 and 19 cannot be set */
    mov r0, #9
    ldr r1, =ign_action
    mov r2, #0
    mov r3, #8
    mov r7, #174
    svc #0
    cmn r0, #22
    bne fail13
    mov r0, #19
    ldr r1, =ign_action
    mov r2, #0
    mov r3, #8
    mov r7, #174
    svc #0
    cmn r0, #22
    bne fail13

    /* 14: 0 and 65 are invalid */
    mov r0, #0
    ldr r1, =ign_action
    mov r2, #0
    mov r3, #8
    mov r7, #174
    svc #0
    cmn r0, #22
    bne fail14
    mov r0, #65
    ldr r1, =ign_action
    mov r2, #0
    mov r3, #8
    mov r7, #174
    svc #0
    cmn r0, #22
    bne fail14

    /* 15: sending an unmapped signal is EINVAL */
    mov r7, #20
    svc #0
    mov r1, #50
    mov r7, #37
    svc #0
    cmn r0, #22
    bne fail15
    mov r7, #20
    svc #0
    mov r4, r0
    mov r7, #224
    svc #0
    mov r1, r0
    mov r0, r4
    mov r2, #16
    mov r7, #268
    svc #0
    cmn r0, #22
    bne fail15

    /* 16: a mapped signal still runs its handler */
    mov r0, #10
    ldr r1, =usr1_action
    mov r2, #0
    mov r3, #8
    mov r7, #174
    svc #0
    cmp r0, #0
    bne fail16
    mov r7, #20
    svc #0
    mov r1, #10
    mov r7, #37
    svc #0
    cmp r0, #0
    bne fail16
    ldr r1, =usr1_count
    ldr r0, [r1]
    cmp r0, #1
    bne fail16

    mov r0, #0
    b done
fail10: mov r0, #10
    b done
fail11: mov r0, #11
    b done
fail12: mov r0, #12
    b done
fail13: mov r0, #13
    b done
fail14: mov r0, #14
    b done
fail15: mov r0, #15
    b done
fail16: mov r0, #16
done:
    mov r7, #1
    svc #0

usr1_handler:
    ldr r1, =usr1_count
    ldr r0, [r1]
    add r0, r0, #1
    str r0, [r1]
    bx lr

    .section .data
    .align 3
ign_action: .word 1, 4, 0, 0, 0
usr1_action: .word usr1_handler, 4, 0, 0, 0
usr1_count: .word 0
old: .word 0, 0, 0, 0, 0
