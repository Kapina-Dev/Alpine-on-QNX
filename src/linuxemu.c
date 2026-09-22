#define _QNX_SOURCE 1
#include <sys/elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define MAX_LOAD_SEGMENTS 32
#define MAX_PATCHES 4096
#define GUEST_MIN_ADDRESS 0x00010000u
#define GUEST_MAX_ADDRESS 0x70000000u
#define GUEST_STACK_SIZE (1024u * 1024u)
#define MAIN_ET_DYN_BIAS 0x20000000u
#define INTERPRETER_BIAS 0x60000000u
#define ARM_LINUX_SVC_0 0xef000000u
#define ARM_TPIDRURO_MASK 0xffff0fffu
#define ARM_TPIDRURO_READ 0xee1d0f70u
#define ARM_UDF_TRAP 0xe7f000f0u
#define LINUX_ENOSYS 38
#define LINUX_NR_EXIT 1
#define LINUX_NR_WRITE 4
#define LINUX_NR_GETPID 20
#define LINUX_NR_MPROTECT 125
#define LINUX_NR_MMAP2 192
#define LINUX_NR_GETUID32 199
#define LINUX_NR_GETGID32 200
#define LINUX_NR_GETEUID32 201
#define LINUX_NR_GETEGID32 202
#define LINUX_NR_SET_TID_ADDRESS 256
#define LINUX_NR_EXIT_GROUP 248
#define LINUX_ARM_NR_SET_TLS 0x000f0005u
#define LINUX_KUSER_MEMORY_BARRIER 0xffff0fa0u
#define LINUX_KUSER_CMPXCHG 0xffff0fc0u
#define LINUX_KUSER_GET_TLS 0xffff0fe0u
#define LINUX_KUSER_VERSION_ADDRESS 0xffff0ffcu
#define LINUX_KUSER_VERSION 5u
#define ARM_KUSER_VERSION_LOAD 0xe51cc003u
#define LINUX_MAP_SHARED 0x00000001u
#define LINUX_MAP_PRIVATE 0x00000002u
#define LINUX_MAP_FIXED 0x00000010u
#define LINUX_MAP_ANONYMOUS 0x00000020u
#define LINUX_PROT_READ 0x1u
#define LINUX_PROT_WRITE 0x2u
#define LINUX_PROT_EXEC 0x4u
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

struct load_segment {
    uintptr_t map_start;
    size_t map_length;
    int final_protection;
};

struct patch_record {
    uintptr_t address;
    uint32_t original;
};

static struct load_segment load_segments[MAX_LOAD_SEGMENTS];
static size_t load_segment_count;
static struct patch_record patches[MAX_PATCHES];
static size_t patch_count;
static long host_page_size;
static uint32_t guest_tls_pointer;
static int trace_syscalls;
extern char **environ;

struct guest_image {
    uintptr_t entry;
    uintptr_t start_entry;
    uintptr_t interpreter_base;
    uintptr_t program_headers;
    uint32_t program_header_size;
    uint32_t program_header_count;
};

struct guest_auxv {
    uint32_t type;
    uint32_t value;
};

struct elf_object {
    uintptr_t entry;
    uintptr_t load_bias;
    uintptr_t program_headers;
    uint32_t program_header_size;
    uint32_t program_header_count;
};

extern void linuxemu_enter_guest(uintptr_t entry, uintptr_t stack_pointer);

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

static uintptr_t align_down(uintptr_t value, uintptr_t alignment)
{
    return value & ~(alignment - 1u);
}

static uintptr_t align_up(uintptr_t value, uintptr_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static int read_exact_at(int fd, void *buffer, size_t length, off_t offset)
{
    unsigned char *cursor = buffer;
    ssize_t result;

    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        return -1;
    }
    while (length != 0) {
        result = read(fd, cursor, length);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            errno = EIO;
            return -1;
        }
        cursor += result;
        length -= (size_t)result;
    }
    return 0;
}

static int protection_from_flags(Elf32_Word flags)
{
    int protection = 0;

    if (flags & PF_R) {
        protection |= PROT_READ;
    }
    if (flags & PF_W) {
        protection |= PROT_WRITE;
    }
    if (flags & PF_X) {
        protection |= PROT_EXEC;
    }
    return protection;
}

