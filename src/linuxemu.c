#include "linuxemu.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef LINUXEMU_TESTING
static int occupy_test_range(void)
{
    void *mapping;
    if (getenv("LINUXEMU_TEST_OCCUPY_STATIC") == 0) return 0;
    mapping = mmap((void *)0x000ff000u, 0x2000,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) return -1;
    if (mapping != (void *)0x000ff000u) {
        munmap(mapping, 0x2000);
        errno = EEXIST;
        return -1;
    }
    return 0;
}
#endif

int main(int argc, char **argv)
{
    struct guest_image image;
    uintptr_t stack_pointer;
    long page_size;
    const char *root;

    if (argc < 2) {
        fprintf(stderr, "usage: %s ARM_LINUX_ELF [ARG ...]\n", argv[0]);
        return 2;
    }
    page_size = sysconf(_SC_PAGESIZE);
    runtime_initialize(page_size, getenv("LINUXEMU_SYSTRACE") != 0);
    root = getenv("LINUXEMU_ROOT");
    if (root != 0 && guest_path_initialize(root) != 0) {
        perror("linuxemu rootfs");
        return 1;
    }
    if (guest_memory_initialize(page_size) != 0 || install_guest_traps() != 0) {
        perror("linuxemu initialization");
        return 1;
    }
#ifdef LINUXEMU_TESTING
    if (occupy_test_range() != 0) {
        perror("linuxemu test setup");
        return 1;
    }
#endif
    if (load_guest_image(argv[1], &image) != 0) {
        perror("linuxemu load");
        return 1;
    }
    stack_pointer = create_guest_stack(argc - 1, &argv[1], &image);
    if (stack_pointer == 0) {
        perror("linuxemu stack");
        return 1;
    }

    printf("linuxemu: entry=%p start=%p segments=%u patches=%u stack=%p\n",
        (void *)image.entry, (void *)image.start_entry,
        (unsigned)guest_memory_segment_count(), (unsigned)arm_patch_count(),
        (void *)stack_pointer);
    fflush(stdout);
    linuxemu_enter_guest(image.start_entry, stack_pointer);
    return 126;
}
