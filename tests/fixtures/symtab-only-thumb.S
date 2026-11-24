/* Host fixture for tests/code-map-test.c (Phase 4 part 4): a file that
   keeps .symtab but has no mapping symbols, like Android's
   /system/bin/linker. hidden is a local Thumb function with an svc and no
   other evidence (no unwind entry, not in .dynsym, never called), so only
   its .symtab STT_FUNC symbol says it is Thumb code. Built on the laptop:
     arm-linux-gnueabihf-gcc -shared -nostdlib -o symtab-only-thumb.so \
       symtab-only-thumb.S
     arm-linux-gnueabihf-objcopy --wildcard --strip-symbol='$*' \
       symtab-only-thumb.so symtab-only-thumb.nomap.so */
    .syntax unified
    .text
    .thumb
    .global exported
    .type exported, %function
    .thumb_func
exported:
    bx lr
    .type hidden, %function
    .thumb_func
hidden:
    movs r7, #20
    svc #0
    bx lr
