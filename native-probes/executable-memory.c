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

static int set_protection(void *mapping, size_t length, int protection,
    const char *stage)
{
    int result;
    int saved_errno;

    printf("mode=%s stage=%s_begin protection=0x%x\n",
        PROBE_MODE, stage, protection);
    fflush(stdout);
    errno = 0;
    result = mprotect(mapping, length, protection);
    saved_errno = errno;
    printf("mode=%s stage=%s_end result=%d errno=%d\n",
        PROBE_MODE, stage, result, saved_errno);
    fflush(stdout);
    return result;
}

static int sync_instruction_cache(void *mapping, size_t length,
    const char *stage)
{
    int result;
    int saved_errno;

    printf("mode=%s stage=%s_begin flags=0x%x\n",
        PROBE_MODE, stage, MS_INVALIDATE_ICACHE);
    fflush(stdout);
    errno = 0;
    result = msync(mapping, length, MS_INVALIDATE_ICACHE);
    saved_errno = errno;
    printf("mode=%s stage=%s_end result=%d errno=%d\n",
        PROBE_MODE, stage, result, saved_errno);
    fflush(stdout);
    return result;
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    void *mapping;
    generated_function function;
    int initial_protection = PROT_READ | PROT_WRITE;
    int first;
    int second;

    alarm(10);
    if (page_size <= 0) {
        perror("sysconf");
        return 1;
    }

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
    printf("mode=%s stage=first_write_complete\n", PROBE_MODE);
    fflush(stdout);
    if (set_protection(mapping, (size_t)page_size,
            PROT_READ | PROT_EXEC, "first_mprotect_rx") != 0) {
        munmap(mapping, (size_t)page_size);
        return 1;
    }
    if (sync_instruction_cache(mapping, 8, "first_cache_sync") != 0) {
        munmap(mapping, (size_t)page_size);
        return 1;
    }

    function = callable_address(mapping);
    printf("mode=%s stage=first_call address=%p\n", PROBE_MODE,
        (void *)(uintptr_t)function);
    fflush(stdout);
    first = function();
    printf("mode=%s stage=first_return value=%d\n", PROBE_MODE, first);
    fflush(stdout);

    if (set_protection(mapping, (size_t)page_size,
            PROT_READ | PROT_WRITE, "second_mprotect_rw") != 0) {
        munmap(mapping, (size_t)page_size);
        return 1;
    }
    emit_function(mapping, 43);
    printf("mode=%s stage=second_write_complete\n", PROBE_MODE);
    fflush(stdout);
    if (set_protection(mapping, (size_t)page_size,
            PROT_READ | PROT_EXEC, "second_mprotect_rx") != 0) {
        munmap(mapping, (size_t)page_size);
        return 1;
    }
    if (sync_instruction_cache(mapping, 8, "second_cache_sync") != 0) {
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
