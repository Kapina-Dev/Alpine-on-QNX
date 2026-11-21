/* Phase 4 part 1 measurement, not a regression test: the cost of a JIT's
   W^X flips. Maps 256 KiB of anonymous memory, fills it with a pattern
   that holds no svc-like word, and flips it RW -> RX -> RW N times
   (default 1000). Prints "flips=N usec=T". */
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#define REGION (256 * 1024)

int main(int argc, char **argv)
{
    int flips = argc > 1 ? atoi(argv[1]) : 1000;
    unsigned *words;
    struct timespec start, end;
    long usec;
    int i;

    words = mmap(0, REGION, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (words == MAP_FAILED) { perror("mmap"); return 1; }
    for (i = 0; i < REGION / 4; ++i) words[i] = 0xe1a00000u + (i & 0xff);
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (i = 0; i < flips; ++i) {
        if (mprotect(words, REGION, PROT_READ | PROT_EXEC) != 0 ||
            mprotect(words, REGION, PROT_READ | PROT_WRITE) != 0) {
            perror("mprotect");
            return 1;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    usec = (end.tv_sec - start.tv_sec) * 1000000L +
        (end.tv_nsec - start.tv_nsec) / 1000;
    printf("flips=%d usec=%ld\n", flips, usec);
    return 0;
}
