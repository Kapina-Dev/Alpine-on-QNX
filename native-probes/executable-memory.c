#define _QNX_SOURCE 1
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef PROBE_MODE
#define PROBE_MODE "unknown"
#endif

typedef int (*generated_function)(void);

static void emit_function(void *mapping, int value)
{
#if defined(GENERATED_THUMB)
    uint16_t *code = mapping;
    code[0] = (uint16_t)(0x2000u | (unsigned)value); /* movs r0, #value */
    code[1] = 0x4770u;                              /* bx lr */
#else
    uint32_t *code = mapping;
    code[0] = 0xe3a00000u | (unsigned)value; /* mov r0, #value */
    code[1] = 0xe12fff1eu;                   /* bx lr */
#endif
}

static generated_function callable_address(void *mapping)
{
    union {
        uintptr_t address;
        generated_function function;
    } conversion;

    conversion.address = (uintptr_t)mapping;
#if defined(GENERATED_THUMB)
    conversion.address |= 1u;
#endif
    return conversion.function;
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    void *mapping;
    generated_function function;
    int initial_protection = PROT_READ | PROT_WRITE;
    int first;
    int second;

    if (page_size <= 0) {
        perror("sysconf");
        return 1;
    }

#if defined(INITIAL_RWX)
    initial_protection |= PROT_EXEC;
#endif
    mapping = mmap(0, (size_t)page_size, initial_protection,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) {
        printf("mode=%s mmap_error=%d %s\n", PROBE_MODE, errno,
            strerror(errno));
        return 1;
    }

    printf("mode=%s stage=mapped mapping=%p initial_protection=0x%x\n",
        PROBE_MODE, mapping, initial_protection);
    fflush(stdout);
    emit_function(mapping, 42);
    __builtin___clear_cache(mapping, (char *)mapping + 8);
#if !defined(INITIAL_RWX)
    if (mprotect(mapping, (size_t)page_size, PROT_READ | PROT_EXEC) != 0) {
        printf("mode=%s first_mprotect_rx_error=%d %s\n", PROBE_MODE,
            errno, strerror(errno));
        munmap(mapping, (size_t)page_size);
        return 1;
    }
#endif

    function = callable_address(mapping);
    printf("mode=%s stage=first_call address=%p\n", PROBE_MODE,
        (void *)(uintptr_t)function);
    fflush(stdout);
    first = function();
    printf("mode=%s stage=first_return value=%d\n", PROBE_MODE, first);
    fflush(stdout);

    if (mprotect(mapping, (size_t)page_size, PROT_READ | PROT_WRITE) != 0) {
        printf("mode=%s mprotect_rw_error=%d %s first=%d\n", PROBE_MODE,
            errno, strerror(errno), first);
        munmap(mapping, (size_t)page_size);
        return 1;
    }
    emit_function(mapping, 43);
    __builtin___clear_cache(mapping, (char *)mapping + 8);
    if (mprotect(mapping, (size_t)page_size, PROT_READ | PROT_EXEC) != 0) {
        printf("mode=%s second_mprotect_rx_error=%d %s first=%d\n",
            PROBE_MODE, errno, strerror(errno), first);
        munmap(mapping, (size_t)page_size);
        return 1;
    }

    second = function();
    printf("mode=%s page=%ld mapping=%p first=%d second=%d\n",
        PROBE_MODE, page_size, mapping, first, second);
    munmap(mapping, (size_t)page_size);
    printf("executable_memory=%s\n",
        first == 42 && second == 43 ? "PASS" : "FAIL");
    return first == 42 && second == 43 ? 0 : 1;
}
