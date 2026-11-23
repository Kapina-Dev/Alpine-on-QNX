/* A Thumb-2 shared library for the Phase 4 part 2 dlopen fixture: a raw
   Thumb svc, a Thumb TPIDRURO read, and a data word in .text ($d) that
   looks like ARM svc #0. Built unstripped, so it keeps its mapping
   symbols. */

int thumb_getpid(void)
{
    register int result __asm__("r0");

    /* ip = an invalid QNX call, in case the svc ever runs unpatched. */
    __asm__ volatile(
        "push {r7}\n\t"
        "movw ip, #0x7fff\n\t"
        "movs r7, #20\n\t"
        "svc #0\n\t"
        "pop {r7}"
        : "=r"(result) : : "ip", "memory");
    return result;
}

void *thumb_tp(void)
{
    void *value;

    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(value));
    return value;
}

__asm__(
    ".text\n"
    ".balign 4\n"
    ".hidden thumb_data_word\n"
    ".type thumb_data_word, %object\n"
    "thumb_data_word:\n"
    ".word 0xef000000\n"
    ".size thumb_data_word, 4\n");

extern const unsigned thumb_data_word[];

const unsigned *thumb_data(void)
{
    return thumb_data_word;
}
