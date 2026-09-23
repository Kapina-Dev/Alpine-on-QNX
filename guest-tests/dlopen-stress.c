#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

typedef int (*integer_function)(void);

int main(void)
{
    int iteration;
    for (iteration = 0; iteration != 100; ++iteration) {
        void *module = dlopen("/tmp/libphase9.so", RTLD_NOW | RTLD_LOCAL);
        integer_function direct_getpid;
        integer_function value;
        if (module == 0) {
            fprintf(stderr, "dlopen: %s\n", dlerror());
            return 1;
        }
        direct_getpid = (integer_function)dlsym(module,
            "phase9_direct_getpid");
        value = (integer_function)dlsym(module, "phase9_value");
        if (direct_getpid == 0 || value == 0 ||
            direct_getpid() != (int)getpid() || value() != 0x2909) {
            fprintf(stderr, "dynamic symbol execution failed\n");
            return 2;
        }
        if (dlclose(module) != 0) {
            fprintf(stderr, "dlclose: %s\n", dlerror());
            return 3;
        }
    }
    puts("dlopen_stress=PASS iterations=100");
    return 0;
}
