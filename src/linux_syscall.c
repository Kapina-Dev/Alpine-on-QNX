#include "linuxemu.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <unistd.h>

#define LINUX_ENOSYS 38
#define LINUX_NR_EXIT 1
#define LINUX_NR_FORK 2
#define LINUX_NR_READ 3
#define LINUX_NR_WRITE 4
#define LINUX_NR_OPEN 5
#define LINUX_NR_CLOSE 6
#define LINUX_NR_EXECVE 11
#define LINUX_NR_CHDIR 12
#define LINUX_NR_LSEEK 19
#define LINUX_NR_GETPID 20
#define LINUX_NR_ACCESS 33
#define LINUX_NR_PIPE 42
#define LINUX_NR_DUP 41
#define LINUX_NR_BRK 45
#define LINUX_NR_DUP2 63
#define LINUX_NR_GETPPID 64
#define LINUX_NR_GETPGRP 65
#define LINUX_NR_SETSID 66
#define LINUX_NR_READLINK 85
#define LINUX_NR_MUNMAP 91
#define LINUX_NR_WAIT4 114
#define LINUX_NR_CLONE 120
#define LINUX_NR_UNAME 122
#define LINUX_NR_FCHDIR 133
#define LINUX_NR_LLSEEK 140
#define LINUX_NR_WRITEV 146
#define LINUX_NR_RT_SIGACTION 174
#define LINUX_NR_RT_SIGPROCMASK 175
#define LINUX_NR_RT_SIGSUSPEND 179
#define LINUX_NR_MPROTECT 125
#define LINUX_NR_GETCWD 183
#define LINUX_NR_VFORK 190
#define LINUX_NR_MMAP2 192
#define LINUX_NR_STAT64 195
#define LINUX_NR_LSTAT64 196
#define LINUX_NR_FSTAT64 197
#define LINUX_NR_GETUID32 199
#define LINUX_NR_GETGID32 200
#define LINUX_NR_GETEUID32 201
#define LINUX_NR_GETEGID32 202
#define LINUX_NR_SETUID32 213
#define LINUX_NR_SETGID32 214
#define LINUX_NR_EXIT_GROUP 248
#define LINUX_NR_SET_TID_ADDRESS 256
#define LINUX_NR_GETDENTS64 217
#define LINUX_NR_FCNTL64 221
#define LINUX_NR_OPENAT 322
#define LINUX_NR_FSTATAT64 327
#define LINUX_NR_READLINKAT 332
#define LINUX_NR_DUP3 358
#define LINUX_NR_PIPE2 359
#define LINUX_ARM_NR_SET_TLS 0x000f0005u
#define LINUX_MAP_SHARED 0x00000001u
#define LINUX_MAP_PRIVATE 0x00000002u
#define LINUX_MAP_FIXED 0x00000010u
#define LINUX_MAP_ANONYMOUS 0x00000020u
#define LINUX_MAP_KNOWN (LINUX_MAP_SHARED | LINUX_MAP_PRIVATE | \
    LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS)
#define LINUX_PROT_READ 0x1u
#define LINUX_PROT_WRITE 0x2u
#define LINUX_PROT_EXEC 0x4u
#define LINUX_AT_FDCWD (-100)
#define LINUX_AT_SYMLINK_NOFOLLOW 0x100u
#define LINUX_O_CREAT 0x00000040u
#define LINUX_O_EXCL 0x00000080u
#define LINUX_O_NOFOLLOW 0x00008000u
#define LINUX_O_NONBLOCK 0x00000800u
#define LINUX_O_CLOEXEC 0x00080000u
#define LINUX_WNOHANG 0x00000001u
#define LINUX_WUNTRACED 0x00000002u
#define LINUX_WCONTINUED 0x00000008u
#define MAX_TRACKED_FDS 256

static DIR *directory_streams[MAX_TRACKED_FDS];
static char *fd_paths[MAX_TRACKED_FDS];

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
    return result < 0 ? -(int32_t)linux_errno_number(errno) : (int32_t)result;
}

static int32_t linux_host_result(int result)
{
    return result < 0 ? -(int32_t)linux_errno_number(errno) : result;
}

