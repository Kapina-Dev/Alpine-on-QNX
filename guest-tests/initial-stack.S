    .syntax unified
    .arm
    .text
    .global _start
    .type _start, %function

_start:
    mov r4, sp
    tst r4, #15
    bne fail_alignment

    ldr r0, [r4]
    cmp r0, #3
    bne fail_argc
    ldr r8, [r4, #4]
    cmp r8, #0
    beq fail_argv
    ldrb r0, [r8]
    cmp r0, #0
    beq fail_argv

    ldr r0, [r4, #8]
    ldr r1, =expected_alpha
    bl compare_string
    cmp r0, #0
    bne fail_argv
    ldr r0, [r4, #12]
    ldr r1, =expected_beta
    bl compare_string
    cmp r0, #0
    bne fail_argv
    ldr r0, [r4, #16]
    cmp r0, #0
    bne fail_argv

    add r5, r4, #20
find_environment_end:
    ldr r0, [r5], #4
    cmp r0, #0
    bne find_environment_end

    mov r6, #0
find_auxiliary:
    ldr r0, [r5], #4
    ldr r1, [r5], #4
    cmp r0, #0
    beq check_auxiliary
    cmp r0, #3
    beq found_phdr
    cmp r0, #4
    beq found_phent
    cmp r0, #5
    beq found_phnum
    cmp r0, #6
    beq found_pagesz
    cmp r0, #9
    beq found_entry
    cmp r0, #25
    beq found_random
    cmp r0, #31
    beq found_execfn
    b find_auxiliary

found_phdr:
    cmp r1, #0
    orrne r6, r6, #16
    b find_auxiliary
found_phent:
    cmp r1, #32
    orreq r6, r6, #32
    b find_auxiliary
found_phnum:
    cmp r1, #0
    orrne r6, r6, #64
    b find_auxiliary
found_pagesz:
    cmp r1, #4096
    orreq r6, r6, #1
    b find_auxiliary
found_entry:
    ldr r2, =_start
    cmp r1, r2
    orreq r6, r6, #2
    b find_auxiliary
found_random:
    cmp r1, #0
    beq find_auxiliary
    ldm r1, {r0-r3}
    orr r0, r0, r1
    orr r0, r0, r2
    orrs r0, r0, r3
    orrne r6, r6, #8
    b find_auxiliary
found_execfn:
    cmp r1, r8
    orreq r6, r6, #4
    b find_auxiliary

check_auxiliary:
    cmp r6, #127
    bne fail_auxiliary
    mov r0, #0
    b exit_guest

fail_alignment:
    mov r0, #41
    b exit_guest
fail_argc:
    mov r0, #42
    b exit_guest
fail_argv:
    mov r0, #43
    b exit_guest
fail_auxiliary:
    mov r0, #44

exit_guest:
    mov r7, #1
    svc #0

compare_string:
    ldrb r2, [r0], #1
    ldrb r3, [r1], #1
    cmp r2, r3
    bne strings_differ
    cmp r2, #0
    bne compare_string
    mov r0, #0
    bx lr
strings_differ:
    mov r0, #1
    bx lr

expected_alpha:
    .asciz "alpha"
expected_beta:
    .asciz "beta"
    .align 2
    .ltorg
