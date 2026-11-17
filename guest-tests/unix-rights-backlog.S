    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/*
 * SCM_RIGHTS sent while the peer still holds unread stream data.
 * QNX's AF_UNIX defaults (SO_SNDBUF 7168, SO_RCVBUF 5120) let a sender
 * overfill the receiver; a descriptor-bearing record that no longer fits is
 * dropped with its payload while sendmsg reports success.  This is tmux's
 * identify handshake: ~5.8 KiB of plain frames, then the terminal fds.
 * Linux delivers every byte and the descriptor.
 */
_start:
    /* socketpair(AF_UNIX, SOCK_STREAM, 0, pair_fds) */
    mov r0, #1
    mov r1, #1
    mov r2, #0
    ldr r3, =pair_fds
    movw r7, #288
    svc #0
    cmp r0, #0
    bne fail_socketpair
    ldr r8, =pair_fds
    ldr r4, [r8]
    ldr r5, [r8, #4]

    /* send(sender, prelude, 5893, 0): leave it unread */
    mov r0, r4
    ldr r1, =prelude
    movw r2, #5893
    mov r3, #0
    movw r7, #289
    svc #0
    movw r1, #5893
    cmp r0, r1
    bne fail_prelude

    /* sendmsg(sender, 16 bytes + SCM_RIGHTS(sender fd), 0) */
    ldr r0, =send_control
    str r4, [r0, #12]
    mov r0, r4
    ldr r1, =send_header
    mov r2, #0
    movw r7, #296
    svc #0
    cmp r0, #16
    bne fail_sendmsg

    /* Drain with MSG_DONTWAIT; r9 = bytes, r10 = descriptors, r11 = tries */
    mov r9, #0
    mov r10, #0
    mov r11, #0
drain:
    cmp r11, #8
    beq drained
    add r11, r11, #1
    ldr r3, =receive_header
    mov r0, #16
    str r0, [r3, #20]
    mov r0, #0
    str r0, [r3, #24]
    ldr r3, =receive_control
    str r0, [r3]
    str r0, [r3, #4]
    str r0, [r3, #8]
    mvn r0, #0
    str r0, [r3, #12]
    mov r0, r5
    ldr r1, =receive_header
    mov r2, #0x40
    movw r7, #297
    svc #0
    cmp r0, #0
    ble drained
    add r9, r9, r0
    ldr r3, =receive_header
    ldr r0, [r3, #20]
    cmp r0, #16
    blt drain_next
    ldr r3, =receive_control
    ldr r0, [r3, #4]
    cmp r0, #1
    bne fail_ancillary
    ldr r0, [r3, #8]
    cmp r0, #1
    bne fail_ancillary
    ldr r6, [r3, #12]
    cmp r6, #0
    bmi fail_ancillary
    add r10, r10, #1
    mov r0, r6
    mov r7, #6
    svc #0
drain_next:
    movw r1, #5909
    cmp r9, r1
    bne drain
drained:
    movw r1, #5909
    cmp r9, r1
    bne fail_lost_bytes
    cmp r10, #1
    bne fail_lost_rights

    mov r0, r4
    mov r7, #6
    svc #0
    mov r0, r5
    mov r7, #6
    svc #0
    mov r0, #0
    b exit_guest

fail_socketpair: mov r0, #120; b exit_guest
fail_prelude: mov r0, #121; b exit_guest
fail_sendmsg: mov r0, #122; b exit_guest
fail_ancillary: mov r0, #123; b exit_guest
fail_lost_bytes: mov r0, #124; b exit_guest
fail_lost_rights: mov r0, #125
exit_guest:
    mov r7, #1
    svc #0
1:  b 1b
    .size _start, .-_start
    .ltorg

    .section .rodata
rights_payload: .ascii "rights_payload16"

    .section .data
    .align 2
pair_fds: .space 8
send_iov: .word rights_payload, 16
send_control: .word 16, 1, 1, -1
send_header: .word 0, 0, send_iov, 1, send_control, 16, 0
receive_iov: .word receive_buffer, 8192
receive_header: .word 0, 0, receive_iov, 1, receive_control, 16, 0

    .section .bss
    .align 2
prelude: .space 5896
receive_buffer: .space 8192
receive_control: .space 16
