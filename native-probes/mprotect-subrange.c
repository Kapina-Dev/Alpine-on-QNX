#define _QNX_SOURCE 1
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int run_case(const char *name, int initial, int final)
{
    long page_size = sysconf(_SC_PAGESIZE);
    unsigned char *mapping;
    int result;
    int saved_errno;

    mapping = mmap(0, (size_t)page_size * 2, initial,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) {
        printf("case=%s mmap_failed errno=%d (%s)\n", name, errno,
            strerror(errno));
        return 1;
    }
    errno = 0;
    result = mprotect(mapping + page_size, (size_t)page_size, final);
    saved_errno = errno;
    printf("case=%s mapping=%p mprotect=%d errno=%d (%s)\n", name,
        mapping, result, saved_errno, strerror(saved_errno));
    munmap(mapping, (size_t)page_size * 2);
    return result != 0;
}

int main(void)
{
    int failures = 0;

    failures += run_case("none_to_rw", PROT_NONE, PROT_READ | PROT_WRITE);
    failures += run_case("rw_to_none", PROT_READ | PROT_WRITE, PROT_NONE);
    failures += run_case("rw_to_rx", PROT_READ | PROT_WRITE,
        PROT_READ | PROT_EXEC);
    printf("mprotect_subrange_failures=%d\n", failures);
    return failures != 0;
}
