    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/*
 * getpriority/setpriority, sched_*affinity, sched_setscheduler,
 * sched_getparam, capget/capset, prctl (DUMPABLE, NO_NEW_PRIVS, PDEATHSIG)
 * and membarrier.  Each failing check exits with its own number (10..20);
 * 9 is a setup failure.
 */
    .macro sys nr
    movw r7, #\nr
    svc #0
    .endm
    .macro want value, fail
    cmp r0, #\value
    bne \fail
    .endm
    .macro wanterr value, fail
    cmn r0, #\value
    bne \fail
    .endm
    .macro wantmem addr, value, fail
    ldr r1, =\addr
    ldr r1, [r1]
    ldr r2, =\value
    cmp r1, r2
    bne \fail
    .endm

_start:
    /* 10: getpriority is 20 - nice; setpriority 5 gives 15; -5 is EACCES */
    mov r0, #0
    mov r1, #0
    sys 96
    want 20, f10
    mov r0, #0
    mov r1, #0
    mov r2, #5
    sys 97
    want 0, f10
    mov r0, #0
    mov r1, #0
    sys 96
    want 15, f10
    mov r0, #0
    mov r1, #0
    mvn r2, #4
    sys 97
    wanterr 13, f10

    /* 11: PRIO_PGRP and PRIO_USER on the caller work (15) or are EINVAL.
       A line records which. */
    mov r0, #1
    mov r1, #0
    sys 96
    mov r5, r0
    mov r0, #2
    mov r1, #0
    sys 96
    cmp r5, #15
    bne 1f
    cmp r0, #15
    bne f11
    ldr r1, =msg_prio_ok
    mov r2, #msg_prio_ok_len
    b 2f
1:  cmn r5, #22
    bne f11
    cmn r0, #22
    bne f11
    ldr r1, =msg_prio_einval
    mov r2, #msg_prio_einval_len
