/* Stage 3 refusal fixture (stripped-unknown-armhf.so), stripped Thumb.
   hidden_getpid has an unwind entry but no evidence of its mode at all:
   no caller, no relocation, no .dynsym entry and no address formed in code
   (Phase 4 part 4 follows ldr/add pc and movw/movt formation, which the
   part 3 C version of this fixture used). It holds an svc, so Linuxemu
   must refuse the library. Different unwind data (.save) keeps ld from
   merging the two unwind entries. Built by scripts/build-stripped-fixtures.sh
   (the committed copy: arm-linux-gnueabihf-gcc -shared -nostdlib, then
   arm-linux-gnueabihf-strip; same assembly). */
    .syntax unified
    .thumb
    .text
    .global unknown_entry
    .type unknown_entry, %function
    .thumb_func
unknown_entry:
    .fnstart
    movs r0, #0
    bx lr
    .fnend

    .type hidden_getpid, %function
    .thumb_func
hidden_getpid:
    .fnstart
    .save {r7}
    push {r7}
    movw ip, #0x7fff
    movs r7, #20
    svc #0
    pop {r7}
    bx lr
    .fnend
