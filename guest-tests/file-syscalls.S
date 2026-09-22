    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

_start:
    ldr r0, =etc_path
    ldr r1, =0x24000
    mov r2, #0
    mov r7, #5
    svc #0
    cmp r0, #0
    bmi fail_open_directory
    mov r4, r0

    mov r0, r4
    ldr r1, =release_name
    ldr r2, =0x20000
    mov r3, #0
    movw r7, #322
    svc #0
    cmp r0, #0
    bmi fail_openat
    mov r5, r0

    mov r0, r5
    ldr r1, =read_buffer
    mov r2, #16
    mov r7, #3
    svc #0
    cmp r0, #0
    ble fail_read

    mov r0, r5
    mov r1, #0
    mov r2, #0
    mov r7, #19
    svc #0
    cmp r0, #0
    bne fail_lseek

    mov r0, r5
    ldr r1, =stat_buffer
    mov r7, #197
    svc #0
    cmp r0, #0
    bne fail_fstat

    mov r0, r4
    ldr r1, =release_name
    ldr r2, =stat_buffer
    mov r3, #0
    movw r7, #327
    svc #0
    cmp r0, #0
    bne fail_fstatat

    mov r0, r5
    mov r7, #6
    svc #0
    cmp r0, #0
    bne fail_close
    mov r0, r4
    mov r7, #6
    svc #0
    cmp r0, #0
    bne fail_close

    ldr r0, =bin_path
    ldr r1, =0x24000
    mov r2, #0
    mov r7, #5
    svc #0
    cmp r0, #0
    bmi fail_open_directory
    mov r4, r0
    mov r0, r4
    ldr r1, =sh_name
    ldr r2, =read_buffer
    mov r3, #64
    movw r7, #332
    svc #0
    cmp r0, #0
    ble fail_readlinkat
    mov r0, r4
    mov r7, #6
    svc #0
    cmp r0, #0
    bne fail_close

    mov r0, #0
    b exit_guest
fail_open_directory:
    mov r0, #61
    b exit_guest
fail_openat:
    mov r0, #62
    b exit_guest
fail_read:
    mov r0, #63
    b exit_guest
fail_lseek:
    mov r0, #64
    b exit_guest
fail_fstat:
    mov r0, #65
    b exit_guest
fail_fstatat:
    mov r0, #66
    b exit_guest
fail_close:
    mov r0, #67
    b exit_guest
fail_readlinkat:
    mov r0, #68
exit_guest:
    mov r7, #1
    svc #0
1:
    b 1b
    .size _start, .-_start
    .ltorg

    .section .rodata
etc_path:
    .asciz "/etc"
bin_path:
    .asciz "/bin"
release_name:
    .asciz "alpine-release"
sh_name:
    .asciz "sh"

    .section .bss
    .align 3
stat_buffer:
    .space 96
read_buffer:
    .space 64