static int ranges_overlap(uintptr_t first_start, uintptr_t first_end,
    uintptr_t second_start, uintptr_t second_end)
{
    return first_start < second_end && second_start < first_end;
}

static int validate_load_range(uintptr_t start, uintptr_t end)
{
    size_t i;

    if (start < GUEST_MIN_ADDRESS || end <= start ||
        end > GUEST_MAX_ADDRESS) {
        errno = ENOEXEC;
        return -1;
    }
    for (i = 0; i < load_segment_count; ++i) {
        uintptr_t existing_end = load_segments[i].map_start +
            load_segments[i].map_length;
        if (ranges_overlap(start, end, load_segments[i].map_start,
                existing_end)) {
            errno = ENOEXEC;
            return -1;
        }
    }
    return 0;
}

static int patch_arm_instructions(uintptr_t start, size_t length)
{
    uintptr_t address;
    uintptr_t end = start + length;
    uint32_t *instruction;

    for (address = align_up(start, 4); address + 4 <= end; address += 4) {
        instruction = (uint32_t *)address;
        if (*instruction != ARM_LINUX_SVC_0 &&
            (*instruction & ARM_TPIDRURO_MASK) != ARM_TPIDRURO_READ) {
            continue;
        }
        if (patch_count == ARRAY_COUNT(patches)) {
            errno = E2BIG;
            return -1;
        }
        patches[patch_count].address = address;
        patches[patch_count].original = *instruction;
        patch_count++;
        *instruction = ARM_UDF_TRAP;
    }
    return 0;
}

