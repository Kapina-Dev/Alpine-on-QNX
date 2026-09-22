#define _QNX_SOURCE 1
#include <sys/elf.h>
#include <errno.h>
#include <fcntl.h>
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
#define MAX_LOAD_SEGMENTS 16
#define MAX_PATCHES 256
#define GUEST_MIN_ADDRESS 0x00010000u
#define GUEST_MAX_ADDRESS 0x08000000u
#define GUEST_STACK_SIZE (1024u * 1024u)
#define ARM_LINUX_SVC_0 0xef000000u
#define ARM_UDF_TRAP 0xe7f000f0u
#define LINUX_ENOSYS 38
#define LINUX_NR_EXIT 1
#define LINUX_NR_WRITE 4
#define LINUX_NR_EXIT_GROUP 248

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

static int patch_arm_syscalls(uintptr_t start, size_t length)
{
    uintptr_t address;
    uintptr_t end = start + length;
    uint32_t *instruction;

    for (address = align_up(start, 4); address + 4 <= end; address += 4) {
        instruction = (uint32_t *)address;
        if (*instruction != ARM_LINUX_SVC_0) {
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

static int map_load_segment(int fd, const Elf32_Phdr *header)
{
    uintptr_t page = (uintptr_t)host_page_size;
    uintptr_t segment_start = (uintptr_t)header->p_vaddr;
    uintptr_t segment_end;
    uintptr_t map_start;
    uintptr_t map_end;
    size_t prefix;
    void *mapping;
    int final_protection;

    if (header->p_memsz < header->p_filesz ||
        header->p_vaddr + header->p_memsz < header->p_vaddr ||
        header->p_offset + header->p_filesz < header->p_offset) {
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
        patch_arm_syscalls(segment_start, header->p_filesz) != 0) {
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

static int is_patched_address(uintptr_t address)
{
    size_t i;

    for (i = 0; i < patch_count; ++i) {
        if (patches[i].address == address) {
            return 1;
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

static void syscall_handler(int sig, siginfo_t *info, void *argument)
{
    ucontext_t *context = argument;
    uintptr_t pc = context->uc_mcontext.cpu.gpr[15];
    uint32_t syscall_number;
    int32_t result;

    (void)info;
    if (sig != SIGILL || !is_patched_address(pc)) {
        _exit(125);
    }

    syscall_number = context->uc_mcontext.cpu.gpr[7];
    switch (syscall_number) {
    case LINUX_NR_WRITE:
        result = linux_result(write(
            (int)context->uc_mcontext.cpu.gpr[0],
            (const void *)context->uc_mcontext.cpu.gpr[1],
            (size_t)context->uc_mcontext.cpu.gpr[2]));
        context->uc_mcontext.cpu.gpr[0] = (uint32_t)result;
        break;
    case LINUX_NR_EXIT:
    case LINUX_NR_EXIT_GROUP:
        _exit((int)(context->uc_mcontext.cpu.gpr[0] & 0xffu));
        break;
    default:
        context->uc_mcontext.cpu.gpr[0] = (uint32_t)-LINUX_ENOSYS;
        break;
    }
    context->uc_mcontext.cpu.gpr[15] = (uint32_t)(pc + 4u);
}

static int install_syscall_handler(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_sigaction = syscall_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGILL, &action, 0);
}

static uintptr_t create_guest_stack(void)
{
    uintptr_t *stack_pointer;
    void *mapping;

    mapping = mmap(0, GUEST_STACK_SIZE, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON | MAP_STACK, -1, 0);
    if (mapping == MAP_FAILED) {
        return 0;
    }
    stack_pointer = (uintptr_t *)align_down(
        (uintptr_t)mapping + GUEST_STACK_SIZE, 16);

    *--stack_pointer = 0; /* AT_NULL value */
    *--stack_pointer = 0; /* AT_NULL type */
    *--stack_pointer = 0; /* envp terminator */
    *--stack_pointer = 0; /* argv terminator */
    *--stack_pointer = 0; /* argc */
    return (uintptr_t)stack_pointer;
}

static int load_guest(const char *path, uintptr_t *entry)
{
    Elf32_Ehdr elf_header;
    Elf32_Phdr *program_headers = 0;
    struct stat file_status;
    size_t headers_size;
    size_t i;
    int fd = -1;
    int result = -1;

    fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &file_status) != 0 ||
        read_exact_at(fd, &elf_header, sizeof(elf_header), 0) != 0) {
        goto done;
    }
    if (memcmp(elf_header.e_ident, ELFMAG, SELFMAG) != 0 ||
        elf_header.e_ident[EI_CLASS] != ELFCLASS32 ||
        elf_header.e_ident[EI_DATA] != ELFDATA2LSB ||
        elf_header.e_machine != EM_ARM ||
        elf_header.e_type != ET_EXEC ||
        elf_header.e_phentsize != sizeof(Elf32_Phdr) ||
        elf_header.e_phnum == 0 || elf_header.e_phnum > MAX_LOAD_SEGMENTS ||
        elf_header.e_phoff > (Elf32_Off)file_status.st_size) {
        errno = ENOEXEC;
        goto done;
    }
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
            errno = ENOTSUP;
            goto done;
        }
        if (program_headers[i].p_type == PT_LOAD &&
            program_headers[i].p_memsz != 0 &&
            map_load_segment(fd, &program_headers[i]) != 0) {
            goto done;
        }
    }
    if (load_segment_count == 0 || patch_count == 0 ||
        elf_header.e_entry < GUEST_MIN_ADDRESS ||
        elf_header.e_entry >= GUEST_MAX_ADDRESS ||
        !is_executable_address(elf_header.e_entry) ||
        finalize_segments() != 0) {
        if (errno == 0) {
            errno = ENOEXEC;
        }
        goto done;
    }

    *entry = elf_header.e_entry;
    result = 0;
done:
    free(program_headers);
    if (fd >= 0) {
        close(fd);
    }
    return result;
}

int main(int argc, char **argv)
{
    uintptr_t entry;
    uintptr_t stack_pointer;

    if (argc != 2) {
        fprintf(stderr, "usage: %s STATIC_ARM_LINUX_ELF\n", argv[0]);
        return 2;
    }
    host_page_size = sysconf(_SC_PAGESIZE);
    if (host_page_size <= 0 || install_syscall_handler() != 0) {
        perror("linuxemu initialization");
        return 1;
    }
    if (load_guest(argv[1], &entry) != 0) {
        perror("linuxemu load");
        return 1;
    }
    stack_pointer = create_guest_stack();
    if (stack_pointer == 0) {
        perror("linuxemu stack");
        return 1;
    }

    printf("linuxemu: entry=%p segments=%u patches=%u stack=%p\n",
        (void *)entry, (unsigned)load_segment_count, (unsigned)patch_count,
        (void *)stack_pointer);
    fflush(stdout);
    linuxemu_enter_guest(entry, stack_pointer);
    return 126;
}
