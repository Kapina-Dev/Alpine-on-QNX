/* Host fixture for tests/code-map-test.c (final review of Phase 4 part 3):
   an ARM function with a string inside it and an odd pointer to that
   string in .data. The pointer lies inside unwind-table coverage, so it
   is weak Thumb evidence; it must not split armfn, whose svc stays ARM.
   Built with: arm-linux-gnueabihf-gcc -shared -nostdlib -o
   odd-pointer-arm.so odd-pointer-arm.S, then arm-linux-gnueabihf-strip
   -o odd-pointer-arm.stripped.so odd-pointer-arm.so. */
    .syntax unified
    .text
    .arm
    .global armfn
    .type armfn, %function
    .align 2
armfn:
    .fnstart
    b 1f
str:
    .ascii "abcd"
1:  push {r7, lr}

    svc #0
    pop {r7, pc}
    .fnend
    .size armfn, .-armfn

    .thumb
    .global thumbfn
    .type thumbfn, %function
    .thumb_func
thumbfn:
    .fnstart
    movs r0, #1
    bx lr
    .fnend
    .size thumbfn, .-thumbfn

    .data
    .global ptr
ptr: .word str+1
