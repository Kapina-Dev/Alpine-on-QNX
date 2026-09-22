#include "linuxemu.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define LINUX_AT_NULL 0
#define LINUX_AT_PHDR 3
#define LINUX_AT_PHENT 4
#define LINUX_AT_PHNUM 5
#define LINUX_AT_PAGESZ 6
#define LINUX_AT_BASE 7
#define LINUX_AT_ENTRY 9
#define LINUX_AT_UID 11
#define LINUX_AT_EUID 12
#define LINUX_AT_GID 13
#define LINUX_AT_EGID 14
#define LINUX_AT_SECURE 23
#define LINUX_AT_RANDOM 25
#define LINUX_AT_EXECFN 31

struct guest_auxv {
    uint32_t type;
    uint32_t value;
};

static long host_page_size;
static int trace_enabled;
static uint32_t guest_tls_pointer;
extern char **environ;

static int guest_environment_entry(const char *value)
{
    return strncmp(value, "LINUXEMU_", 9) != 0 &&
        strncmp(value, "LD_LIBRARY_PATH=", 16) != 0;
}

__asm__(
    ".text\n"
    ".align 2\n"
    ".arm\n"
    ".global linuxemu_enter_guest\n"
    ".type linuxemu_enter_guest, %function\n"
    "linuxemu_enter_guest:\n"
    "mov ip, r0\n"
    "mov sp, r1\n"
    "mov r0, #0\n"
    "mov r1, #0\n"
    "mov r2, #0\n"
    "mov r3, #0\n"
    "mov r4, #0\n"
    "mov r5, #0\n"
    "mov r6, #0\n"
    "mov r7, #0\n"
    "mov r8, #0\n"
    "mov lr, #0\n"
    "bx ip\n"
    ".size linuxemu_enter_guest, .-linuxemu_enter_guest\n");

void runtime_initialize(long page_size, int enable_trace)
{
    host_page_size = page_size;
    trace_enabled = enable_trace;
}

long runtime_page_size(void) { return host_page_size; }
int runtime_trace_enabled(void) { return trace_enabled; }
uint32_t runtime_guest_tls(void) { return guest_tls_pointer; }
void runtime_set_guest_tls(uint32_t value) { guest_tls_pointer = value; }

static int fill_random_bytes(void *buffer, size_t length)
{
    unsigned char *cursor = buffer;
    int fd = open("/dev/urandom", O_RDONLY);

    if (fd < 0) return -1;
    while (length != 0) {
        ssize_t count = read(fd, cursor, length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            close(fd);
            errno = EIO;
            return -1;
        }
        cursor += count;
        length -= (size_t)count;
    }
    close(fd);
    return 0;
}

static uintptr_t push_bytes(uintptr_t *cursor, uintptr_t lower_bound,
    const void *source, size_t length)
{
    if (length > *cursor - lower_bound) {
        errno = E2BIG;
        return 0;
    }
    *cursor -= length;
    memcpy((void *)*cursor, source, length);
    return *cursor;
}

