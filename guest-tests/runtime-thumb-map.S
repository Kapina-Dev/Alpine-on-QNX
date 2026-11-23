    .syntax unified
    .arch armv7-a
    .arm
    .section .text
    .global _start
    .type _start, %function

/* A runtime mapping of this binary's own Thumb page (vaddr 0x101000, file offset 0x2000)
   is patched by mapping symbol: its Thumb svc and Thumb TLS read become
   traps that work, and the $d word that looks like an ARM svc #0 stays
   intact (Phase 4 part 2).

   No argument: mmap the page read-execute.  With an argument: mmap it
   read-write, then mprotect it read-execute (the mprotect must not
   re-patch it).

   71 open, 72 mmap, 73 the $d word changed, 74 the svc halfword is not a
   trap, 75 the TLS read is not a trap, 76 the Thumb getpid is wrong,
   77 the Thumb TLS read is wrong, 78 mprotect. */
_start:
    ldr r11, [sp]            @ argc
    ldr r0, =0x12340000
    ldr r7, =0xf0005         @ __ARM_NR_set_tls
    svc #0

    ldr r0, [sp, #4]         @ argv[0]: this binary
    mov r1, #0               @ O_RDONLY
    mov r7, #5
    svc #0
    cmp r0, #0
    blt fail_open
    mov r6, r0

    mov r0, #0
    mov r1, #0x1000
    mov r2, #5               @ PROT_READ | PROT_EXEC
    cmp r11, #2
    movge r2, #3             @ PROT_READ | PROT_WRITE
    mov r3, #0x02            @ MAP_PRIVATE
    mov r4, r6
    mov r5, #2               @ offset in pages: the segment starts at 0xff000
    mov r7, #192
    svc #0
    cmn r0, #4096
    bhi fail_mmap
    mov r8, r0

    cmp r11, #2
    blt 1f
    mov r0, r8
    mov r1, #0x1000
    mov r2, #5
    mov r7, #125
    svc #0
    cmp r0, #0
    bne fail_mprotect
1:
    ldr r1, =data_word
    ldr r3, =thumb_page
    sub r1, r1, r3
    ldr r0, [r8, r1]
    ldr r2, =0xef000000
    cmp r0, r2
    bne fail_data

    ldr r1, =svc_site
    ldr r3, =thumb_page
    sub r1, r1, r3
    ldrh r0, [r8, r1]
    ldr r2, =0xde00
    cmp r0, r2
    bne fail_svc_site

    ldr r1, =thumb_tls
    ldr r3, =thumb_page
    sub r1, r1, r3
    ldrh r0, [r8, r1]
    cmp r0, r2
    bne fail_tls_site

    mov r7, #20              @ getpid, from ARM
    svc #0
    mov r9, r0
    ldr r1, =thumb_svc
    ldr r3, =thumb_page
    sub r1, r1, r3
    add r4, r8, r1
    orr r4, r4, #1
    blx r4
    cmp r0, r9
    bne fail_getpid

    ldr r1, =thumb_tls
    ldr r3, =thumb_page
    sub r1, r1, r3
    add r4, r8, r1
    orr r4, r4, #1
    blx r4
    ldr r1, =0x12340000
    cmp r0, r1
    bne fail_tls

    mov r0, #0
    b exit_guest

fail_open:
    mov r0, #71
    b exit_guest
fail_mmap:
    mov r0, #72
    b exit_guest
fail_data:
    mov r0, #73
    b exit_guest
fail_svc_site:
    mov r0, #74
    b exit_guest
fail_tls_site:
    mov r0, #75
    b exit_guest
fail_getpid:
    mov r0, #76
    b exit_guest
fail_tls:
    mov r0, #77
    b exit_guest
fail_mprotect:
    mov r0, #78
exit_guest:
    mov r7, #1
    svc #0
2:
    b 2b
    .size _start, .-_start
    .ltorg

/* Vaddr 0x101000, file offset 0x2000: the page the test maps at run time. */
    .balign 4096
thumb_page:
    .thumb
thumb_svc:
    movw ip, #0x7fff         @ an invalid QNX call if the svc runs unpatched
    movs r7, #20             @ getpid
svc_site:
    svc #0
    bx lr
thumb_tls:
    mrc p15, 0, r0, c13, c0, 3
    bx lr
    .balign 4
data_word:
    .word 0xef000000         @ data that looks like ARM svc #0
    .thumb
    nop
    bx lr
    .balign 4096
