#define _QNX_SOURCE 1
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/neutrino.h>

#define CPU_COUNT 4

struct cpu_result {
    unsigned cpu;
    int threadctl_result;
    int threadctl_errno;
    uintptr_t before;
    uintptr_t after;
};

static uintptr_t read_tpidrurw(void)
{
    uintptr_t value;
    __asm__ volatile ("mrc p15, 0, %0, c13, c0, 2" : "=r" (value));
    return value;
}

static void write_tpidrurw(uintptr_t value)
{
    __asm__ volatile (
        "mcr p15, 0, %0, c13, c0, 2\n"
        "isb\n"
        : : "r" (value) : "memory");
}

static void *clear_cpu(void *argument)
{
    struct cpu_result *result = argument;
    uintptr_t runmask = (uintptr_t)1u << result->cpu;

    errno = 0;
    result->threadctl_result = ThreadCtl(_NTO_TCTL_RUNMASK, (void *)runmask);
    result->threadctl_errno = errno;
    if (result->threadctl_result == 0) {
        result->before = read_tpidrurw();
        write_tpidrurw(0);
        result->after = read_tpidrurw();
    }
    return 0;
}

int main(void)
{
    struct cpu_result results[CPU_COUNT];
    pthread_t threads[CPU_COUNT];
    int created = 0;
    int error;
    int i;
    int ok = 1;

    memset(results, 0, sizeof(results));
    for (i = 0; i < CPU_COUNT; ++i) {
        results[i].cpu = (unsigned)i;
        error = pthread_create(&threads[i], 0, clear_cpu, &results[i]);
        if (error != 0) {
            printf("pthread_create[%d]=%d\n", i, error);
            ok = 0;
            break;
        }
        created++;
    }
    for (i = 0; i < created; ++i) {
        error = pthread_join(threads[i], 0);
        if (error != 0) {
            printf("pthread_join[%d]=%d\n", i, error);
            ok = 0;
        }
        printf("cpu=%u threadctl_result=%d errno=%d before=%p after=%p\n",
            results[i].cpu, results[i].threadctl_result,
            results[i].threadctl_errno, (void *)results[i].before,
            (void *)results[i].after);
        if (results[i].threadctl_result != 0 || results[i].after != 0) {
            ok = 0;
        }
    }
    printf("tpidrurw_cleanup=%s cpus=%d\n",
        ok && created == CPU_COUNT ? "PASS" : "FAIL", created);
    return ok && created == CPU_COUNT ? 0 : 1;
}