static int directory_path(int directory_fd, const char *guest_path,
    const char **host_directory)
{
    if ((guest_path != 0 && guest_path[0] == '/') ||
        directory_fd == LINUX_AT_FDCWD) {
        *host_directory = 0;
        return 0;
    }
    if (directory_fd < 0 || directory_fd >= MAX_TRACKED_FDS ||
        fd_paths[directory_fd] == 0) {
        errno = EBADF;
        return -1;
    }
    *host_directory = fd_paths[directory_fd];
    return 0;
}

static int resolve_at(int directory_fd, const char *guest_path,
    int allow_missing_leaf, int nofollow, char *host_path, size_t host_path_size)
{
    const char *base;
    if (directory_path(directory_fd, guest_path, &base) != 0) return -1;
    if (base != 0) return guest_path_resolve_at(base, guest_path,
        allow_missing_leaf, nofollow, host_path, host_path_size);
    return nofollow ? guest_path_resolve_nofollow(guest_path, host_path,
        host_path_size) : guest_path_resolve(guest_path, allow_missing_leaf,
        host_path, host_path_size);
}

static int32_t linux_open_path(int directory_fd, const char *guest_path,
    uint32_t flags, uint32_t mode)
{
    char host_path[PATH_MAX];
    int host_flags;
    int fd;

    if (linux_open_flags(flags, &host_flags) != 0) return -EINVAL;
    if (resolve_at(directory_fd, guest_path, (host_flags & O_CREAT) != 0,
            (flags & LINUX_O_NOFOLLOW) != 0 ||
            (flags & (LINUX_O_CREAT | LINUX_O_EXCL)) ==
                (LINUX_O_CREAT | LINUX_O_EXCL), host_path,
            sizeof(host_path)) != 0)
        return -(int32_t)linux_errno_number(errno);
    fd = open(host_path, host_flags, (mode_t)(mode & 07777u));
    if (fd >= 0 && fd < MAX_TRACKED_FDS) {
        free(fd_paths[fd]);
        fd_paths[fd] = strdup(host_path);
    }
    return linux_host_result(fd);
}

static int32_t linux_stat_path(int directory_fd, const char *guest_path,
    void *guest_status, int follow)
{
    char host_path[PATH_MAX];
    struct stat status;
    int result;

    if (resolve_at(directory_fd, guest_path, 0, !follow, host_path,
            sizeof(host_path)) != 0)
        return -(int32_t)linux_errno_number(errno);
    result = follow ? stat(host_path, &status) : lstat(host_path, &status);
    if (result != 0) return -(int32_t)linux_errno_number(errno);
    linux_stat64_store(guest_status, &status);
    return 0;
}

static int32_t linux_fstat_fd(int fd, void *guest_status)
{
    struct stat status;
    if (fstat(fd, &status) != 0) return -(int32_t)linux_errno_number(errno);
    linux_stat64_store(guest_status, &status);
    return 0;
}

static int32_t linux_getdents64(int fd, void *guest_buffer, size_t capacity)
{
    unsigned char *buffer = guest_buffer;
    size_t used = 0;
    DIR *directory;
    struct dirent *entry;

    if (fd < 0 || fd >= MAX_TRACKED_FDS) return -EBADF;
    directory = directory_streams[fd];
    if (directory == 0) {
        if (fd_paths[fd] == 0) return -EBADF;
        directory = opendir(fd_paths[fd]);
        if (directory == 0) return -(int32_t)linux_errno_number(errno);
        directory_streams[fd] = directory;
    }
    errno = 0;
    for (;;) {
        long previous_offset = telldir(directory);
        entry = readdir(directory);
        if (entry == 0) break;
        size_t name_length = strlen(entry->d_name) + 1;
        size_t record_length = (19 + name_length + 7) & ~(size_t)7;
        uint64_t inode = (uint64_t)entry->d_ino;
        int64_t offset = (int64_t)telldir(directory);
        uint16_t short_length = (uint16_t)record_length;
        if (record_length > capacity - used) {
            seekdir(directory, previous_offset);
            break;
        }
        memset(buffer + used, 0, record_length);
        memcpy(buffer + used, &inode, 8);
        memcpy(buffer + used + 8, &offset, 8);
        memcpy(buffer + used + 16, &short_length, 2);
        buffer[used + 18] = 0;
        memcpy(buffer + used + 19, entry->d_name, name_length);
        used += record_length;
        errno = 0;
    }
    if (used == 0 && errno != 0) return -(int32_t)linux_errno_number(errno);
    return (int32_t)used;
}

