    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/*
 * Path records for directory fds, including numbers >= 256 and records
 * that must outlive the number they were duplicated from.
 */
_start:
    /* openat(AT_FDCWD, "/", O_RDONLY | O_DIRECTORY) */
    mvn r0, #99
    ldr r1, =root_path
    mov r2, #0x4000
    mov r3, #0
    movw r7, #322
    svc #0
    cmp r0, #0
    blt fail_open
    mov r4, r0

    /* dup2(d, 300) */
    mov r0, r4
    movw r1, #300
    mov r7, #63
    svc #0
    movw r1, #300
    cmp r0, r1
    bne fail_dup2

    /* getdents64(300, buffer, 4096) > 0 */
    movw r0, #300
    ldr r1, =dirent_buffer
    mov r2, #4096
    movw r7, #217
    svc #0
    cmp r0, #0
    ble fail_getdents

    /* fstatat64(300, "etc", stat_buffer, 0) == 0 */
    movw r0, #300
    ldr r1, =etc_path
    ldr r2, =stat_buffer
    mov r3, #0
    movw r7, #327
    svc #0
    cmp r0, #0
    bne fail_fstatat

    /* fchdir(300) == 0 */
    movw r0, #300
    mov r7, #133
    svc #0
    cmp r0, #0
    bne fail_fchdir

    /* close(d); the record on 300 must survive */
    mov r0, r4
    mov r7, #6
    svc #0
    movw r0, #300
    ldr r1, =etc_path
    ldr r2, =stat_buffer
    mov r3, #0
    movw r7, #327
    svc #0
    cmp r0, #0
    bne fail_survive

    /* fcntl64 F_SETFD and F_SETFL return exactly 0, not the previous
       flags (QNX returns those): set each twice, check the second call. */
    movw r0, #300
    mov r1, #2
    mov r2, #1
    movw r7, #221
    svc #0
    movw r0, #300
    mov r1, #2
    mov r2, #1
    movw r7, #221
    svc #0
    cmp r0, #0
    bne fail_setfd
    movw r0, #300
    mov r1, #4
    mov r2, #0x800
    movw r7, #221
    svc #0
    movw r0, #300
    mov r1, #4
    mov r2, #0x800
    movw r7, #221
    svc #0
    cmp r0, #0
    bne fail_setfl

    movw r0, #300
    mov r7, #6
    svc #0
    mov r0, #0
    b exit_guest

fail_setfd: mov r0, #136; b exit_guest
fail_setfl: mov r0, #137; b exit_guest

fail_open: mov r0, #130; b exit_guest
fail_getdents: mov r0, #131; b exit_guest
fail_fstatat: mov r0, #132; b exit_guest
fail_fchdir: mov r0, #133; b exit_guest
fail_survive: mov r0, #134; b exit_guest
fail_dup2: mov r0, #135
exit_guest:
    mov r7, #1
    svc #0
1:  b 1b
    .size _start, .-_start
    .ltorg

    .section .rodata
root_path: .asciz "/"
etc_path: .asciz "etc"

    .section .bss
    .align 3
dirent_buffer: .space 4096
stat_buffer: .space 128