static int map_load_segment(int fd, const Elf32_Phdr *header,
    uintptr_t load_bias)
{
    uintptr_t page = (uintptr_t)host_page_size;
    uintptr_t segment_start;
    uintptr_t segment_end;
    uintptr_t map_start;
    uintptr_t map_end;
    size_t prefix;
    void *mapping;
    int final_protection;

    if (header->p_memsz < header->p_filesz ||
        header->p_vaddr + header->p_memsz < header->p_vaddr ||
        header->p_offset + header->p_filesz < header->p_offset ||
        load_bias > UINTPTR_MAX - (uintptr_t)header->p_vaddr) {
        errno = ENOEXEC;
        return -1;
    }
    segment_start = load_bias + (uintptr_t)header->p_vaddr;
    if ((uintptr_t)header->p_memsz > UINTPTR_MAX - segment_start) {
        errno = ENOEXEC;
        return -1;
    }
    segment_end = segment_start + header->p_memsz;
    map_start = align_down(segment_start, page);
    map_end = align_up(segment_end, page);
    if (validate_load_range(map_start, map_end) != 0) {
        return -1;
    }
    if (load_segment_count == ARRAY_COUNT(load_segments)) {
        errno = E2BIG;
        return -1;
    }

    mapping = mmap((void *)map_start, map_end - map_start,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (mapping == MAP_FAILED || (uintptr_t)mapping != map_start) {
        return -1;
    }

    prefix = segment_start - map_start;
    if (header->p_filesz != 0 &&
        read_exact_at(fd, (void *)segment_start, header->p_filesz,
            (off_t)header->p_offset) != 0) {
        return -1;
    }
    if (header->p_memsz > header->p_filesz) {
        memset((void *)(segment_start + header->p_filesz), 0,
            header->p_memsz - header->p_filesz);
    }
    if (prefix != 0) {
        memset((void *)map_start, 0, prefix);
    }

    if ((header->p_flags & PF_X) != 0 &&
        patch_arm_instructions(segment_start, header->p_filesz) != 0) {
        return -1;
    }

    final_protection = protection_from_flags(header->p_flags);
    load_segments[load_segment_count].map_start = map_start;
    load_segments[load_segment_count].map_length = map_end - map_start;
    load_segments[load_segment_count].final_protection = final_protection;
    load_segment_count++;
    return 0;
}

static int finalize_segments(void)
{
    size_t i;

    for (i = 0; i < load_segment_count; ++i) {
        if (mprotect((void *)load_segments[i].map_start,
                load_segments[i].map_length,
                load_segments[i].final_protection) != 0) {
            return -1;
        }
        if ((load_segments[i].final_protection & PROT_EXEC) != 0 &&
            msync((void *)load_segments[i].map_start,
                load_segments[i].map_length,
                MS_INVALIDATE_ICACHE) != 0) {
            return -1;
        }
    }
    return 0;
}

static const struct patch_record *find_patch(uintptr_t address)
{
    size_t i;

    for (i = 0; i < patch_count; ++i) {
        if (patches[i].address == address) {
            return &patches[i];
        }
    }
    return 0;
}

static int is_executable_address(uintptr_t address)
{
    size_t i;

    for (i = 0; i < load_segment_count; ++i) {
        if ((load_segments[i].final_protection & PROT_EXEC) != 0 &&
            address >= load_segments[i].map_start &&
            address < load_segments[i].map_start +
                load_segments[i].map_length) {
            return 1;
        }
    }
    return 0;
}

static int32_t linux_result(ssize_t result)
{
    if (result < 0) {
        return -(int32_t)errno;
    }
    return (int32_t)result;
}

static int translate_linux_protection(uint32_t linux_protection,
    int *host_protection)
{
    if ((linux_protection &
            ~(LINUX_PROT_READ | LINUX_PROT_WRITE | LINUX_PROT_EXEC)) != 0) {
        return -1;
    }
    *host_protection = PROT_NONE;
    if ((linux_protection & LINUX_PROT_READ) != 0) {
        *host_protection |= PROT_READ;
    }
    if ((linux_protection & LINUX_PROT_WRITE) != 0) {
        *host_protection |= PROT_WRITE;
    }
    if ((linux_protection & LINUX_PROT_EXEC) != 0) {
        *host_protection |= PROT_EXEC;
    }
    return 0;
}

static int32_t linux_mmap2(ucontext_t *context)
{
    uint32_t linux_flags = context->uc_mcontext.cpu.gpr[3];
    int host_flags = 0;
    int host_protection;
    void *result;
    off_t offset;

    if (translate_linux_protection(context->uc_mcontext.cpu.gpr[2],
            &host_protection) != 0) {
        return -EINVAL;
    }
    if ((linux_flags & LINUX_MAP_SHARED) != 0) {
        host_flags |= MAP_SHARED;
    } else if ((linux_flags & LINUX_MAP_PRIVATE) != 0) {
        host_flags |= MAP_PRIVATE;
    } else {
        return -EINVAL;
    }
    if ((linux_flags & LINUX_MAP_FIXED) != 0) {
        host_flags |= MAP_FIXED;
    }
    if ((linux_flags & LINUX_MAP_ANONYMOUS) != 0) {
        host_flags |= MAP_ANON;
    }
    offset = (off_t)context->uc_mcontext.cpu.gpr[5] * 4096;
    result = mmap((void *)context->uc_mcontext.cpu.gpr[0],
        (size_t)context->uc_mcontext.cpu.gpr[1],
        host_protection, host_flags,
        (int)context->uc_mcontext.cpu.gpr[4], offset);
    if (result == MAP_FAILED) {
        return -(int32_t)errno;
    }
    return (int32_t)(uintptr_t)result;
}

static int32_t linux_mprotect(uintptr_t address, size_t length, int protection)
{
    if (mprotect((void *)address, length, protection) == 0) {
        return 0;
    }
    return -(int32_t)errno;
}

static size_t append_text(char *buffer, size_t position, const char *text)
{
    while (*text != '\0') {
        buffer[position++] = *text++;
    }
    return position;
}

static size_t append_hex32(char *buffer, size_t position, uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    int shift;

    for (shift = 28; shift >= 0; shift -= 4) {
        buffer[position++] = digits[(value >> shift) & 15u];
    }
    return position;
}

static void trace_guest_syscall(uint32_t number, const ucontext_t *context)
{
    char message[160];
    size_t length = 0;
    unsigned i;

    if (!trace_syscalls) {
        return;
    }
    length = append_text(message, length, "linuxemu: syscall=0x");
    length = append_hex32(message, length, number);
    for (i = 0; i != 6; ++i) {
        length = append_text(message, length, " r");
        message[length++] = (char)('0' + i);
        length = append_text(message, length, "=0x");
        length = append_hex32(message, length,
            context->uc_mcontext.cpu.gpr[i]);
    }
    message[length++] = '\n';
    write(STDERR_FILENO, message, length);
}

static void trace_guest_result(uint32_t result)
{
    char message[40];
    size_t length = 0;

    if (!trace_syscalls) {
        return;
    }
    length = append_text(message, length, "linuxemu: result=0x");
    length = append_hex32(message, length, result);
    message[length++] = '\n';
    write(STDERR_FILENO, message, length);
}

static void fatal_guest_signal(int sig, siginfo_t *info, uintptr_t pc)
{
    char message[80];
    size_t length = 0;

    length = append_text(message, length, "linuxemu: fatal signal=0x");
    length = append_hex32(message, length, (uint32_t)sig);
    length = append_text(message, length, " pc=0x");
    length = append_hex32(message, length, (uint32_t)pc);
    length = append_text(message, length, " address=0x");
    length = append_hex32(message, length, (uint32_t)(uintptr_t)info->si_addr);
    message[length++] = '\n';
    write(STDERR_FILENO, message, length);
    _exit(125);
}

static void syscall_handler(int sig, siginfo_t *info, void *argument)
{
    ucontext_t *context = argument;
    uintptr_t pc = context->uc_mcontext.cpu.gpr[15];
    const struct patch_record *patch = find_patch(pc);
    uint32_t syscall_number;
    int32_t result;

    if (sig == SIGSEGV) {
        if (pc == LINUX_KUSER_GET_TLS) {
            context->uc_mcontext.cpu.gpr[0] = guest_tls_pointer;
            context->uc_mcontext.cpu.gpr[15] =
                context->uc_mcontext.cpu.gpr[14];
            return;
        }
        if (pc == LINUX_KUSER_MEMORY_BARRIER) {
            __sync_synchronize();
            context->uc_mcontext.cpu.gpr[15] =
                context->uc_mcontext.cpu.gpr[14];
            return;
        }
        if (pc == LINUX_KUSER_CMPXCHG) {
            volatile uint32_t *address = (volatile uint32_t *)
                context->uc_mcontext.cpu.gpr[2];
            uint32_t old_value = context->uc_mcontext.cpu.gpr[0];
            if (__sync_val_compare_and_swap(address, old_value,
                    context->uc_mcontext.cpu.gpr[1]) == old_value) {
                context->uc_mcontext.cpu.gpr[0] = 0;
            } else {
                context->uc_mcontext.cpu.gpr[0] = (uint32_t)-1;
            }
            context->uc_mcontext.cpu.gpr[15] =
                context->uc_mcontext.cpu.gpr[14];
            return;
        }
        if ((uintptr_t)info->si_addr == LINUX_KUSER_VERSION_ADDRESS &&
            is_executable_address(pc) &&
            *(const uint32_t *)pc == ARM_KUSER_VERSION_LOAD) {
            context->uc_mcontext.cpu.gpr[12] = LINUX_KUSER_VERSION;
            context->uc_mcontext.cpu.gpr[15] = (uint32_t)(pc + 4u);
            return;
        }
        fatal_guest_signal(sig, info, pc);
    }
    if (sig != SIGILL || patch == 0) {
        fatal_guest_signal(sig, info, pc);
    }

    if ((patch->original & ARM_TPIDRURO_MASK) == ARM_TPIDRURO_READ) {
        unsigned destination_register = (patch->original >> 12) & 15u;
        context->uc_mcontext.cpu.gpr[destination_register] = guest_tls_pointer;
        context->uc_mcontext.cpu.gpr[15] = (uint32_t)(pc + 4u);
        return;
    }
    if (patch->original != ARM_LINUX_SVC_0) {
        fatal_guest_signal(sig, info, pc);
    }

    syscall_number = context->uc_mcontext.cpu.gpr[7];
    trace_guest_syscall(syscall_number, context);
    switch (syscall_number) {
    case LINUX_NR_WRITE:
        result = linux_result(write(
            (int)context->uc_mcontext.cpu.gpr[0],
            (const void *)context->uc_mcontext.cpu.gpr[1],
            (size_t)context->uc_mcontext.cpu.gpr[2]));
        context->uc_mcontext.cpu.gpr[0] = (uint32_t)result;
        break;
    case LINUX_NR_MMAP2:
        context->uc_mcontext.cpu.gpr[0] = (uint32_t)linux_mmap2(context);
        break;
    case LINUX_NR_GETPID:
        context->uc_mcontext.cpu.gpr[0] = (uint32_t)getpid();
        break;
    case LINUX_NR_GETUID32:
        context->uc_mcontext.cpu.gpr[0] = (uint32_t)getuid();
        break;
    case LINUX_NR_GETGID32:
        context->uc_mcontext.cpu.gpr[0] = (uint32_t)getgid();
        break;
    case LINUX_NR_GETEUID32:
        context->uc_mcontext.cpu.gpr[0] = (uint32_t)geteuid();
        break;
    case LINUX_NR_GETEGID32:
        context->uc_mcontext.cpu.gpr[0] = (uint32_t)getegid();
        break;
    case LINUX_NR_MPROTECT:
        if (translate_linux_protection(context->uc_mcontext.cpu.gpr[2],
                &result) != 0) {
            result = -EINVAL;
        } else {
            result = linux_mprotect(context->uc_mcontext.cpu.gpr[0],
                (size_t)context->uc_mcontext.cpu.gpr[1], result);
        }
        context->uc_mcontext.cpu.gpr[0] = (uint32_t)result;
        break;
    case LINUX_NR_SET_TID_ADDRESS:
        context->uc_mcontext.cpu.gpr[0] = (uint32_t)getpid();
        break;
    case LINUX_NR_EXIT:
    case LINUX_NR_EXIT_GROUP:
        _exit((int)(context->uc_mcontext.cpu.gpr[0] & 0xffu));
        break;
    case LINUX_ARM_NR_SET_TLS:
        guest_tls_pointer = context->uc_mcontext.cpu.gpr[0];
        context->uc_mcontext.cpu.gpr[0] = 0;
        break;
    default:
        context->uc_mcontext.cpu.gpr[0] = (uint32_t)-LINUX_ENOSYS;
        break;
    }
    trace_guest_result(context->uc_mcontext.cpu.gpr[0]);
    context->uc_mcontext.cpu.gpr[15] = (uint32_t)(pc + 4u);
}

static int install_syscall_handler(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_sigaction = syscall_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGILL, &action, 0) != 0) {
        return -1;
    }
    return sigaction(SIGSEGV, &action, 0);
}

