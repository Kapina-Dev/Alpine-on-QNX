    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/*
 * sendmmsg [374] and recvmmsg [365] over an AF_UNIX datagram socketpair.
 * Each failing check exits with its own number (10..17); 9 is a setup
 * failure.  Check 18 (a signal while recvmmsg blocks) is the C fixture
 * socket-mmsg-interrupt.
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
    .macro chklen vec, index, value, fail
    ldr r1, =\vec
    ldr r0, [r1, #(28 + 32 * \index)]
    cmp r0, #\value
    bne \fail
    .endm
    .macro chkbyte buf, value, fail
    ldr r1, =\buf
    ldrb r0, [r1]
    cmp r0, #\value
    bne \fail
    .endm
    .macro msg name, namelen, iov, iovn, ctl, ctllen
    .word \name, \namelen, \iov, \iovn, \ctl, \ctllen, 0, 0
    .endm

_start:
    /* setup: datagram pair in r4/r5, hidden-range number in r10 */
    mov r0, #1
    mov r1, #2
    mov r2, #0
    ldr r3, =pair_fds
    sys 288
    cmp r0, #0
    bne f9
    ldr r8, =pair_fds
    ldr r4, [r8]
    ldr r5, [r8, #4]
    mov r0, #0
    mov r1, #7
    mov r2, #0
    ldr r3, =old_limit
    sys 369
    want 0, f9
    ldr r3, =old_limit
    ldr r10, [r3]

    /* 10: sendmmsg of 3 returns 3 with each msg_len set */
    mov r0, r4
    ldr r1, =svec
    mov r2, #3
    mov r3, #0
    sys 374
    want 3, f10
    chklen svec, 0, 4, f10
    chklen svec, 1, 6, f10
    chklen svec, 2, 2, f10

    /* 11: recvmmsg of 3 returns 3, payloads in order */
    mov r0, r5
    ldr r1, =rvec
    mov r2, #3
    mov r3, #0
    mov r4, #0
    sys 365
    ldr r4, [r8]
    want 3, f11
    chklen rvec, 0, 4, f11
    chklen rvec, 1, 6, f11
    chklen rvec, 2, 2, f11
    chkbyte rbuf0, 'A', f11
    chkbyte rbuf1, 'B', f11
    chkbyte rbuf2, 'C', f11

    /* 12: MSG_DONTWAIT with one queued returns 1 */
    mov r0, r4
    ldr r1, =svec
    mov r2, #1
    mov r3, #0
    sys 374
    want 1, f12
    mov r0, r5
    ldr r1, =rvec
    mov r2, #3
    mov r3, #0x40
    mov r4, #0
    sys 365
    ldr r4, [r8]
    want 1, f12

    /* 13: MSG_WAITFORONE with 2 queued returns 2 without blocking */
    mov r0, r4
    ldr r1, =svec
    mov r2, #2
    mov r3, #0
    sys 374
    want 2, f13
    mov r0, r5
    ldr r1, =rvec
    mov r2, #3
    ldr r3, =0x10000
    mov r4, #0
    sys 365
    ldr r4, [r8]
    want 2, f13

    /* 14: first message bad -> the error; a later one bad -> the count */
    mov r0, r4
    ldr r1, =bad_first
    mov r2, #2
    mov r3, #0
    sys 374
    wanterr 97, f14
    mov r0, r4
    ldr r1, =bad_second
    mov r2, #2
    mov r3, #0
    sys 374
    want 1, f14
    mov r0, r5
    ldr r1, =rvec
    mov r2, #3
    mov r3, #0x40
    mov r4, #0
    sys 365
    ldr r4, [r8]
    want 1, f14

    /* 15: SCM_RIGHTS in message 2 arrives as a working fd in slot 2 */
    ldr r0, =pipe_fds
    sys 42
    want 0, f15
    ldr r0, =pipe_fds
    ldr r0, [r0]
    ldr r1, =send_cmsg
    str r0, [r1, #12]
    mov r0, r4
    ldr r1, =rights_vec
    mov r2, #2
    mov r3, #0
    sys 374
    want 2, f15
    mov r0, r5
    ldr r1, =rights_recv
    mov r2, #2
    mov r3, #0
    mov r4, #0
    sys 365
    ldr r4, [r8]
    want 2, f15
    ldr r1, =rights_recv
    ldr r0, [r1, #(32 + 20)]
    cmp r0, #16
    bne f15
    ldr r1, =recv_cmsg
    ldr r0, [r1]
    cmp r0, #16
    bne f15
    ldr r0, [r1, #4]
    cmp r0, #1
    bne f15
    ldr r0, [r1, #8]
    cmp r0, #1
    bne f15
    ldr r6, [r1, #12]
    ldr r0, =pipe_fds
    ldr r0, [r0, #4]
    ldr r1, =pair_fds
    mov r2, #1
    sys 4
    want 1, f15
    mov r0, r6
    ldr r1, =rbuf3
    mov r2, #1
    sys 3
    want 1, f15

    /* 16: vlen 2000 is clamped, no error, at most 1024 */
    mov r0, r4
    ldr r1, =svec
    ldr r2, =2000
    mov r3, #0x40
    sys 374
    sub r1, r0, #1
    ldr r2, =1023
    cmp r1, r2
    bhi f16

    /* 17: a hidden descriptor is EBADF for both calls */
    mov r0, r10
    ldr r1, =svec
    mov r2, #1
    mov r3, #0
    sys 374
    wanterr 9, f17
    mov r0, r10
    ldr r1, =rvec
    mov r2, #1
    mov r3, #0x40
    mov r4, #0
    sys 365
    wanterr 9, f17

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

exit_guest:
    mov r7, #1
    svc #0
1:  b 1b
    .size _start, .-_start
    .ltorg

    .data
    .align 3
buf_a: .ascii "AAAA"
buf_b: .ascii "BBBBBB"
buf_c: .ascii "CC"
    .align 2
iov_a: .word buf_a, 4
iov_b: .word buf_b, 6
iov_c: .word buf_c, 2
riov0: .word rbuf0, 16
riov1: .word rbuf1, 16
riov2: .word rbuf2, 16
badaddr: .hword 99
    .space 14
    .align 3
svec:
    msg 0, 0, iov_a, 1, 0, 0
    msg 0, 0, iov_b, 1, 0, 0
    msg 0, 0, iov_c, 1, 0, 0
    .space 65536
rvec:
    msg 0, 0, riov0, 1, 0, 0
    msg 0, 0, riov1, 1, 0, 0
    msg 0, 0, riov2, 1, 0, 0
bad_first:
    msg badaddr, 16, iov_a, 1, 0, 0
    msg 0, 0, iov_a, 1, 0, 0
bad_second:
    msg 0, 0, iov_a, 1, 0, 0
    msg badaddr, 16, iov_a, 1, 0, 0
rights_vec:
    msg 0, 0, iov_a, 1, 0, 0
    msg 0, 0, iov_b, 1, send_cmsg, 16
rights_recv:
    msg 0, 0, riov0, 1, 0, 0
    msg 0, 0, riov1, 1, recv_cmsg, 32
send_cmsg: .word 16, 1, 1, 0
recv_cmsg: .space 32
rbuf0: .space 16
rbuf1: .space 16
rbuf2: .space 16
rbuf3: .space 16
pair_fds: .space 8
pipe_fds: .space 8
old_limit: .space 16