static int32_t linux_close_fd(int fd)
{
    if (fd >= 0 && fd < MAX_TRACKED_FDS && directory_streams[fd] != 0) {
        closedir(directory_streams[fd]);
        directory_streams[fd] = 0;
    }
    if (fd >= 0 && fd < MAX_TRACKED_FDS) {
        free(fd_paths[fd]);
        fd_paths[fd] = 0;
    }
    return linux_host_result(close(fd));
}

static void copy_fd_path(int source, int destination)
{
    if (destination < 0 || destination >= MAX_TRACKED_FDS) return;
    if (directory_streams[destination] != 0) {
        closedir(directory_streams[destination]);
        directory_streams[destination] = 0;
    }
    free(fd_paths[destination]);
    fd_paths[destination] = source >= 0 && source < MAX_TRACKED_FDS &&
        fd_paths[source] != 0 ? strdup(fd_paths[source]) : 0;
}

static int32_t linux_fcntl64(int fd, int command, uint32_t argument)
{
    int result;
    switch (command) {
    case 0: result = fcntl(fd, F_DUPFD, (int)argument); break;
    case 1: result = fcntl(fd, F_GETFD); break;
    case 2: result = fcntl(fd, F_SETFD,
        (argument & 1u) != 0 ? FD_CLOEXEC : 0); break;
    case 3:
        result = fcntl(fd, F_GETFL);
        if (result >= 0) return (int32_t)linux_status_flags(result);
        break;
    case 4: {
        int requested;
        int current = fcntl(fd, F_GETFL);
        if (current < 0) return linux_host_result(current);
        if (linux_open_flags(argument & (0x00000400u | 0x00000800u),
                &requested) != 0) return -EINVAL;
        current &= ~(O_APPEND | O_NONBLOCK);
        current |= requested & (O_APPEND | O_NONBLOCK);
        result = fcntl(fd, F_SETFL, current);
        break;
    }
    default: return -EINVAL;
    }
    if (command == 0 && result >= 0) copy_fd_path(fd, result);
    return linux_host_result(result);
}

static int32_t linux_pipe(void *guest_descriptors, uint32_t flags)
{
    int descriptors[2];
    int host_status_flags = 0;

    if ((flags & ~(LINUX_O_NONBLOCK | LINUX_O_CLOEXEC)) != 0) return -EINVAL;
    if (pipe(descriptors) != 0) return -(int32_t)linux_errno_number(errno);
    if ((flags & LINUX_O_NONBLOCK) != 0) host_status_flags |= O_NONBLOCK;
    if ((host_status_flags != 0 &&
            (fcntl(descriptors[0], F_SETFL, host_status_flags) != 0 ||
            fcntl(descriptors[1], F_SETFL, host_status_flags) != 0)) ||
        ((flags & LINUX_O_CLOEXEC) != 0 &&
            (fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) != 0 ||
            fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) != 0))) {
        int saved_errno = errno;
        close(descriptors[0]);
        close(descriptors[1]);
        errno = saved_errno;
        return -(int32_t)linux_errno_number(errno);
    }
    copy_fd_path(-1, descriptors[0]);
    copy_fd_path(-1, descriptors[1]);
    memcpy(guest_descriptors, descriptors, sizeof(descriptors));
    return 0;
}

static int32_t linux_execve(const char *guest_path, char *const guest_argv[],
    char *const guest_envp[])
{
    char host_path[PATH_MAX];

    if (guest_path_resolve(guest_path, 0, host_path, sizeof(host_path)) != 0)
        return -(int32_t)linux_errno_number(errno);
    guest_process_exec(host_path, guest_argv, guest_envp);
    return -(int32_t)linux_errno_number(errno);
}

