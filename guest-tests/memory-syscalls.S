    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

_start:
    mov r0, #0
    mov r7, #45
    svc #0
    cmp r0, #0
    beq fail_brk_query
    mov r4, r0

    add r5, r4, #0x1000
    mov r0, r5
    mov r7, #45
    svc #0
    cmp r0, r5
    bne fail_brk_grow
    mov r6, #0x5a
    str r6, [r4]
    ldr r0, [r4]
    cmp r0, r6
    bne fail_brk_memory

    mov r0, #0
    mov r1, #0x1000
    mov r2, #3
    mov r3, #0x22
    mvn r4, #0
    mov r5, #0
    mov r7, #192
    svc #0
    cmp r0, #0
    bmi fail_mmap
    mov r4, r0
    ldr r5, =0x12345678
    str r5, [r4]
    ldr r0, [r4]
    cmp r0, r5
    bne fail_mmap_memory

    mov r0, r4
    mov r1, #0x1000
    mov r2, #1
    mov r7, #125
    svc #0
    cmp r0, #0
    bne fail_mprotect

    mov r0, r4
    mov r1, #0x1000
    mov r7, #91
    svc #0
    cmp r0, #0
    bne fail_munmap

    /* Reserve with PROT_NONE + MAP_NORESERVE, then open one page, the way
       bionic allocates guarded thread stacks and static TLS. Memory must be
       zero-filled (QNX's 0x4000 means "don't initialise"; Linux's does not). */
    mov r0, #0
    mov r1, #0x3000
    mov r2, #0
    ldr r3, =0x4022
    mvn r4, #0
    mov r5, #0
    mov r7, #192
    svc #0
    cmn r0, #4096
    bhi fail_noreserve
    mov r6, r0
    add r0, r6, #0x1000
    mov r1, #0x1000
    mov r2, #3
    mov r7, #125
    svc #0
    cmp r0, #0
    bne fail_noreserve
    add r0, r6, #0x1000
    ldr r1, [r0]
    cmp r1, #0
    bne fail_noreserve_zero
    ldr r1, =0x5a5a5a5a
    str r1, [r0]
    ldr r2, [r0]
    cmp r1, r2
    bne fail_noreserve_zero
    mov r0, r6
    mov r1, #0x3000
    mov r7, #91
    svc #0
    cmp r0, #0
    bne fail_munmap

    mov r0, #0
    b exit_guest
fail_noreserve:
    mov r0, #58
    b exit_guest
fail_noreserve_zero:
    mov r0, #59
    b exit_guest
fail_brk_query:
    mov r0, #51
    b exit_guest
fail_brk_grow:
    mov r0, #52
    b exit_guest
fail_brk_memory:
    mov r0, #53
    b exit_guest
fail_mmap:
    mov r0, #54
    b exit_guest
fail_mmap_memory:
    mov r0, #55
    b exit_guest
fail_mprotect:
    mov r0, #56
    b exit_guest
fail_munmap:
    mov r0, #57
exit_guest:
    mov r7, #1
    svc #0
1:
    b 1b
    .size _start, .-_start
    .ltorg

    .section .data
    .word 0
