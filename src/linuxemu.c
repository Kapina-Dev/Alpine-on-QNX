#include "linuxemu.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef LINUXEMU_TESTING
static int occupy_test_range(void)
{
    void *mapping;
    uintptr_t address;
    size_t length;
    if (getenv("LINUXEMU_TEST_OCCUPY_STATIC") != 0) {
        address = 0x000ff000u;
        length = 0x2000;
    } else if (getenv("LINUXEMU_TEST_OCCUPY_DYNAMIC") != 0) {
        address = MAIN_ET_DYN_BIAS;
        length = 0x1000;
    } else return 0;
    mapping = mmap((void *)address, length,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) return -1;
    if (mapping != (void *)address) {
        munmap(mapping, length);
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
    const char *load_path;
    const char *initial_cwd = 0;
    char **guest_argv;
    int guest_argc;

    if (argc >= 6 && strcmp(argv[1], "--linuxemu-exec") == 0) {
        root = argv[2];
        initial_cwd = argv[3];
        load_path = argv[4];
        guest_argv = &argv[5];
        guest_argc = argc - 5;
    } else if (argc >= 2) {
        root = getenv("LINUXEMU_ROOT");
        load_path = argv[1];
        guest_argv = &argv[1];
        guest_argc = argc - 1;
    } else {
        fprintf(stderr, "usage: %s ARM_LINUX_ELF [ARG ...]\n", argv[0]);
        return 2;
    }
    page_size = sysconf(_SC_PAGESIZE);
    runtime_initialize(page_size, getenv("LINUXEMU_SYSTRACE") != 0);
    if (guest_process_initialize(argv[0]) != 0) {
        perror("linuxemu executable");
        return 1;
    }
    if (root != 0 && (guest_path_initialize(root) != 0 ||
            (initial_cwd != 0 && guest_path_chdir(initial_cwd) != 0))) {
        perror("linuxemu rootfs");
        return 1;
    }
    if (guest_thread_initialize() != 0 ||
            guest_memory_initialize(page_size) != 0 ||
            install_guest_traps() != 0) {
        perror("linuxemu initialization");
        return 1;
    }
#ifdef LINUXEMU_TESTING
    if (occupy_test_range() != 0) {
        perror("linuxemu test setup");
        return 1;
    }
#endif
    if (load_guest_image(load_path, &image) != 0) {
        if (errno == EEXIST && getenv("LINUXEMU_TEST_OCCUPY_STATIC") == 0)
            guest_process_retry_load(argv);
        perror("linuxemu load");
        return 1;
    }
    stack_pointer = create_guest_stack(guest_argc, guest_argv, &image);
    if (stack_pointer == 0) {
        perror("linuxemu stack");
        return 1;
    }

    if (getenv("LINUXEMU_VERBOSE") != 0) {
        printf("linuxemu: entry=%p start=%p segments=%u patches=%u stack=%p\n",
            (void *)image.entry, (void *)image.start_entry,
            (unsigned)guest_memory_segment_count(), (unsigned)arm_patch_count(),
            (void *)stack_pointer);
        fflush(stdout);
    }
    linuxemu_enter_guest(image.start_entry, stack_pointer);
    return 126;
}