static int fill_random_bytes(void *buffer, size_t length)
{
    unsigned char *cursor = buffer;
    int fd;

    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    while (length != 0) {
        ssize_t count = read(fd, cursor, length);
        if (count < 0 && errno == EINTR) {
            continue;
        }
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

static uintptr_t push_stack_bytes(uintptr_t *cursor, uintptr_t lower_bound,
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

static uintptr_t create_guest_stack(int guest_argc, char **guest_argv,
    const struct guest_image *image)
{
    void *mapping;
    uintptr_t lower_bound;
    uintptr_t cursor;
    uintptr_t vector_start;
    uint32_t *words;
    uint32_t *argument_addresses;
    uint32_t *environment_addresses;
    struct guest_auxv auxiliary[16];
    unsigned char random_bytes[16];
    uintptr_t random_address;
    uintptr_t executable_address;
    size_t environment_count = 0;
    size_t auxiliary_count = 0;
    size_t word_count;
    size_t i;

    mapping = mmap(0, GUEST_STACK_SIZE, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON | MAP_STACK, -1, 0);
    if (mapping == MAP_FAILED) {
        return 0;
    }
    lower_bound = (uintptr_t)mapping;
    cursor = lower_bound + GUEST_STACK_SIZE;

    while (environ[environment_count] != 0) {
        environment_count++;
    }
    argument_addresses = calloc((size_t)guest_argc, sizeof(*argument_addresses));
    environment_addresses = calloc(environment_count,
        sizeof(*environment_addresses));
    if ((guest_argc != 0 && argument_addresses == 0) ||
        (environment_count != 0 && environment_addresses == 0)) {
        goto fail;
    }

    for (i = environment_count; i != 0; --i) {
        uintptr_t address = push_stack_bytes(&cursor, lower_bound,
            environ[i - 1], strlen(environ[i - 1]) + 1);
        if (address == 0) {
            goto fail;
        }
        environment_addresses[i - 1] = (uint32_t)address;
    }
    for (i = (size_t)guest_argc; i != 0; --i) {
        uintptr_t address = push_stack_bytes(&cursor, lower_bound,
            guest_argv[i - 1], strlen(guest_argv[i - 1]) + 1);
        if (address == 0) {
            goto fail;
        }
        argument_addresses[i - 1] = (uint32_t)address;
    }
    executable_address = argument_addresses[0];

    if (fill_random_bytes(random_bytes, sizeof(random_bytes)) != 0) {
        goto fail;
    }
    cursor = align_down(cursor, 16);
    random_address = push_stack_bytes(&cursor, lower_bound, random_bytes,
        sizeof(random_bytes));
    if (random_address == 0) {
        goto fail;
    }

#define ADD_AUXILIARY(aux_type, aux_value) do { \
    auxiliary[auxiliary_count].type = (aux_type); \
    auxiliary[auxiliary_count].value = (uint32_t)(aux_value); \
    auxiliary_count++; \
} while (0)
    if (image->program_headers != 0) {
        ADD_AUXILIARY(LINUX_AT_PHDR, image->program_headers);
    }
    ADD_AUXILIARY(LINUX_AT_PHENT, image->program_header_size);
    ADD_AUXILIARY(LINUX_AT_PHNUM, image->program_header_count);
    ADD_AUXILIARY(LINUX_AT_PAGESZ, host_page_size);
    ADD_AUXILIARY(LINUX_AT_BASE, image->interpreter_base);
    ADD_AUXILIARY(LINUX_AT_ENTRY, image->entry);
    ADD_AUXILIARY(LINUX_AT_UID, getuid());
    ADD_AUXILIARY(LINUX_AT_EUID, geteuid());
    ADD_AUXILIARY(LINUX_AT_GID, getgid());
    ADD_AUXILIARY(LINUX_AT_EGID, getegid());
    ADD_AUXILIARY(LINUX_AT_SECURE, 0);
    ADD_AUXILIARY(LINUX_AT_RANDOM, random_address);
    ADD_AUXILIARY(LINUX_AT_EXECFN, executable_address);
#undef ADD_AUXILIARY

    word_count = 1 + (size_t)guest_argc + 1 + environment_count + 1 +
        (auxiliary_count + 1) * 2;
    if (word_count * sizeof(*words) > cursor - lower_bound) {
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
    for (i = 0; i < (size_t)guest_argc; ++i) {
        *words++ = argument_addresses[i];
    }
    *words++ = 0;
    for (i = 0; i < environment_count; ++i) {
        *words++ = environment_addresses[i];
    }
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

static int load_elf_object(const char *path, uintptr_t dynamic_bias,
    int allow_executable, struct elf_object *object,
    char *interpreter, size_t interpreter_capacity)
{
    Elf32_Ehdr elf_header;
    Elf32_Phdr *program_headers = 0;
    struct stat file_status;
    size_t headers_size;
    size_t i;
    int fd = -1;
    int result = -1;
    uintptr_t load_bias;

    memset(object, 0, sizeof(*object));
    if (interpreter != 0 && interpreter_capacity != 0) {
        interpreter[0] = '\0';
    }

    fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &file_status) != 0 ||
        read_exact_at(fd, &elf_header, sizeof(elf_header), 0) != 0) {
        goto done;
    }
    if (memcmp(elf_header.e_ident, ELFMAG, SELFMAG) != 0 ||
        elf_header.e_ident[EI_CLASS] != ELFCLASS32 ||
        elf_header.e_ident[EI_DATA] != ELFDATA2LSB ||
        elf_header.e_machine != EM_ARM ||
        (elf_header.e_type != ET_DYN &&
            !(allow_executable && elf_header.e_type == ET_EXEC)) ||
        elf_header.e_phentsize != sizeof(Elf32_Phdr) ||
        elf_header.e_phnum == 0 || elf_header.e_phnum > MAX_LOAD_SEGMENTS ||
        elf_header.e_phoff > (Elf32_Off)file_status.st_size) {
        errno = ENOEXEC;
        goto done;
    }
    load_bias = elf_header.e_type == ET_DYN ? dynamic_bias : 0;
    headers_size = (size_t)elf_header.e_phnum * sizeof(Elf32_Phdr);
    if (headers_size > (size_t)file_status.st_size - elf_header.e_phoff) {
        errno = ENOEXEC;
        goto done;
    }
    program_headers = malloc(headers_size);
    if (program_headers == 0 ||
        read_exact_at(fd, program_headers, headers_size,
            (off_t)elf_header.e_phoff) != 0) {
        goto done;
    }

    for (i = 0; i < elf_header.e_phnum; ++i) {
        if (program_headers[i].p_type == PT_INTERP) {
            if (interpreter == 0 || interpreter_capacity == 0 ||
                program_headers[i].p_filesz == 0 ||
                program_headers[i].p_filesz > interpreter_capacity ||
                read_exact_at(fd, interpreter, program_headers[i].p_filesz,
                    (off_t)program_headers[i].p_offset) != 0) {
                if (errno == 0) {
                    errno = ENOEXEC;
                }
                goto done;
            }
            if (interpreter[program_headers[i].p_filesz - 1] != '\0') {
                errno = ENOEXEC;
                goto done;
            }
        }
        if (program_headers[i].p_type == PT_LOAD &&
            program_headers[i].p_memsz != 0 &&
            map_load_segment(fd, &program_headers[i], load_bias) != 0) {
            goto done;
        }
    }
    if (load_bias > UINTPTR_MAX - elf_header.e_entry) {
        errno = ENOEXEC;
        goto done;
    }

    object->entry = load_bias + elf_header.e_entry;
    object->load_bias = load_bias;
    object->program_headers = 0;
    object->program_header_size = elf_header.e_phentsize;
    object->program_header_count = elf_header.e_phnum;
    for (i = 0; i < elf_header.e_phnum; ++i) {
        Elf32_Phdr *header = &program_headers[i];
        if (header->p_type == PT_LOAD &&
            elf_header.e_phoff >= header->p_offset &&
            elf_header.e_phoff - header->p_offset <= header->p_filesz &&
            headers_size <= header->p_filesz -
                (elf_header.e_phoff - header->p_offset)) {
            object->program_headers = load_bias + header->p_vaddr +
                (elf_header.e_phoff - header->p_offset);
            break;
        }
    }
    result = 0;
done:
    free(program_headers);
    if (fd >= 0) {
        close(fd);
    }
    return result;
}

static int load_guest(const char *path, struct guest_image *image)
{
    struct elf_object main_object;
    struct elf_object interpreter_object;
    char interpreter[PATH_MAX];
    char interpreter_path[PATH_MAX];
    const char *root;
    int length;

    memset(image, 0, sizeof(*image));
    if (load_elf_object(path, MAIN_ET_DYN_BIAS, 1, &main_object,
            interpreter, sizeof(interpreter)) != 0) {
        return -1;
    }

    image->entry = main_object.entry;
    image->start_entry = main_object.entry;
    image->program_headers = main_object.program_headers;
    image->program_header_size = main_object.program_header_size;
    image->program_header_count = main_object.program_header_count;

    if (interpreter[0] != '\0') {
        root = getenv("LINUXEMU_ROOT");
        if (root == 0 || root[0] == '\0' || interpreter[0] != '/') {
            errno = ENOENT;
            return -1;
        }
        length = snprintf(interpreter_path, sizeof(interpreter_path),
            "%s%s", root, interpreter);
        if (length < 0 || (size_t)length >= sizeof(interpreter_path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (load_elf_object(interpreter_path, INTERPRETER_BIAS, 0,
                &interpreter_object, 0, 0) != 0) {
            return -1;
        }
        image->start_entry = interpreter_object.entry;
        image->interpreter_base = interpreter_object.load_bias;
    }

    if (load_segment_count == 0 || patch_count == 0 ||
        image->entry < GUEST_MIN_ADDRESS ||
        image->entry >= GUEST_MAX_ADDRESS ||
        image->start_entry < GUEST_MIN_ADDRESS ||
        image->start_entry >= GUEST_MAX_ADDRESS ||
        !is_executable_address(image->entry) ||
        !is_executable_address(image->start_entry) ||
        finalize_segments() != 0) {
        if (errno == 0) {
            errno = ENOEXEC;
        }
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct guest_image image;
    uintptr_t stack_pointer;

    if (argc < 2) {
        fprintf(stderr, "usage: %s ARM_LINUX_ELF [ARG ...]\n", argv[0]);
        return 2;
    }
    host_page_size = sysconf(_SC_PAGESIZE);
    trace_syscalls = getenv("LINUXEMU_SYSTRACE") != 0;
    if (host_page_size <= 0 || install_syscall_handler() != 0) {
        perror("linuxemu initialization");
        return 1;
    }
    if (load_guest(argv[1], &image) != 0) {
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
        (unsigned)load_segment_count,
        (unsigned)patch_count,
        (void *)stack_pointer);
    fflush(stdout);
    linuxemu_enter_guest(image.start_entry, stack_pointer);
    return 126;
}