2:  mov r0, #1
    sys 4

    /* 12: getaffinity returns 4 with a nonzero mask; size 2 is EINVAL;
       a NULL mask is EFAULT */
    ldr r1, =mask
    mov r2, #0
    str r2, [r1]
    mov r0, #0
    mov r1, #4
    ldr r2, =mask
    sys 242
    want 4, f12
    ldr r1, =mask
    ldr r1, [r1]
    cmp r1, #0
    beq f12
    mov r0, #0
    mov r1, #2
    ldr r2, =mask
    sys 242
    wanterr 22, f12
    mov r0, #0
    mov r1, #4
    mov r2, #0
    sys 242
    wanterr 14, f12

    /* 13: setaffinity to CPU 0 reads back as that one bit; mask 0 is
       EINVAL; pid 999999 is ESRCH */
    ldr r1, =mask
    mov r2, #1
    str r2, [r1]
    mov r0, #0
    mov r1, #4
    ldr r2, =mask
    sys 241
    want 0, f13
    ldr r1, =mask
    mov r2, #0xff
    str r2, [r1]
    mov r0, #0
    mov r1, #4
    ldr r2, =mask
    sys 242
    want 4, f13
    wantmem mask, 1, f13
    ldr r1, =mask
    mov r2, #0
    str r2, [r1]
    mov r0, #0
    mov r1, #4
    ldr r2, =mask
    sys 241
    wanterr 22, f13
    ldr r1, =mask
    mov r2, #1
    str r2, [r1]
    ldr r0, =999999
    mov r1, #4
    ldr r2, =mask
    sys 241
    wanterr 3, f13

    /* 14: sched_setscheduler OTHER ok, FIFO EPERM, policy 7 EINVAL;
       sched_getparam gives priority 0 */
    ldr r1, =param
    mov r2, #0
    str r2, [r1]
    mov r0, #0
    mov r1, #0
    ldr r2, =param
    sys 156
    want 0, f14
    ldr r1, =param
    mov r2, #10
    str r2, [r1]
    mov r0, #0
    mov r1, #1
    ldr r2, =param
    sys 156
    wanterr 1, f14
    ldr r1, =param
    mov r2, #0
    str r2, [r1]
    mov r0, #0
    mov r1, #7
    ldr r2, =param
    sys 156
    wanterr 22, f14
    ldr r1, =param
    mov r2, #99
    str r2, [r1]
    mov r0, #0
    ldr r1, =param
    sys 155
    want 0, f14
    wantmem param, 0, f14

    /* 15: capget v3 gives all-zero sets; a bad version with NULL data is
       EINVAL and the header comes back with the preferred version */
    ldr r1, =capdata
    mvn r2, #0
    str r2, [r1]
    str r2, [r1, #4]
    str r2, [r1, #8]
    str r2, [r1, #12]
    str r2, [r1, #16]
    str r2, [r1, #20]
    ldr r1, =caphdr
    ldr r2, =0x20080522
    str r2, [r1]
    mov r2, #0
    str r2, [r1, #4]
    ldr r0, =caphdr
    ldr r1, =capdata
    sys 184
    want 0, f15
    ldr r1, =capdata
    mov r3, #6
3:  ldr r2, [r1], #4
    cmp r2, #0
    bne f15
    subs r3, r3, #1
    bne 3b
    ldr r1, =caphdr
    ldr r2, =0x12345678
    str r2, [r1]
    ldr r0, =caphdr
    mov r1, #0
    sys 184
    wanterr 22, f15
    wantmem caphdr, 0x20080522, f15

    /* 16: capset with all-zero sets succeeds; effective bit 12 is EPERM */
    ldr r1, =caphdr
    ldr r2, =0x20080522
    str r2, [r1]
    ldr r1, =capdata
    mov r2, #0
    str r2, [r1]
    str r2, [r1, #4]
    str r2, [r1, #8]
    str r2, [r1, #12]
    str r2, [r1, #16]
    str r2, [r1, #20]
    ldr r0, =caphdr
    ldr r1, =capdata
    sys 185
    want 0, f16
    ldr r1, =capdata
    mov r2, #0x1000
    str r2, [r1]
    ldr r0, =caphdr
    ldr r1, =capdata
    sys 185
    wanterr 1, f16

    /* 17: PR_GET_DUMPABLE 1; SET 0 then GET 0; SET 2 EINVAL */
    mov r0, #3
    mov r1, #0
    sys 172
    want 1, f17
    mov r0, #4
    mov r1, #0
    sys 172
    want 0, f17
    mov r0, #3
    mov r1, #0
    sys 172
    want 0, f17
    mov r0, #4
    mov r1, #2
    sys 172
    wanterr 22, f17

    /* 18: PR_SET_NO_NEW_PRIVS 1 then GET 1; SET 0 is EINVAL */
    mov r0, #38
    mov r1, #1
    mov r2, #0
    mov r3, #0
    mov r4, #0
    sys 172
    want 0, f18
    mov r0, #39
    mov r1, #0
    mov r2, #0
    mov r3, #0
    mov r4, #0
    sys 172
    want 1, f18
    mov r0, #38
    mov r1, #0
    mov r2, #0
    mov r3, #0
    mov r4, #0
    sys 172
    wanterr 22, f18

    /* 19: PR_SET_PDEATHSIG SIGTERM reads back 15; signal 65 is EINVAL */
    mov r0, #1
    mov r1, #15
    sys 172
    want 0, f19
    ldr r1, =value
    mov r2, #0
    str r2, [r1]
    mov r0, #2
    ldr r1, =value
    sys 172
    want 0, f19
    wantmem value, 15, f19
    mov r0, #1
    mov r1, #65
    sys 172
    wanterr 22, f19

    /* 20: membarrier QUERY is 0; PRIVATE_EXPEDITED is EINVAL (no fake
       barrier) */
    mov r0, #0
    mov r1, #0
    mov r2, #0
    sys 389
    want 0, f20
    mov r0, #8
    mov r1, #0
    mov r2, #0
    sys 389
    wanterr 22, f20

    mov r0, #0
    b exit_guest

f9:  mov r0, #9;  b exit_guest
f10: mov r0, #10; b exit_guest
f11: mov r0, #11; b exit_guest
f12: mov r0, #12; b exit_guest
f13: mov r0, #13; b exit_guest
f14: mov r0, #14; b exit_guest
f15: mov r0, #15; b exit_guest
f16: mov r0, #16; b exit_guest
f17: mov r0, #17; b exit_guest
f18: mov r0, #18; b exit_guest
f19: mov r0, #19; b exit_guest
f20: mov r0, #20; b exit_guest
exit_guest:
    mov r7, #1
    svc #0
1:  b 1b
    .size _start, .-_start
    .ltorg

    .section .rodata
msg_prio_ok: .ascii "prio-pgrp-user: supported\n"
    .equ msg_prio_ok_len, . - msg_prio_ok
msg_prio_einval: .ascii "prio-pgrp-user: EINVAL\n"
    .equ msg_prio_einval_len, . - msg_prio_einval

    .section .bss
    .align 3
mask: .space 16
param: .space 8
caphdr: .space 8
capdata: .space 24
value: .space 8
