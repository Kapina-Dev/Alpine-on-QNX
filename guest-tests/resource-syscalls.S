    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/*
 * ugetrlimit/setrlimit, getrusage, sysinfo and setitimer/getitimer.  Each
 * failing check exits with its own number (10..23); 9 is a setup failure.
 * A line on stdout records whether ITIMER_VIRTUAL is supported.
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

_start:
    /* 10: ugetrlimit(NOFILE) matches prlimit64's virtual value */
    mov r0, #7
    ldr r1, =r32
    sys 191
    want 0, f10
    mov r0, #0
    mov r1, #7
    mov r2, #0
    ldr r3, =old64
    sys 369
    want 0, f10
    ldr r1, =old64
    ldr r2, [r1]
    ldr r3, =r32
    ldr r3, [r3]
    cmp r2, r3
    bne f10
    ldr r1, =old64
    ldr r2, [r1, #8]
    ldr r3, =r32
    ldr r3, [r3, #4]
    cmp r2, r3
    bne f10

    /* 11: setrlimit(NOFILE, {100, max}) then ugetrlimit gives cur 100 */
    ldr r1, =set32
    mov r2, #100
    str r2, [r1]
    ldr r2, =r32
    ldr r2, [r2, #4]
    str r2, [r1, #4]
    mov r0, #7
    sys 75
    want 0, f11
    mov r0, #7
    ldr r1, =r32
    sys 191
    want 0, f11
    ldr r1, =r32
    ldr r2, [r1]
    cmp r2, #100
    bne f11

    /* 12: RLIMIT_STACK: readable; cur above max is EINVAL; raising max
       is EPERM.  The limit is first lowered to a known pair. */
    mov r0, #3
    ldr r1, =r32
    sys 191
    want 0, f12
    ldr r1, =set32
    ldr r2, =0x800000
    str r2, [r1]
    str r2, [r1, #4]
    mov r0, #3
    sys 75
    cmn r0, #1
    beq stack_skip
    want 0, f12
    mov r0, #3
    ldr r1, =r32
    sys 191
    want 0, f12
    ldr r1, =r32
    ldr r2, [r1]
    ldr r3, =0x800000
    cmp r2, r3
    bne f12
    ldr r2, [r1, #4]
    cmp r2, r3
    bne f12
    ldr r1, =set32
    ldr r2, =0x800001
    str r2, [r1]
    mov r0, #3
    sys 75
    wanterr 22, f12
    ldr r1, =set32
    ldr r2, =0x800000
    str r2, [r1]
    ldr r2, =0x900000
    str r2, [r1, #4]
    mov r0, #3
    sys 75
    wanterr 1, f12
    b stack_done
stack_skip:
    /* host hard limit below 8 MiB: setup skip, not a failure */
    mov r0, #1
    ldr r1, =msg_skip
    mov r2, #msg_skip_len
    sys 4
stack_done:

    /* 21: a Linux resource with no QNX counterpart (RLIMIT_MEMLOCK) is
       -EINVAL and is reported as rlimit-resource */
    mov r0, #8
    ldr r1, =r32
    sys 191
    wanterr 22, f21

    /* 13: an infinite host value reads as 0xffffffff (RLIMIT_CPU) */
    mov r0, #0
    ldr r1, =r32
    sys 191
    want 0, f13
    ldr r1, =r32
    ldr r2, [r1]
    mvn r3, #0
    cmp r2, r3
    bne f13

    /* 14: getrusage after a busy loop */
    ldr r4, =30000000
1:  subs r4, r4, #1
    bne 1b
    mov r0, #0
    ldr r1, =rusage
    sys 77
    want 0, f14
    ldr r1, =rusage
    ldr r2, [r1]
    ldr r3, [r1, #4]
    orr r2, r2, r3
    ldr r3, [r1, #8]
    orr r2, r2, r3
    ldr r3, [r1, #12]
    orrs r2, r2, r3
    beq f14
    mvn r0, #0
    ldr r1, =rusage
    sys 77
    want 0, f14
    mov r0, #1
    ldr r1, =rusage
    sys 77
    want 0, f14
    ldr r1, =rusage
    ldr r2, [r1]
    ldr r3, [r1, #4]
    orrs r2, r2, r3
    beq f14
    mov r0, #5
    ldr r1, =rusage
    sys 77
    wanterr 22, f14

    /* 15: getrusage(0, NULL) = -EFAULT */
    mov r0, #0
    mov r1, #0
    sys 77
    wanterr 14, f15

    /* 16: sysinfo */
    ldr r0, =sysinfo
    sys 116
    want 0, f16
    ldr r1, =sysinfo
    ldr r2, [r1]
    cmp r2, #0
    ble f16
    ldr r2, [r1, #16]
    ldr r3, [r1, #52]
    umull r4, r5, r2, r3
    cmp r5, #0
    bne 2f
    ldr r6, =0x4000000
    cmp r4, r6
    bls f16
2:  ldr r2, [r1, #20]
    ldr r3, [r1, #16]
    cmp r2, r3
    bhi f16
    ldrh r2, [r1, #40]
    cmp r2, #1
    blo f16

    /* 17: sysinfo(NULL) = -EFAULT */
    mov r0, #0
    sys 116
    wanterr 14, f17

    /* 18: a never-armed ITIMER_REAL reads as disarmed (QNX errors), then
       ITIMER_REAL delivers SIGALRM to the guest handler */
    mov r0, #0
    ldr r1, =timerbuf
    sys 105
    want 0, f18
    ldr r1, =timerbuf
    ldr r2, [r1, #8]
    ldr r3, [r1, #12]
    orrs r2, r2, r3
    bne f18
    mov r0, #14
    ldr r1, =alrm_action
    mov r2, #0
    mov r3, #8
    sys 174
    want 0, f18
    mov r0, #0
    ldr r1, =timer_50ms
    mov r2, #0
    sys 104
    want 0, f18
    mov r0, #0
    ldr r1, =timerbuf
    sys 105
    want 0, f18
    ldr r1, =timerbuf
    ldr r2, [r1, #8]
    ldr r3, [r1, #12]
    orrs r2, r2, r3
    beq f18
    ldr r4, =300000000
3:  ldr r0, =alrm_count
    ldr r0, [r0]
    cmp r0, #0
    bne 4f
    subs r4, r4, #1
    bne 3b
    b f18
4:  mov r0, #0
    ldr r1, =timer_off
    mov r2, #0
    sys 104
    want 0, f18

    /* 19: ITIMER_VIRTUAL is consistent: both calls succeed or both -22 */
    mov r0, #1
    ldr r1, =timer_1s
    mov r2, #0
    sys 104
    mov r5, r0
    mov r0, #1
    ldr r1, =timerbuf
    sys 105
    cmp r5, #0
    bne 5f
    cmp r0, #0
    bne f19
    mov r0, #1
    ldr r1, =timer_off
    mov r2, #0
    sys 104
    mov r0, #1
    ldr r1, =msg_ok
    mov r2, #msg_ok_len
    b 6f
5:  cmn r5, #22
    bne f19
    cmn r0, #22
    bne f19
    mov r0, #1
    ldr r1, =msg_einval
    mov r2, #msg_einval_len
6:  sys 4
    mov r0, #3
    ldr r1, =timer_1s
    mov r2, #0
    sys 104
    wanterr 22, f19

    /* 20: getitimer(0, NULL) = -EFAULT */
    mov r0, #0
    mov r1, #0
    sys 105
    wanterr 14, f20

    /* 22: 64-bit limits are not truncated.  prlimit64(RLIMIT_DATA,
       {4 GiB, old max}) reads back 4 GiB (a 32-bit rlim_t made it 0), and
       the old pair is restored.  RLIMIT_FSIZE {4 GiB, 4 GiB} either reads
       back exactly or is refused with -EINVAL (QNX refuses every finite
       FSIZE) and stays unchanged; never 0. */
    mov r0, #0
    mov r1, #2
    mov r2, #0
    ldr r3, =old64
    sys 369
    want 0, f22
    ldr r1, =new64
    mov r2, #0
    str r2, [r1]
    mov r2, #1
    str r2, [r1, #4]
    ldr r3, =old64
    ldr r2, [r3, #8]
    str r2, [r1, #8]
    ldr r2, [r3, #12]
    str r2, [r1, #12]
    mov r0, #0
    mov r1, #2
    ldr r2, =new64
    mov r3, #0
    sys 369
    want 0, f22
    mov r0, #0
    mov r1, #2
    mov r2, #0
    ldr r3, =got64
    sys 369
    want 0, f22
    ldr r1, =got64
    ldr r2, [r1]
    cmp r2, #0
    bne f22
    ldr r2, [r1, #4]
    cmp r2, #1
    bne f22
    mov r0, #0
    mov r1, #2
    ldr r2, =old64
    mov r3, #0
    sys 369
    want 0, f22
    mov r0, #0
    mov r1, #1
    mov r2, #0
    ldr r3, =old64
    sys 369
    want 0, f22
    ldr r1, =new64
    mov r2, #0
    str r2, [r1]
    str r2, [r1, #8]
    mov r2, #1
    str r2, [r1, #4]
    str r2, [r1, #12]
    mov r0, #0
    mov r1, #1
    ldr r2, =new64
    mov r3, #0
    sys 369
    mov r6, r0
    mov r0, #0
    mov r1, #1
    mov r2, #0
    ldr r3, =got64
    sys 369
    want 0, f22
    ldr r1, =got64
    cmp r6, #0
    bne 1f
    ldr r2, [r1]
    cmp r2, #0
    bne f22
    ldr r2, [r1, #4]
    cmp r2, #1
    bne f22
    ldr r2, [r1, #8]
    cmp r2, #0
    bne f22
    ldr r2, [r1, #12]
    cmp r2, #1
    bne f22
    b 2f
1:  cmn r6, #22
    bne f22
    ldr r3, =old64
    mov r4, #0
3:  ldr r2, [r1, r4]
    ldr r5, [r3, r4]
    cmp r2, r5
    bne f22
    add r4, r4, #4
    cmp r4, #16
    bne 3b
2:

    /* 23: setrlimit(RLIMIT_CORE, {0x7fffffff, 0x7fffffff}) reads back as
       0x7fffffff, not as unlimited (QNX's 32-bit RLIM_INFINITY) */
    ldr r1, =set32
    ldr r2, =0x7fffffff
    str r2, [r1]
    str r2, [r1, #4]
    mov r0, #4
    sys 75
    want 0, f23
    mov r0, #4
    ldr r1, =r32
    sys 191
    want 0, f23
    ldr r1, =r32
    ldr r2, [r1]
    ldr r3, =0x7fffffff
    cmp r2, r3
    bne f23
    ldr r2, [r1, #4]
    cmp r2, r3
    bne f23

    mov r0, #0
    b exit_guest

alrm_handler:
    ldr r0, =alrm_count
    ldr r1, [r0]
    add r1, r1, #1
    str r1, [r0]
    bx lr

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
f21: mov r0, #21; b exit_guest
f22: mov r0, #22; b exit_guest
f23: mov r0, #23
exit_guest:
    mov r7, #1
    svc #0
1:  b 1b
    .size _start, .-_start
    .ltorg

    .section .rodata
msg_ok: .ascii "itimer-virtual: supported\n"
    .equ msg_ok_len, . - msg_ok
msg_skip: .ascii "stack-rlimit: skipped (host hard limit below 8 MiB)\n"
    .equ msg_skip_len, . - msg_skip
msg_einval: .ascii "itimer-virtual: EINVAL\n"
    .equ msg_einval_len, . - msg_einval

    .section .data
    .align 3
alrm_action: .word alrm_handler, 4, 0, 0, 0
alrm_count: .word 0
timer_50ms: .word 0, 0, 0, 50000
timer_1s: .word 0, 0, 1, 0
timer_off: .word 0, 0, 0, 0

    .section .bss
    .align 3
r32: .space 8
set32: .space 8
old64: .space 16
new64: .space 16
got64: .space 16
rusage: .space 72
sysinfo: .space 64
timerbuf: .space 16
