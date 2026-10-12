    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

_start:
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

    mov r0, r4
    ldr r1, =pair_message
    mov r2, #13
    movw r3, #0x4000
    movw r7, #289
    svc #0
    cmp r0, #13
    bne fail_send
    mov r0, r5
    ldr r1, =receive_buffer
    mov r2, #13
    mov r3, #0x100
    movw r7, #291
    svc #0
    cmp r0, #13
    bne fail_recv
    ldr r0, =pair_message
    ldr r1, =receive_buffer
    mov r2, #13
    bl compare_bytes
    cmp r0, #0
    bne fail_recv

    ldr r0, =send_control
    str r4, [r0, #12]
    mov r0, r4
    ldr r1, =send_header
    mov r2, #0
    movw r7, #296
    svc #0
    cmp r0, #12
    bne fail_sendmsg
    mov r0, r5
    ldr r1, =receive_header
    ldr r2, =0x40000000
    movw r7, #297
    svc #0
    cmp r0, #12
    bne fail_recvmsg
    ldr r0, =message_two
    ldr r1, =receive_buffer_two
    mov r2, #12
    bl compare_bytes
    cmp r0, #0
    bne fail_recvmsg
    ldr r3, =receive_control
    ldr r0, [r3]
    cmp r0, #16
    bne fail_ancillary
    ldr r0, [r3, #4]
    cmp r0, #1
    bne fail_ancillary
    ldr r0, [r3, #8]
    cmp r0, #1
    bne fail_ancillary
    ldr r6, [r3, #12]
    cmp r6, #0
    bmi fail_ancillary
    mov r0, r6
    mov r1, #1
    mov r2, #0
    movw r7, #221
    svc #0
    tst r0, #1
    beq fail_ancillary
    mov r0, r6
    mov r7, #6
    svc #0

    mov r0, r4
    mov r1, #1
    movw r7, #293
    svc #0
    cmp r0, #0
    bne fail_shutdown
    mov r0, r4
    mov r7, #6
    svc #0
    mov r0, r5
    mov r7, #6
    svc #0

    /* Linux applications commonly pass the full 110-byte sockaddr_un even
       when the pathname is short.  The QNX structure is smaller, so Linuxemu
       must trim the padding and resolve the pathname inside the guest root. */
    ldr r0, =unix_path
    mov r7, #10
    svc #0

    mov r0, #1
    mov r1, #1
    mov r2, #0
    movw r7, #281
    svc #0
    cmp r0, #0
    bmi fail_unix_socket
    ldr r1, =unix_server_fd
    str r0, [r1]

    mov r1, r0
    ldr r2, =unix_address
    mov r3, #110
    mov r0, r1
    mov r1, r2
    mov r2, r3
    movw r7, #282
    svc #0
    cmp r0, #0
    bne fail_unix_bind

    ldr r0, =unix_server_fd
    ldr r0, [r0]
    mov r1, #1
    movw r7, #284
    svc #0
    cmp r0, #0
    bne fail_unix_listen

    ldr r0, =unix_server_fd
    ldr r0, [r0]
    ldr r1, =unix_returned_address
    ldr r2, =unix_returned_length
    movw r7, #286
    svc #0
    cmp r0, #0
    bne fail_unix_name
    ldr r0, =unix_returned_address
    ldrh r1, [r0]
    cmp r1, #1
    bne fail_unix_name
    add r1, r0, #2
    ldr r0, =unix_path
    mov r2, #25
    bl compare_bytes
    cmp r0, #0
    bne fail_unix_name

    mov r0, #1
    mov r1, #1
    mov r2, #0
    movw r7, #281
    svc #0
    cmp r0, #0
    bmi fail_unix_socket
    ldr r1, =unix_client_fd
    str r0, [r1]
    ldr r1, =unix_address
    mov r2, #110
    movw r7, #283
    svc #0
    cmp r0, #0
    bne fail_unix_connect

    ldr r0, =unix_server_fd
    ldr r0, [r0]
    mov r1, #0
    mov r2, #0
    movw r7, #285
    svc #0
    cmp r0, #0
    bmi fail_unix_accept
    ldr r1, =unix_accepted_fd
    str r0, [r1]

    mov r0, r0
    mov r1, #1
    mov r2, #17
    ldr r3, =unix_peer
    ldr r4, =unix_peer_length
    movw r7, #295
    svc #0
    cmp r0, #0
    bne fail_unix_peer
    ldr r0, =unix_peer
    ldr r0, [r0]
    cmp r0, #0
    ble fail_unix_peer

    ldr r0, =unix_client_fd
    ldr r0, [r0]
    ldr r1, =unix_message
    mov r2, #9
    mov r3, #0
    movw r7, #289
    svc #0
    cmp r0, #9
    bne fail_unix_transfer
    ldr r0, =unix_accepted_fd
    ldr r0, [r0]
    ldr r1, =unix_receive
    mov r2, #9
    mov r3, #0
    movw r7, #291
    svc #0
    cmp r0, #9
    bne fail_unix_transfer
    ldr r0, =unix_message
    ldr r1, =unix_receive
    mov r2, #9
    bl compare_bytes
    cmp r0, #0
    bne fail_unix_transfer

    ldr r4, =unix_server_fd
    ldr r0, [r4]
    mov r7, #6
    svc #0
    ldr r0, =unix_client_fd
    ldr r0, [r0]
    mov r7, #6
    svc #0
    ldr r0, =unix_accepted_fd
    ldr r0, [r0]
    mov r7, #6
    svc #0
    ldr r0, =unix_path
    mov r7, #10
    svc #0
    cmp r0, #0
    bne fail_unix_unlink

    mov r0, #2
    ldr r1, =0x00080801
    mov r2, #0
    movw r7, #281
    svc #0
    cmp r0, #0
    bmi fail_socket_flags
    mov r4, r0
    ldr r1, =flagged_fd
    str r4, [r1]
    mov r1, #3
    mov r2, #0
    movw r7, #221
    svc #0
    tst r0, #0x800
    beq fail_socket_flags
    mov r0, r4
    mov r1, #1
    mov r2, #0
    movw r7, #221
    svc #0
    tst r0, #1
    beq fail_socket_flags

    mov r0, r4
    mov r1, #1
    mov r2, #2
    ldr r3, =one
    mov r4, #4
    movw r7, #294
    svc #0
    cmp r0, #0
    bne fail_sockopt
    ldr r0, =flagged_fd
    ldr r0, [r0]
    mov r7, #6
    svc #0

    mov r0, #2
    mov r1, #2
    mov r2, #0
    movw r7, #281
    svc #0
    cmp r0, #0
    bmi fail_udp
    ldr r1, =inet_fd
    str r0, [r1]
    mov r4, r0
    mov r1, #1
    mov r2, #2
    ldr r3, =one
    mov r4, #4
    movw r7, #294
    svc #0
    cmp r0, #0
    bne fail_sockopt
    ldr r4, =inet_fd
    ldr r4, [r4]
    mov r0, r4
    ldr r1, =loopback_any_port
    mov r2, #16
    movw r7, #282
    svc #0
    cmp r0, #0
    bne fail_bind
    mov r0, r4
    ldr r1, =bound_address
    ldr r2, =address_length
    movw r7, #286
    svc #0
    cmp r0, #0
    bne fail_getsockname
    ldr r0, =bound_address
    ldrh r1, [r0]
    cmp r1, #2
    bne fail_getsockname
    ldrh r1, [r0, #2]
    cmp r1, #0
    beq fail_getsockname
    mov r0, r4
    mov r1, #1
    mov r2, #3
    ldr r3, =socket_type_value
    ldr r4, =option_length
    movw r7, #295
    svc #0
    cmp r0, #0
    bne fail_sockopt
    ldr r0, =socket_type_value
    ldr r0, [r0]
    cmp r0, #2
    bne fail_sockopt

    mov r0, #2
    mov r1, #2
    mov r2, #0
    movw r7, #281
    svc #0
    cmp r0, #0
    bmi fail_udp
    mov r5, r0
    ldr r1, =second_fd
    str r5, [r1]
    mov r0, r5
    ldr r1, =loopback_any_port
    mov r2, #16
    movw r7, #282
    svc #0
    cmp r0, #0
    bne fail_bind
    mov r0, r5
    ldr r1, =second_address
    ldr r2, =second_address_length
    movw r7, #286
    svc #0
    cmp r0, #0
    bne fail_getsockname
    ldr r0, =inet_fd
    ldr r0, [r0]
    ldr r1, =udp_message
    mov r2, #10
    mov r3, #0
    ldr r4, =second_address
    mov r5, #16
    movw r7, #290
    svc #0
    cmp r0, #10
    bne fail_sendto
    mov r0, r5
    ldr r0, =second_fd
    ldr r0, [r0]
    ldr r1, =udp_receive
    mov r2, #10
    mov r3, #0
    ldr r4, =source_address
    ldr r5, =source_address_length
    movw r7, #292
    svc #0
    cmp r0, #10
    bne fail_recvfrom
    ldr r0, =udp_message
    ldr r1, =udp_receive
    mov r2, #10
    bl compare_bytes
    cmp r0, #0
    bne fail_recvfrom

    ldr r0, =inet_fd
    ldr r0, [r0]
    mov r7, #6
    svc #0
    ldr r0, =second_fd
    ldr r0, [r0]
    mov r7, #6
    svc #0

    ldr r0, =legacy_socket_arguments
    mov r1, r0
    mov r0, #1
    mov r7, #102
    svc #0
    cmp r0, #0
    bmi fail_socketcall
    mov r7, #6
    svc #0

    mov r0, #0
    b exit_guest
fail_socketpair: mov r0, #91; b exit_guest
fail_send: mov r0, #92; b exit_guest
fail_recv: mov r0, #93; b exit_guest
fail_sendmsg: mov r0, #94; b exit_guest
fail_recvmsg: mov r0, #95; b exit_guest
fail_shutdown: mov r0, #96; b exit_guest
fail_socket_flags: mov r0, #97; b exit_guest
fail_sockopt: mov r0, #98; b exit_guest
fail_udp: mov r0, #99; b exit_guest
fail_bind: mov r0, #100; b exit_guest
fail_getsockname: mov r0, #101; b exit_guest
fail_sendto: mov r0, #102; b exit_guest
fail_recvfrom: mov r0, #103; b exit_guest
fail_socketcall: mov r0, #104
    b exit_guest
fail_unix_socket: mov r0, #105; b exit_guest
fail_unix_bind: mov r0, #106; b exit_guest
fail_unix_listen: mov r0, #107; b exit_guest
fail_unix_name: mov r0, #108; b exit_guest
fail_unix_connect: mov r0, #109; b exit_guest
fail_unix_accept: mov r0, #110; b exit_guest
fail_unix_transfer: mov r0, #111; b exit_guest
fail_unix_unlink: mov r0, #112; b exit_guest
fail_ancillary: mov r0, #113; b exit_guest
fail_unix_peer: mov r0, #114
exit_guest:
    mov r7, #1
    svc #0
1:  b 1b

compare_bytes:
    cmp r2, #0
    moveq r0, #0
    bxeq lr
2:  ldrb r3, [r0], #1
    ldrb r6, [r1], #1
    cmp r3, r6
    movne r0, #1
    bxne lr
    subs r2, r2, #1
    bne 2b
    mov r0, #0
    bx lr
    .size _start, .-_start
    .ltorg

    .section .rodata
pair_message: .ascii "phase5_socket"
message_two: .ascii "message_path"
udp_message: .ascii "udp_phase5"
unix_message: .ascii "unix_path"
unix_address:
    .hword 1
unix_path:
    .asciz "/tmp/linuxemu-unix-socket"
    .space 82
loopback_any_port:
    .hword 2
    .hword 0
    .byte 127, 0, 0, 1
    .space 8

    .section .data
    .align 2
one: .word 1
pair_fds: .space 8
flagged_fd: .word -1
inet_fd: .word -1
second_fd: .word -1
socket_type_value: .word 0
option_length: .word 4
address_length: .word 16
second_address_length: .word 16
source_address_length: .word 16
unix_server_fd: .word -1
unix_client_fd: .word -1
unix_accepted_fd: .word -1
unix_returned_length: .word 110
unix_peer_length: .word 12
send_iov: .word message_two, 12
receive_iov: .word receive_buffer_two, 12
send_control: .word 16, 1, 1, -1
send_header: .word 0, 0, send_iov, 1, send_control, 16, 0
receive_header: .word 0, 0, receive_iov, 1, receive_control, 16, 0
legacy_socket_arguments: .word 2, 2, 0, 0, 0, 0

    .section .bss
    .align 2
receive_buffer: .space 16
receive_buffer_two: .space 16
udp_receive: .space 16
bound_address: .space 16
second_address: .space 16
source_address: .space 16
unix_returned_address: .space 110
unix_receive: .space 16
receive_control: .space 16
unix_peer: .space 12
