    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

_start:
    mov r0, #2
    ldr r1, =0x00080801
    mov r2, #0
    movw r7, #281
    svc #0
    cmp r0, #0
    bmi fail_socket
    mov r4, r0
    ldr r1, =write_poll
    str r4, [r1]

    mov r0, r4
    ldr r1, =server_address
    mov r2, #16
    movw r7, #283
    svc #0
    cmp r0, #0
    beq connected
    cmn r0, #115
    bne fail_connect
    ldr r0, =write_poll
    mov r1, #4
    strh r1, [r0, #4]
    mov r1, #1
    movw r2, #3000
    mov r7, #168
    svc #0
    cmp r0, #1
    bne fail_poll
    ldr r0, =write_poll
    ldrh r1, [r0, #6]
    tst r1, #4
    beq fail_poll

connected:
    mov r0, r4
    mov r1, #1
    mov r2, #4
    ldr r3, =socket_error
    ldr r4, =option_length
    movw r7, #295
    svc #0
    cmp r0, #0
    bne fail_so_error
    ldr r0, =socket_error
    ldr r0, [r0]
    cmp r0, #0
    bne fail_so_error
    ldr r4, =write_poll
    ldr r4, [r4]

    mov r0, r4
    ldr r1, =peer_address
    ldr r2, =peer_length
    movw r7, #287
    svc #0
    cmp r0, #0
    bne fail_peer
    ldr r0, =peer_address
    ldrh r0, [r0]
    cmp r0, #2
    bne fail_peer

    mov r0, r4
    ldr r1, =request
    mov r2, #request_length
    movw r3, #0x4000
    movw r7, #289
    svc #0
    cmp r0, #request_length
    bne fail_send
    ldr r0, =write_poll
    mov r1, #1
    strh r1, [r0, #4]
    mov r1, #0
    strh r1, [r0, #6]
    mov r1, #1
    movw r2, #3000
    mov r7, #168
    svc #0
    cmp r0, #1
    bne fail_read_poll
    ldr r0, =write_poll
    ldrh r1, [r0, #6]
    tst r1, #1
    beq fail_read_poll
    mov r0, r4
    ldr r1, =response
    mov r2, #128
    mov r3, #0
    movw r7, #291
    svc #0
    cmp r0, #0
    ble fail_recv
    mov r0, r4
    mov r7, #6
    svc #0
    mov r0, #0
    b exit_guest

fail_socket: mov r0, #111; b exit_guest
fail_connect: mov r0, #112; b exit_guest
fail_poll: mov r0, #113; b exit_guest
fail_so_error: mov r0, #114; b exit_guest
fail_peer: mov r0, #115; b exit_guest
fail_send: mov r0, #116; b exit_guest
fail_recv: mov r0, #117; b exit_guest
fail_read_poll: mov r0, #118
exit_guest:
    mov r7, #1
    svc #0
1:  b 1b
    .size _start, .-_start
    .ltorg

    .section .rodata
server_address:
    .hword 2
    .byte 0x46, 0xa0
    .byte 127, 0, 0, 1
    .space 8
request:
    .ascii "GET / HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n"
    .equ request_length, .-request

    .section .data
    .align 2
write_poll: .space 8
socket_error: .word -1
option_length: .word 4
peer_length: .word 16

    .section .bss
peer_address: .space 16
response: .space 128
