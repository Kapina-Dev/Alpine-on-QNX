#include <stdint.h>

int phase9_direct_getpid(void)
{
    register uint32_t result __asm__("r0");
    register uint32_t syscall_number __asm__("r7") = 20;
    __asm__ volatile("svc #0" : "=r"(result) : "r"(syscall_number) : "memory");
    return (int)result;
}

int phase9_value(void)
{
    return 0x2909;
}
