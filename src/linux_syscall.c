#include "linuxemu.h"

#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

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
#define LINUX_NR_EXIT_GROUP 248
#define LINUX_NR_SET_TID_ADDRESS 256
#define LINUX_ARM_NR_SET_TLS 0x000f0005u
#define LINUX_MAP_SHARED 0x00000001u
#define LINUX_MAP_PRIVATE 0x00000002u
#define LINUX_MAP_FIXED 0x00000010u
#define LINUX_MAP_ANONYMOUS 0x00000020u
#define LINUX_PROT_READ 0x1u
#define LINUX_PROT_WRITE 0x2u
#define LINUX_PROT_EXEC 0x4u

static size_t append_text(char *buffer, size_t position, const char *text)
{
    while (*text != '\0') buffer[position++] = *text++;
    return position;
}

static size_t append_hex32(char *buffer, size_t position, uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    int shift;
    for (shift = 28; shift >= 0; shift -= 4)
        buffer[position++] = digits[(value >> shift) & 15u];
    return position;
}

static void trace_call(uint32_t number, const ucontext_t *context)
{
    char message[160];
    size_t length = 0;
    unsigned i;

    if (!runtime_trace_enabled()) return;
    length = append_text(message, length, "linuxemu: syscall=0x");
    length = append_hex32(message, length, number);
    for (i = 0; i != 6; ++i) {
        length = append_text(message, length, " r");
        message[length++] = (char)('0' + i);
        length = append_text(message, length, "=0x");
        length = append_hex32(message, length, context->uc_mcontext.cpu.gpr[i]);
    }
    message[length++] = '\n';
    write(STDERR_FILENO, message, length);
}

static void trace_result(uint32_t result)
{
    char message[40];
    size_t length = 0;
    if (!runtime_trace_enabled()) return;
    length = append_text(message, length, "linuxemu: result=0x");
    length = append_hex32(message, length, result);
    message[length++] = '\n';
    write(STDERR_FILENO, message, length);
}

static int32_t linux_result(ssize_t result)
{
    return result < 0 ? -(int32_t)errno : (int32_t)result;
}

static int translate_protection(uint32_t linux_protection,
    int *host_protection)
{
    if ((linux_protection &
            ~(LINUX_PROT_READ | LINUX_PROT_WRITE | LINUX_PROT_EXEC)) != 0)
        return -1;
    *host_protection = PROT_NONE;
    if ((linux_protection & LINUX_PROT_READ) != 0) *host_protection |= PROT_READ;
    if ((linux_protection & LINUX_PROT_WRITE) != 0) *host_protection |= PROT_WRITE;
    if ((linux_protection & LINUX_PROT_EXEC) != 0) *host_protection |= PROT_EXEC;
    return 0;
}

static int32_t linux_mmap2(ucontext_t *context)
{
    uint32_t linux_flags = context->uc_mcontext.cpu.gpr[3];
    int host_flags = 0;
    int host_protection;
    void *result;
    off_t offset;

    if (translate_protection(context->uc_mcontext.cpu.gpr[2],
            &host_protection) != 0) return -EINVAL;
    if ((linux_flags & LINUX_MAP_SHARED) != 0) host_flags |= MAP_SHARED;
    else if ((linux_flags & LINUX_MAP_PRIVATE) != 0) host_flags |= MAP_PRIVATE;
    else return -EINVAL;
    if ((linux_flags & LINUX_MAP_FIXED) != 0) host_flags |= MAP_FIXED;
    if ((linux_flags & LINUX_MAP_ANONYMOUS) != 0) host_flags |= MAP_ANON;
    offset = (off_t)context->uc_mcontext.cpu.gpr[5] * 4096;
    result = mmap((void *)context->uc_mcontext.cpu.gpr[0],
        (size_t)context->uc_mcontext.cpu.gpr[1], host_protection, host_flags,
        (int)context->uc_mcontext.cpu.gpr[4], offset);
    return result == MAP_FAILED ? -(int32_t)errno : (int32_t)(uintptr_t)result;
}

void linux_syscall_dispatch(ucontext_t *context)
{
    uint32_t number = context->uc_mcontext.cpu.gpr[7];
    int32_t result;

    trace_call(number, context);
    switch (number) {
    case LINUX_NR_WRITE:
        result = linux_result(write((int)context->uc_mcontext.cpu.gpr[0],
            (const void *)context->uc_mcontext.cpu.gpr[1],
            (size_t)context->uc_mcontext.cpu.gpr[2]));
        break;
    case LINUX_NR_MMAP2:
        result = linux_mmap2(context);
        break;
    case LINUX_NR_MPROTECT:
        if (translate_protection(context->uc_mcontext.cpu.gpr[2], &result) != 0)
            result = -EINVAL;
        else if (mprotect((void *)context->uc_mcontext.cpu.gpr[0],
                (size_t)context->uc_mcontext.cpu.gpr[1], result) != 0)
            result = -(int32_t)errno;
        else result = 0;
        break;
    case LINUX_NR_GETPID:
    case LINUX_NR_SET_TID_ADDRESS:
        result = (int32_t)getpid();
        break;
    case LINUX_NR_GETUID32: result = (int32_t)getuid(); break;
    case LINUX_NR_GETGID32: result = (int32_t)getgid(); break;
    case LINUX_NR_GETEUID32: result = (int32_t)geteuid(); break;
    case LINUX_NR_GETEGID32: result = (int32_t)getegid(); break;
    case LINUX_NR_EXIT:
    case LINUX_NR_EXIT_GROUP:
        _exit((int)(context->uc_mcontext.cpu.gpr[0] & 0xffu));
        return;
    case LINUX_ARM_NR_SET_TLS:
        runtime_set_guest_tls(context->uc_mcontext.cpu.gpr[0]);
        result = 0;
        break;
    default:
        result = -LINUX_ENOSYS;
        break;
    }
    context->uc_mcontext.cpu.gpr[0] = (uint32_t)result;
    trace_result((uint32_t)result);
}