static int host_signal_to_linux(int signal_number)
{
    switch (signal_number) {
    case 1: case 2: case 3: case 4: case 5: case 6: case 8: case 9:
    case 11: case 13: case 14: case 15: return signal_number;
    case 10: return 7;
    case 12: return 31;
    case 16: return 10;
    case 17: return 12;
    case 18: return 17;
    case 19: return 30;
    case 20: return 28;
    case 21: return 23;
    case 22: return 29;
    case 23: return 19;
    case 24: return 20;
    case 25: return 18;
    case 26: return 21;
    case 27: return 22;
    case 28: return 26;
    case 29: return 27;
    case 30: return 24;
    case 31: return 25;
    default: return signal_number;
    }
}

static int linux_wait_status(int host_status)
{
    if (WIFSIGNALED(host_status))
        return host_signal_to_linux(WTERMSIG(host_status)) |
            (host_status & 0x80);
    if (WIFSTOPPED(host_status))
        return (host_signal_to_linux(WSTOPSIG(host_status)) << 8) | 0x7f;
    return host_status;
}

static int32_t linux_wait4(int pid, int *guest_status, uint32_t linux_options,
    void *guest_usage)
{
    struct rusage usage;
    int host_options = 0;
    int host_status;
    pid_t result;

    if ((linux_options &
            ~(LINUX_WNOHANG | LINUX_WUNTRACED | LINUX_WCONTINUED)) != 0)
        return -EINVAL;
    if ((linux_options & LINUX_WNOHANG) != 0) host_options |= WNOHANG;
    if ((linux_options & LINUX_WUNTRACED) != 0) host_options |= WUNTRACED;
#ifdef WCONTINUED
    if ((linux_options & LINUX_WCONTINUED) != 0) host_options |= WCONTINUED;
#else
    if ((linux_options & LINUX_WCONTINUED) != 0) return -EINVAL;
#endif
    result = waitpid((pid_t)pid, &host_status, host_options);
    if (result < 0) return -(int32_t)linux_errno_number(errno);
    if (result != 0 && guest_status != 0)
        *guest_status = linux_wait_status(host_status);
    if (result != 0 && guest_usage != 0) {
        if (getrusage(RUSAGE_CHILDREN, &usage) != 0)
            return -(int32_t)linux_errno_number(errno);
        linux_rusage_store(guest_usage, &usage);
    }
    return (int32_t)result;
}

static int32_t linux_uname(void *guest_buffer)
{
    struct utsname host;
    unsigned char *buffer = guest_buffer;
    const char *fields[6];
    size_t i;

    if (uname(&host) != 0) return -(int32_t)linux_errno_number(errno);
    fields[0] = "Linux";
    fields[1] = host.nodename;
    fields[2] = "5.15.0-linuxemu";
    fields[3] = "Linuxemu on BlackBerry 10/QNX";
    fields[4] = "armv7l";
    fields[5] = "";
    memset(buffer, 0, 65 * 6);
    for (i = 0; i != 6; ++i)
        strncpy((char *)buffer + i * 65, fields[i], 64);
    return 0;
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
    if ((linux_flags & ~LINUX_MAP_KNOWN) != 0) return -EINVAL;
    if ((linux_flags & (LINUX_MAP_SHARED | LINUX_MAP_PRIVATE)) ==
            (LINUX_MAP_SHARED | LINUX_MAP_PRIVATE)) return -EINVAL;
    if ((linux_flags & LINUX_MAP_SHARED) != 0) host_flags |= MAP_SHARED;
    else if ((linux_flags & LINUX_MAP_PRIVATE) != 0) host_flags |= MAP_PRIVATE;
    else return -EINVAL;
    if ((linux_flags & LINUX_MAP_FIXED) != 0) host_flags |= MAP_FIXED;
    if ((linux_flags & LINUX_MAP_ANONYMOUS) != 0) host_flags |= MAP_ANON;
    offset = (off_t)context->uc_mcontext.cpu.gpr[5] * 4096;
    result = mmap((void *)context->uc_mcontext.cpu.gpr[0],
        (size_t)context->uc_mcontext.cpu.gpr[1], host_protection, host_flags,
        (int)context->uc_mcontext.cpu.gpr[4], offset);
    return result == MAP_FAILED ?
        -(int32_t)linux_errno_number(errno) : (int32_t)(uintptr_t)result;
}