uintptr_t create_guest_stack(int guest_argc, char **guest_argv,
    const struct guest_image *image)
{
    void *mapping;
    uintptr_t lower_bound;
    uintptr_t cursor;
    uintptr_t vector_start;
    uint32_t *words;
    uint32_t *argument_addresses = 0;
    uint32_t *environment_addresses = 0;
    struct guest_auxv auxiliary[16];
    unsigned char random_bytes[16];
    uintptr_t random_address;
    size_t environment_count = 0;
    size_t auxiliary_count = 0;
    size_t word_count;
    size_t i;

    if (guest_argc <= 0 || guest_argv == 0 || guest_argv[0] == 0) {
        errno = EINVAL;
        return 0;
    }
    mapping = mmap(0, GUEST_STACK_SIZE, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON | MAP_STACK, -1, 0);
    if (mapping == MAP_FAILED) return 0;
    lower_bound = (uintptr_t)mapping;
    cursor = lower_bound + GUEST_STACK_SIZE;

    for (i = 0; environ[i] != 0; ++i)
        if (guest_environment_entry(environ[i])) environment_count++;
    argument_addresses = calloc((size_t)guest_argc,
        sizeof(*argument_addresses));
    environment_addresses = calloc(environment_count,
        sizeof(*environment_addresses));
    if (argument_addresses == 0 ||
        (environment_count != 0 && environment_addresses == 0)) goto fail;

    environment_count = 0;
    for (i = 0; environ[i] != 0; ++i) {
        uintptr_t address;
        if (!guest_environment_entry(environ[i])) continue;
        address = push_bytes(&cursor, lower_bound, environ[i],
            strlen(environ[i]) + 1);
        if (address == 0) goto fail;
        environment_addresses[environment_count++] = (uint32_t)address;
    }
    for (i = (size_t)guest_argc; i != 0; --i) {
        uintptr_t address = push_bytes(&cursor, lower_bound, guest_argv[i - 1],
            strlen(guest_argv[i - 1]) + 1);
        if (address == 0) goto fail;
        argument_addresses[i - 1] = (uint32_t)address;
    }
    if (fill_random_bytes(random_bytes, sizeof(random_bytes)) != 0) goto fail;
    cursor = align_down(cursor, 16);
    random_address = push_bytes(&cursor, lower_bound, random_bytes,
        sizeof(random_bytes));
    if (random_address == 0) goto fail;

#define ADD_AUX(type_value, data_value) do { \
    auxiliary[auxiliary_count].type = (type_value); \
    auxiliary[auxiliary_count].value = (uint32_t)(data_value); \
    auxiliary_count++; \
} while (0)
    if (image->program_headers != 0)
        ADD_AUX(LINUX_AT_PHDR, image->program_headers);
    ADD_AUX(LINUX_AT_PHENT, image->program_header_size);
    ADD_AUX(LINUX_AT_PHNUM, image->program_header_count);
    ADD_AUX(LINUX_AT_PAGESZ, host_page_size);
    ADD_AUX(LINUX_AT_BASE, image->interpreter_base);
    ADD_AUX(LINUX_AT_ENTRY, image->entry);
    ADD_AUX(LINUX_AT_UID, getuid());
    ADD_AUX(LINUX_AT_EUID, geteuid());
    ADD_AUX(LINUX_AT_GID, getgid());
    ADD_AUX(LINUX_AT_EGID, getegid());
    ADD_AUX(LINUX_AT_SECURE, 0);
    ADD_AUX(LINUX_AT_RANDOM, random_address);
    ADD_AUX(LINUX_AT_EXECFN, argument_addresses[0]);
#undef ADD_AUX

    word_count = 1 + (size_t)guest_argc + 1 + environment_count + 1 +
        (auxiliary_count + 1) * 2;
    if (word_count > (cursor - lower_bound) / sizeof(*words)) {
        errno = E2BIG;
        goto fail;
    }
    vector_start = align_down(cursor - word_count * sizeof(*words), 16);
    if (vector_start < lower_bound) {
        errno = E2BIG;
        goto fail;
    }
    words = (uint32_t *)vector_start;
    *words++ = (uint32_t)guest_argc;
    for (i = 0; i < (size_t)guest_argc; ++i) *words++ = argument_addresses[i];
    *words++ = 0;
    for (i = 0; i < environment_count; ++i) *words++ = environment_addresses[i];
    *words++ = 0;
    for (i = 0; i < auxiliary_count; ++i) {
        *words++ = auxiliary[i].type;
        *words++ = auxiliary[i].value;
    }
    *words++ = LINUX_AT_NULL;
    *words++ = 0;
    free(argument_addresses);
    free(environment_addresses);
    return vector_start;

fail:
    free(argument_addresses);
    free(environment_addresses);
    munmap(mapping, GUEST_STACK_SIZE);
    return 0;
}
