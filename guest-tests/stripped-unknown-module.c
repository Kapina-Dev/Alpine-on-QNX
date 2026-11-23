/* Phase 4 part 3 refusal fixture (stripped-unknown-armhf.so), Thumb,
   -funwind-tables, stripped. hidden_getpid has an unwind entry but no
   evidence of its mode: it is static, never called directly, and its
   address is only computed in code (a PC-relative offset, not a
   relocation). It holds an svc, so Linuxemu must refuse the library. */

static __attribute__((noinline, used)) int hidden_getpid(void)
{
    register int result __asm__("r0");
    __asm__ volatile(
        "push {r7}\n\t"
        "movw ip, #0x7fff\n\t"
        "movs r7, #20\n\t"
        "svc #0\n\t"
        "pop {r7}"
        : "=r"(result) : : "ip", "memory");
    return result;
}

int unknown_entry(int offset)
{
    /* The pointer is built at run time from a volatile offset. */
    volatile int bias = offset;
    int (*fn)(void) = (int (*)(void))((char *)hidden_getpid + bias);
    return fn();
}
