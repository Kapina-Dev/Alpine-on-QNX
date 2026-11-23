/* dlopen a Thumb-2 library (guest-tests/thumb-module.c) and use it.
   1 dlopen/dlsym, 2 the library's $d word changed, 3 its Thumb svc
   returned the wrong pid, 4 its Thumb TLS read differs from ours. */
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    void *library;
    int (*thumb_getpid)(void);
    void *(*thumb_tp)(void);
    const unsigned *(*thumb_data)(void);
    void *ours;

    if (argc != 2) return 1;
    library = dlopen(argv[1], RTLD_NOW);
    if (library == 0) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    thumb_getpid = (int (*)(void))dlsym(library, "thumb_getpid");
    thumb_tp = (void *(*)(void))dlsym(library, "thumb_tp");
    thumb_data = (const unsigned *(*)(void))dlsym(library, "thumb_data");
    if (thumb_getpid == 0 || thumb_tp == 0 || thumb_data == 0) return 1;
    if (*thumb_data() != 0xef000000u) return 2;
    if (thumb_getpid() != getpid()) return 3;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(ours));
    if (thumb_tp() != ours) return 4;
    puts("thumb-dlopen ok");
    return 0;
}
