/* The ARM half of the Phase 4 part 3 stripped-decoding fixture, built
   -marm -funwind-tables into the same library as stripped-mixed-module.c.
   Hidden functions are reached by blx from Thumb; mixed_arm_getpid is
   exported (an even .dynsym value). */

#define ARM_GETPID(out) do {                                         \
        register int r0_ __asm__("r0");                                 \
        __asm__ volatile(                                               \
            "push {r7}\n\t"                                             \
            "movw ip, #0x7fff\n\t"                                      \
            "mov r7, #20\n\t"                                           \
            "svc #0\n\t"                                                \
            "pop {r7}"                                                  \
            : "=r"(r0_) : : "ip", "memory");                            \
        (out) = r0_;                                                    \
    } while (0)

__attribute__((visibility("hidden"))) int arm_getpid_inner(void)
{
    int result;
    ARM_GETPID(result);
    return result;
}

__attribute__((visibility("hidden"))) void *arm_tp_inner(void)
{
    void *value;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(value));
    return value;
}

int mixed_arm_getpid(void)
{
    int result;
    ARM_GETPID(result);
    return result;
}

unsigned mixed_arm_pool(void)
{
    unsigned a;
    __asm__ volatile(
        "ldr %0, =0xef000000\n\t"
        "b 1f\n\t"
        ".ltorg\n"
        "1:"
        : "=r"(a));
    return a;
}