void linux_syscall_dispatch(ucontext_t *context)
{
    uint32_t number = context->uc_mcontext.cpu.gpr[7];
    int32_t result;

    trace_call(number, context);
    switch (number) {
    case LINUX_NR_FORK:
        result = linux_host_result(fork());
        break;
    case LINUX_NR_READ:
        result = linux_result(read((int)context->uc_mcontext.cpu.gpr[0],
            (void *)context->uc_mcontext.cpu.gpr[1],
            (size_t)context->uc_mcontext.cpu.gpr[2]));
        break;
    case LINUX_NR_WRITE:
        result = linux_result(write((int)context->uc_mcontext.cpu.gpr[0],
            (const void *)context->uc_mcontext.cpu.gpr[1],
            (size_t)context->uc_mcontext.cpu.gpr[2]));
        break;
    case LINUX_NR_WRITEV:
        result = linux_result(writev((int)context->uc_mcontext.cpu.gpr[0],
            (const struct iovec *)context->uc_mcontext.cpu.gpr[1],
            (int)context->uc_mcontext.cpu.gpr[2]));
        break;
    case LINUX_NR_OPEN:
        result = linux_open_path(LINUX_AT_FDCWD,
            (const char *)context->uc_mcontext.cpu.gpr[0],
            context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2]);
        break;
    case LINUX_NR_OPENAT:
        result = linux_open_path((int32_t)context->uc_mcontext.cpu.gpr[0],
            (const char *)context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2],
            context->uc_mcontext.cpu.gpr[3]);
        break;
    case LINUX_NR_CLOSE:
        result = linux_close_fd((int)context->uc_mcontext.cpu.gpr[0]);
        break;
    case LINUX_NR_EXECVE:
        result = linux_execve((const char *)context->uc_mcontext.cpu.gpr[0],
            (char *const *)context->uc_mcontext.cpu.gpr[1],
            (char *const *)context->uc_mcontext.cpu.gpr[2]);
        break;
    case LINUX_NR_CHDIR:
        result = guest_path_chdir(
            (const char *)context->uc_mcontext.cpu.gpr[0]) == 0 ? 0 :
            -(int32_t)linux_errno_number(errno);
        break;
    case LINUX_NR_LSEEK:
        result = linux_result(lseek((int)context->uc_mcontext.cpu.gpr[0],
            (off_t)(int32_t)context->uc_mcontext.cpu.gpr[1],
            (int)context->uc_mcontext.cpu.gpr[2]));
        break;
    case LINUX_NR_LLSEEK: {
        uint64_t unsigned_offset =
            ((uint64_t)context->uc_mcontext.cpu.gpr[1] << 32) |
            context->uc_mcontext.cpu.gpr[2];
        off_t offset = lseek((int)context->uc_mcontext.cpu.gpr[0],
            (off_t)unsigned_offset, (int)context->uc_mcontext.cpu.gpr[4]);
        if (offset == (off_t)-1) result = -linux_errno_number(errno);
        else {
            uint64_t stored = (uint64_t)offset;
            memcpy((void *)context->uc_mcontext.cpu.gpr[3], &stored, 8);
            result = 0;
        }
        break;
    }
    case LINUX_NR_DUP:
        result = linux_host_result(dup((int)context->uc_mcontext.cpu.gpr[0]));
        if (result >= 0) copy_fd_path((int)context->uc_mcontext.cpu.gpr[0],
            result);
        break;
    case LINUX_NR_DUP2:
        result = linux_host_result(dup2((int)context->uc_mcontext.cpu.gpr[0],
            (int)context->uc_mcontext.cpu.gpr[1]));
        if (result >= 0 &&
            (int)context->uc_mcontext.cpu.gpr[0] != result)
            copy_fd_path((int)context->uc_mcontext.cpu.gpr[0], result);
        break;
    case LINUX_NR_ACCESS: {
        char host_path[PATH_MAX];
        if (guest_path_resolve((const char *)context->uc_mcontext.cpu.gpr[0],
                0, host_path, sizeof(host_path)) != 0)
            result = -(int32_t)linux_errno_number(errno);
        else result = linux_host_result(access(host_path,
            (int)context->uc_mcontext.cpu.gpr[1]));
        break;
    }
    case LINUX_NR_PIPE:
        result = linux_pipe((void *)context->uc_mcontext.cpu.gpr[0], 0);
        break;
    case LINUX_NR_GETPPID:
        result = (int32_t)getppid();
        break;
    case LINUX_NR_GETPGRP:
        result = (int32_t)getpgrp();
        break;
    case LINUX_NR_SETSID:
        result = linux_host_result(setsid());
        break;
    case LINUX_NR_BRK:
        result = (int32_t)guest_brk_set(context->uc_mcontext.cpu.gpr[0]);
        break;
    case LINUX_NR_MMAP2:
        result = linux_mmap2(context);
        break;
    case LINUX_NR_MPROTECT:
        if (translate_protection(context->uc_mcontext.cpu.gpr[2], &result) != 0)
            result = -EINVAL;
        else if (mprotect((void *)context->uc_mcontext.cpu.gpr[0],
                (size_t)context->uc_mcontext.cpu.gpr[1], result) != 0)
            result = -(int32_t)linux_errno_number(errno);
        else result = 0;
        break;
    case LINUX_NR_MUNMAP:
        result = linux_host_result(munmap(
            (void *)context->uc_mcontext.cpu.gpr[0],
            (size_t)context->uc_mcontext.cpu.gpr[1]));
        break;
    case LINUX_NR_WAIT4:
        result = linux_wait4((int32_t)context->uc_mcontext.cpu.gpr[0],
            (int *)context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2],
            (void *)context->uc_mcontext.cpu.gpr[3]);
        break;
    case LINUX_NR_CLONE:
        if ((context->uc_mcontext.cpu.gpr[0] & ~0xffu) != 0 ||
            ((context->uc_mcontext.cpu.gpr[0] & 0xffu) != 0 &&
            (context->uc_mcontext.cpu.gpr[0] & 0xffu) != 17u))
            result = -EINVAL;
        else result = linux_host_result(fork());
        break;
    case LINUX_NR_UNAME:
        result = linux_uname((void *)context->uc_mcontext.cpu.gpr[0]);
        break;
    case LINUX_NR_FCHDIR: {
        int fd = (int)context->uc_mcontext.cpu.gpr[0];
        if (fd < 0 || fd >= MAX_TRACKED_FDS || fd_paths[fd] == 0)
            result = -EBADF;
        else result = guest_path_fchdir(fd_paths[fd]) == 0 ? 0 :
            -(int32_t)linux_errno_number(errno);
        break;
    }
    case LINUX_NR_GETCWD:
        result = guest_path_getcwd(
            (char *)context->uc_mcontext.cpu.gpr[0],
            (size_t)context->uc_mcontext.cpu.gpr[1]);
        if (result < 0) result = -linux_errno_number(errno);
        break;
    case LINUX_NR_VFORK:
        result = linux_host_result(fork());
        break;
    case LINUX_NR_STAT64:
        result = linux_stat_path(LINUX_AT_FDCWD,
            (const char *)context->uc_mcontext.cpu.gpr[0],
            (void *)context->uc_mcontext.cpu.gpr[1], 1);
        break;
    case LINUX_NR_LSTAT64:
        result = linux_stat_path(LINUX_AT_FDCWD,
            (const char *)context->uc_mcontext.cpu.gpr[0],
            (void *)context->uc_mcontext.cpu.gpr[1], 0);
        break;
    case LINUX_NR_FSTAT64:
        result = linux_fstat_fd((int)context->uc_mcontext.cpu.gpr[0],
            (void *)context->uc_mcontext.cpu.gpr[1]);
        break;
    case LINUX_NR_FSTATAT64:
        if ((context->uc_mcontext.cpu.gpr[3] &
                ~LINUX_AT_SYMLINK_NOFOLLOW) != 0)
            result = -EINVAL;
        else result = linux_stat_path(
            (int32_t)context->uc_mcontext.cpu.gpr[0],
            (const char *)context->uc_mcontext.cpu.gpr[1],
            (void *)context->uc_mcontext.cpu.gpr[2],
            (context->uc_mcontext.cpu.gpr[3] &
                LINUX_AT_SYMLINK_NOFOLLOW) == 0);
        break;
    case LINUX_NR_READLINK:
        result = guest_path_readlink(
            (const char *)context->uc_mcontext.cpu.gpr[0],
            (char *)context->uc_mcontext.cpu.gpr[1],
            (size_t)context->uc_mcontext.cpu.gpr[2]);
        if (result < 0) result = -linux_errno_number(errno);
        break;
    case LINUX_NR_READLINKAT:
        {
            char host_path[PATH_MAX];
            if (resolve_at((int32_t)context->uc_mcontext.cpu.gpr[0],
                    (const char *)context->uc_mcontext.cpu.gpr[1], 0, 1,
                    host_path, sizeof(host_path)) != 0)
                result = -linux_errno_number(errno);
            else {
                result = (int32_t)readlink(host_path,
                    (char *)context->uc_mcontext.cpu.gpr[2],
                    (size_t)context->uc_mcontext.cpu.gpr[3]);
                if (result < 0) result = -linux_errno_number(errno);
            }
        }
        break;
    case LINUX_NR_GETDENTS64:
        result = linux_getdents64((int)context->uc_mcontext.cpu.gpr[0],
            (void *)context->uc_mcontext.cpu.gpr[1],
            (size_t)context->uc_mcontext.cpu.gpr[2]);
        break;
    case LINUX_NR_FCNTL64:
        result = linux_fcntl64((int)context->uc_mcontext.cpu.gpr[0],
            (int)context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2]);
        break;
    case LINUX_NR_RT_SIGACTION:
        result = guest_signal_action(
            (int)context->uc_mcontext.cpu.gpr[0],
            (const void *)context->uc_mcontext.cpu.gpr[1],
            (void *)context->uc_mcontext.cpu.gpr[2],
            (size_t)context->uc_mcontext.cpu.gpr[3]);
        break;
    case LINUX_NR_RT_SIGPROCMASK:
        result = guest_signal_mask(
            (int)context->uc_mcontext.cpu.gpr[0],
            (const void *)context->uc_mcontext.cpu.gpr[1],
            (void *)context->uc_mcontext.cpu.gpr[2],
            (size_t)context->uc_mcontext.cpu.gpr[3]);
        break;
    case LINUX_NR_RT_SIGSUSPEND:
        result = guest_signal_suspend(
            (const void *)context->uc_mcontext.cpu.gpr[0],
            (size_t)context->uc_mcontext.cpu.gpr[1]);
        break;
    case LINUX_NR_GETPID:
    case LINUX_NR_SET_TID_ADDRESS:
        result = (int32_t)getpid();
        break;
    case LINUX_NR_GETUID32: result = (int32_t)getuid(); break;
    case LINUX_NR_GETGID32: result = (int32_t)getgid(); break;
    case LINUX_NR_GETEUID32: result = (int32_t)geteuid(); break;
    case LINUX_NR_GETEGID32: result = (int32_t)getegid(); break;
    case LINUX_NR_SETUID32:
        result = linux_host_result(setuid(
            (uid_t)context->uc_mcontext.cpu.gpr[0]));
        break;
    case LINUX_NR_SETGID32:
        result = linux_host_result(setgid(
            (gid_t)context->uc_mcontext.cpu.gpr[0]));
        break;
    case LINUX_NR_DUP3: {
        int source = (int)context->uc_mcontext.cpu.gpr[0];
        int destination = (int)context->uc_mcontext.cpu.gpr[1];
        uint32_t flags = context->uc_mcontext.cpu.gpr[2];
        if (source == destination || (flags & ~LINUX_O_CLOEXEC) != 0)
            result = -EINVAL;
        else {
            result = linux_host_result(dup2(source, destination));
            if (result >= 0 && (flags & LINUX_O_CLOEXEC) != 0 &&
                fcntl(destination, F_SETFD, FD_CLOEXEC) != 0)
                result = -(int32_t)linux_errno_number(errno);
            if (result >= 0) copy_fd_path(source, destination);
        }
        break;
    }
    case LINUX_NR_PIPE2:
        result = linux_pipe((void *)context->uc_mcontext.cpu.gpr[0],
            context->uc_mcontext.cpu.gpr[1]);
        break;
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
