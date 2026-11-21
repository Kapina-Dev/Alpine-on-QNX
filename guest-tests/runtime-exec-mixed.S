    .syntax unified
    .arm
    .section .text
    .global _start
    .type _start, %function

/* One mprotect over a file-backed page and an anonymous page: the file
   page (this binary's .text, file offset 0x1000, mapped private and
   writable, so Linuxemu copies it in and patches its svc instructions when
   it is mapped) keeps its traps; the anonymous page is not patched (Phase 4
   part 1). 61 mmap, 62 mprotect, 64 open, 66 the anonymous word changed,
   67 an svc word left in the file page, 68 no trap word in the file page. */
_start:
    ldr r0, [sp, #4]         @ argv[0]: this binary
    mov r1, #0               @ O_RDONLY
    mov r7, #5
    svc #0
    cmp r0, #0
    blt fail_open
    mov r6, r0

    mov r0, #0
    mov r1, #0x2000
    mov r2, #3
    mov r3, #0x22
    mvn r4, #0
    mov r5, #0
    mov r7, #192
    svc #0
    cmn r0, #4096
    bhi fail_mmap
    mov r8, r0
    add r9, r8, #0x1000      @ the anonymous page

    ldr r0, =0xef000000
    str r0, [r9]

    mov r0, r8               @ file page 1 over the first anonymous page
    mov r1, #0x1000
    mov r2, #3
    mov r3, #0x12            @ MAP_PRIVATE | MAP_FIXED
    mov r4, r6
    mov r5, #1               @ offset in pages
    mov r7, #192
    svc #0
    cmp r0, r8
    bne fail_mmap

    mov r0, r8
    mov r1, #0x2000
    mov r2, #5
    mov r7, #125
    svc #0
    cmp r0, #0
    bne fail_mprotect

    ldr r0, [r9]
    ldr r1, =0xef000000
    cmp r0, r1
    bne fail_anon
    ldr r1, =0xef000000
    bl count_word
    cmp r0, #0
    bne fail_file
    ldr r1, =0xe7f000f0      @ Linuxemu's ARM trap
    bl count_word
    cmp r0, #0
    beq fail_nosvc

    mov r0, #0
    b exit_guest

/* r0 = number of words equal to r1 in the page at r8 */
count_word:
    mov r0, #0
    mov r2, r8
    add r3, r8, #0x1000
1:  ldr r12, [r2], #4
    cmp r12, r1
    addeq r0, r0, #1
    cmp r2, r3
    blo 1b
    bx lr

fail_mmap:
    mov r0, #61
    b exit_guest
fail_mprotect:
    mov r0, #62
    b exit_guest
fail_open:
    mov r0, #64
    b exit_guest
fail_anon:
    mov r0, #66
    b exit_guest
fail_file:
    mov r0, #67
    b exit_guest
fail_nosvc:
    mov r0, #68
exit_guest:
    mov r7, #1
    svc #0
2:
    b 2b
    .size _start, .-_start
    .ltorg
