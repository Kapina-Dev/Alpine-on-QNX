#include "linuxemu.h"
#include "linux_diagnostics.h"
#include "guest_fd.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <utime.h>
#include <unistd.h>

#define LINUX_ENOSYS 38
#define LINUX_EINVAL 22
#define LINUX_EOPNOTSUPP 95
#define LINUX_NR_EXIT 1
#define LINUX_NR_FORK 2
#define LINUX_NR_READ 3
#define LINUX_NR_WRITE 4
#define LINUX_NR_OPEN 5
#define LINUX_NR_CLOSE 6
#define LINUX_NR_LINK 9
#define LINUX_NR_UNLINK 10
#define LINUX_NR_EXECVE 11
#define LINUX_NR_CHDIR 12
#define LINUX_NR_CHMOD 15
#define LINUX_NR_LSEEK 19
#define LINUX_NR_GETPID 20
#define LINUX_NR_ACCESS 33
#define LINUX_NR_KILL 37
#define LINUX_NR_RENAME 38
#define LINUX_NR_MKDIR 39
#define LINUX_NR_RMDIR 40
#define LINUX_NR_IOCTL 54
#define LINUX_NR_UMASK 60
#define LINUX_NR_SETPGID 57
#define LINUX_NR_SELECT 82
#define LINUX_NR_GETTIMEOFDAY 78
#define LINUX_NR_TIME 13
#define LINUX_NR_PIPE 42
#define LINUX_NR_DUP 41
#define LINUX_NR_BRK 45
#define LINUX_NR_DUP2 63
#define LINUX_NR_GETPPID 64
#define LINUX_NR_GETPGRP 65
#define LINUX_NR_SETSID 66
#define LINUX_NR_READLINK 85
#define LINUX_NR_SYMLINK 83
#define LINUX_NR_MUNMAP 91
#define LINUX_NR_SOCKETCALL 102
#define LINUX_NR_WAIT4 114
#define LINUX_NR_FSYNC 118
#define LINUX_NR_CLONE 120
#define LINUX_NR_UNAME 122
#define LINUX_NR_FCHDIR 133
#define LINUX_NR_GETPGID 132
#define LINUX_NR_LLSEEK 140
#define LINUX_NR_FLOCK 143
#define LINUX_NR_READV 145
#define LINUX_NR_WRITEV 146
#define LINUX_NR_FDATASYNC 148
#define LINUX_NR_NEWSELECT 142
#define LINUX_NR_NANOSLEEP 162
#define LINUX_NR_POLL 168
#define LINUX_NR_RT_SIGACTION 174
#define LINUX_NR_RT_SIGPROCMASK 175
#define LINUX_NR_RT_SIGSUSPEND 179
#define LINUX_NR_PREAD64 180
#define LINUX_NR_PWRITE64 181
#define LINUX_NR_SIGALTSTACK 186
#define LINUX_NR_MPROTECT 125
#define LINUX_NR_GETCWD 183
#define LINUX_NR_VFORK 190
#define LINUX_NR_MMAP2 192
#define LINUX_NR_FTRUNCATE64 194
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
#define LINUX_NR_GETTID 224
#define LINUX_NR_TKILL 238
#define LINUX_NR_FCNTL64 221
#define LINUX_NR_FUTEX 240
#define LINUX_NR_CLOCK_GETTIME 263
#define LINUX_NR_CLOCK_GETRES 264
#define LINUX_NR_CLOCK_NANOSLEEP 265
#define LINUX_NR_FSTATFS64 267
#define LINUX_NR_TGKILL 268
#define LINUX_NR_PERSONALITY 136
#define LINUX_NR_SCHED_GETSCHEDULER 157
#define LINUX_NR_PRCTL 172
#define LINUX_NR_RT_TGSIGQUEUEINFO 363
#define LINUX_PR_SET_NAME 15
#define LINUX_PR_GET_NAME 16
#define LINUX_PR_SET_VMA 0x53564d41
#define LINUX_NR_SOCKET 281
#define LINUX_NR_BIND 282
#define LINUX_NR_CONNECT 283
#define LINUX_NR_LISTEN 284
#define LINUX_NR_ACCEPT 285
#define LINUX_NR_GETSOCKNAME 286
#define LINUX_NR_GETPEERNAME 287
#define LINUX_NR_SOCKETPAIR 288
#define LINUX_NR_SEND 289
#define LINUX_NR_SENDTO 290
#define LINUX_NR_RECV 291
#define LINUX_NR_RECVFROM 292
#define LINUX_NR_SHUTDOWN 293
#define LINUX_NR_SETSOCKOPT 294
#define LINUX_NR_GETSOCKOPT 295
#define LINUX_NR_SENDMSG 296
#define LINUX_NR_RECVMSG 297
#define LINUX_NR_OPENAT 322
#define LINUX_NR_MKDIRAT 323
#define LINUX_NR_FSTATAT64 327
#define LINUX_NR_UNLINKAT 328
#define LINUX_NR_RENAMEAT 329
#define LINUX_NR_LINKAT 330
#define LINUX_NR_SYMLINKAT 331
#define LINUX_NR_READLINKAT 332
#define LINUX_NR_FCHMODAT 333
#define LINUX_NR_FACCESSAT 334
#define LINUX_NR_UTIMENSAT 348
#define LINUX_NR_EVENTFD2 356
#define LINUX_NR_PSELECT6 335
#define LINUX_NR_PPOLL 336
#define LINUX_NR_DUP3 358
#define LINUX_NR_PIPE2 359
#define LINUX_NR_ACCEPT4 366
#define LINUX_NR_PRLIMIT64 369
#define LINUX_NR_GETRANDOM 384
#define LINUX_NR_CLOCK_GETTIME64 403
#define LINUX_NR_CLOCK_GETRES_TIME64 406
#define LINUX_NR_CLOCK_NANOSLEEP_TIME64 407
#define LINUX_NR_PSELECT6_TIME64 413
#define LINUX_NR_PPOLL_TIME64 414
#define LINUX_ARM_NR_SET_TLS 0x000f0005u
#define LINUX_MAP_SHARED 0x00000001u
#define LINUX_MAP_PRIVATE 0x00000002u
#define LINUX_MAP_FIXED 0x00000010u
#define LINUX_MAP_ANONYMOUS 0x00000020u
#define LINUX_MAP_NORESERVE 0x00004000u
#define LINUX_MAP_KNOWN (LINUX_MAP_SHARED | LINUX_MAP_PRIVATE | \
    LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS | LINUX_MAP_NORESERVE)
#define LINUX_PROT_READ 0x1u
#define LINUX_PROT_WRITE 0x2u
#define LINUX_PROT_EXEC 0x4u
#define LINUX_AT_FDCWD (-100)
#define LINUX_AT_SYMLINK_NOFOLLOW 0x100u
#define LINUX_AT_REMOVEDIR 0x200u
#define LINUX_AT_SYMLINK_FOLLOW 0x400u
#define LINUX_O_CREAT 0x00000040u
#define LINUX_O_EXCL 0x00000080u
#define LINUX_O_NOFOLLOW 0x00008000u
#define LINUX_O_NONBLOCK 0x00000800u
#define LINUX_O_CLOEXEC 0x00080000u
#define LINUX_EFD_SEMAPHORE 0x00000001u
#define LINUX_WNOHANG 0x00000001u
#define LINUX_WUNTRACED 0x00000002u
#define LINUX_WCONTINUED 0x00000008u
#define LINUX_CLONE_VM 0x00000100u
#define LINUX_CLONE_VFORK 0x00004000u
#define LINUX_SIGCHLD 17u

struct linux_flock64 {
    int16_t type;
    int16_t whence;
    int32_t padding;
    int64_t start;
    int64_t length;
    int32_t pid;
    int32_t trailing_padding;
};

struct linux_rlimit64 {
    uint64_t current;
    uint64_t maximum;
};

static int linux_resource_number(uint32_t resource)
{
    switch (resource) {
    case 0: return RLIMIT_CPU;
    case 1: return RLIMIT_FSIZE;
    case 2: return RLIMIT_DATA;
    case 3: return RLIMIT_STACK;
    case 4: return RLIMIT_CORE;
#ifdef RLIMIT_RSS
    case 5: return RLIMIT_RSS;
#endif
#ifdef RLIMIT_NPROC
    case 6: return RLIMIT_NPROC;
#endif
    case 7: return RLIMIT_NOFILE;
#ifdef RLIMIT_AS
    case 9: return RLIMIT_AS;
#endif
    default: return -1;
    }
}

static uint64_t linux_rlimit_value(rlim_t value)
{
    return value == RLIM_INFINITY ? UINT64_MAX : (uint64_t)value;
}

static rlim_t host_rlimit_value(uint64_t value)
{
    return value == UINT64_MAX ? RLIM_INFINITY : (rlim_t)value;
}

static int32_t linux_prlimit64(int32_t process, uint32_t resource,
    const struct linux_rlimit64 *guest_new_limit,
    struct linux_rlimit64 *guest_old_limit)
{
    struct rlimit host_limit;
    struct rlimit host_new_limit;
    int host_resource = linux_resource_number(resource);
    if (process != 0 && process != (int32_t)getpid()) return -ESRCH;
    if (host_resource < 0) return -EINVAL;
    if (getrlimit(host_resource, &host_limit) != 0)
        return -(int32_t)linux_errno_number(errno);
    if (guest_old_limit != 0) {
        guest_old_limit->current = linux_rlimit_value(host_limit.rlim_cur);
        guest_old_limit->maximum = linux_rlimit_value(host_limit.rlim_max);
    }
    if (guest_new_limit != 0) {
        host_new_limit.rlim_cur = host_rlimit_value(guest_new_limit->current);
        host_new_limit.rlim_max = host_rlimit_value(guest_new_limit->maximum);
        if (setrlimit(host_resource, &host_new_limit) != 0)
            return -(int32_t)linux_errno_number(errno);
    }
    return 0;
}

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

/* Unsupported behaviour of an implemented syscall: report it for
   LINUXEMU_DIAGNOSTICS and return the Linux error. */
static int32_t linux_unsupported(uint32_t number, int32_t linux_error,
    const char *detail, const uint32_t *args)
{
    linux_diag_record(number, linux_error, detail, args);
    return -linux_error;
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
    length = append_text(message, length, " lr=0x");
    length = append_hex32(message, length, context->uc_mcontext.cpu.gpr[14]);
    length = append_text(message, length, " pc=0x");
    length = append_hex32(message, length, context->uc_mcontext.cpu.gpr[15]);
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

static void trace_internal_failure(const char *message)
{
    size_t length = 0;
    if (!runtime_trace_enabled()) return;
    while (message[length] != '\0') length++;
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

/* QNX fork() duplicates the fd table through each descriptor's manager and
   fails with EBADF when another thread closes a descriptor meanwhile; Linux
   fork() never fails that way.  A failed fork creates no child, so retry.
   Measured: about 1 in 3 attempts succeeds under continuous fd churn, so the
   bound is only a guard against an fd that is genuinely broken. */
#define FORK_EBADF_RETRIES 1000

static int32_t linux_fork_process(ucontext_t *context, uint32_t child_stack)
{
    pid_t process;
    int attempts = 0;
    do process = fork();
    while (process < 0 && errno == EBADF && ++attempts < FORK_EBADF_RETRIES);
    if (process < 0) return -(int32_t)linux_errno_number(errno);
    if (process == 0) {
        guest_thread_after_fork();
        linux_diag_after_fork();
        if (child_stack != 0)
            context->uc_mcontext.cpu.gpr[13] = child_stack;
    }
    return (int32_t)process;
}

static int32_t linux_writev_compat(int descriptor, const struct iovec *vectors,
    int count)
{
    unsigned char *buffer;
    size_t total = 0;
    size_t position = 0;
    int index;
    ssize_t result;

    if (count < 0 || count > 1024) return -EINVAL;
    for (index = 0; index != count; ++index) {
        if (vectors[index].iov_len > (size_t)INT_MAX - total) return -EINVAL;
        total += vectors[index].iov_len;
    }
    if (total == 0) return 0;
    buffer = malloc(total);
    if (buffer == 0) return -ENOMEM;
    for (index = 0; index != count; ++index) {
        memcpy(buffer + position, vectors[index].iov_base,
            vectors[index].iov_len);
        position += vectors[index].iov_len;
    }
    result = write(descriptor, buffer, total);
    free(buffer);
    return linux_result(result);
}

static int resolve_at(int directory_fd, const char *guest_path,
    int allow_missing_leaf, int nofollow, char *host_path, size_t host_path_size)
{
    char base[PATH_MAX];
    if ((guest_path == 0 || guest_path[0] != '/') &&
            directory_fd != LINUX_AT_FDCWD) {
        if (guest_fd_path(directory_fd, base, sizeof(base)) != 0) return -1;
        return guest_path_resolve_at(base, guest_path,
            allow_missing_leaf, nofollow, host_path, host_path_size);
    }
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
    if (guest_path != 0 && strcmp(guest_path, "/dev/ptmx") == 0) {
        unsigned index;
        int saved_errno = EAGAIN;
        for (index = 0; index != 256; ++index) {
            int length = snprintf(host_path, sizeof(host_path),
                "/dev/ptyp%u", index);
            if (length < 0 || (size_t)length >= sizeof(host_path))
                return -ENAMETOOLONG;
            fd = open(host_path, host_flags, (mode_t)(mode & 07777u));
            if (fd >= 0) {
                if (guest_fd_record_path(fd, host_path) != 0)
                    guest_fd_clear(fd);
                return fd;
            }
            if (errno == ENOENT) break;
            if (errno != EBUSY) saved_errno = errno;
        }
        return -(int32_t)linux_errno_number(saved_errno);
    }
    if (resolve_at(directory_fd, guest_path, (host_flags & O_CREAT) != 0,
            (flags & LINUX_O_NOFOLLOW) != 0 ||
            (flags & (LINUX_O_CREAT | LINUX_O_EXCL)) ==
                (LINUX_O_CREAT | LINUX_O_EXCL), host_path,
            sizeof(host_path)) != 0)
        return -(int32_t)linux_errno_number(errno);
    fd = open(host_path, host_flags, (mode_t)(mode & 07777u));
    if (fd >= 0 && guest_fd_record_path(fd, host_path) != 0)
        guest_fd_clear(fd);
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

static int32_t linux_mkdirat(int directory_fd, const char *guest_path,
    uint32_t mode)
{
    char host_path[PATH_MAX];
    if (resolve_at(directory_fd, guest_path, 1, 1, host_path,
            sizeof(host_path)) != 0)
        return -(int32_t)linux_errno_number(errno);
    return linux_host_result(mkdir(host_path, (mode_t)(mode & 07777u)));
}

static int32_t linux_unlinkat(int directory_fd, const char *guest_path,
    uint32_t flags)
{
    char host_path[PATH_MAX];
    if ((flags & ~LINUX_AT_REMOVEDIR) != 0) return -EINVAL;
    if (resolve_at(directory_fd, guest_path, 0, 1, host_path,
            sizeof(host_path)) != 0)
        return -(int32_t)linux_errno_number(errno);
    return linux_host_result((flags & LINUX_AT_REMOVEDIR) != 0 ?
        rmdir(host_path) : unlink(host_path));
}

static int32_t linux_renameat(int old_directory_fd, const char *old_guest_path,
    int new_directory_fd, const char *new_guest_path)
{
    char old_host_path[PATH_MAX];
    char new_host_path[PATH_MAX];
    if (resolve_at(old_directory_fd, old_guest_path, 0, 1, old_host_path,
            sizeof(old_host_path)) != 0 ||
        resolve_at(new_directory_fd, new_guest_path, 1, 1, new_host_path,
            sizeof(new_host_path)) != 0)
        return -(int32_t)linux_errno_number(errno);
    return linux_host_result(rename(old_host_path, new_host_path));
}

static int32_t linux_faccessat(int directory_fd, const char *guest_path,
    uint32_t mode)
{
    char host_path[PATH_MAX];
    if ((mode & ~7u) != 0) return -EINVAL;
    if (resolve_at(directory_fd, guest_path, 0, 0, host_path,
            sizeof(host_path)) != 0)
        return -(int32_t)linux_errno_number(errno);
    return linux_host_result(access(host_path, (int)mode));
}

static int32_t linux_linkat(int old_directory_fd, const char *old_guest_path,
    int new_directory_fd, const char *new_guest_path, uint32_t flags)
{
    char old_host_path[PATH_MAX];
    char new_host_path[PATH_MAX];
    if ((flags & ~LINUX_AT_SYMLINK_FOLLOW) != 0) return -EINVAL;
    if (resolve_at(old_directory_fd, old_guest_path, 0,
            (flags & LINUX_AT_SYMLINK_FOLLOW) == 0, old_host_path,
            sizeof(old_host_path)) != 0 ||
        resolve_at(new_directory_fd, new_guest_path, 1, 1, new_host_path,
            sizeof(new_host_path)) != 0)
        return -(int32_t)linux_errno_number(errno);
    return linux_host_result(link(old_host_path, new_host_path));
}

static int32_t linux_symlinkat(const char *guest_target, int directory_fd,
    const char *guest_link)
{
    char host_link[PATH_MAX];
    if (guest_target == 0) return -EFAULT;
    if (resolve_at(directory_fd, guest_link, 1, 1, host_link,
            sizeof(host_link)) != 0)
        return -(int32_t)linux_errno_number(errno);
    return linux_host_result(symlink(guest_target, host_link));
}

static int32_t linux_chmodat(int directory_fd, const char *guest_path,
    uint32_t mode)
{
    char host_path[PATH_MAX];
    if (resolve_at(directory_fd, guest_path, 0, 0, host_path,
            sizeof(host_path)) != 0)
        return -(int32_t)linux_errno_number(errno);
    return linux_host_result(chmod(host_path, (mode_t)(mode & 07777u)));
}

static int32_t linux_utimensat(int directory_fd, const char *guest_path,
    const uint32_t *guest_times, uint32_t flags)
{
    char host_path[PATH_MAX];
    struct stat status;
    struct utimbuf times;
    time_t now;
    size_t index;

    if ((flags & ~LINUX_AT_SYMLINK_NOFOLLOW) != 0) return -EINVAL;
    if (resolve_at(directory_fd, guest_path, 0,
            (flags & LINUX_AT_SYMLINK_NOFOLLOW) != 0, host_path,
            sizeof(host_path)) != 0)
        return -(int32_t)linux_errno_number(errno);
    if (guest_times == 0) return linux_host_result(utime(host_path, 0));
    if ((flags & LINUX_AT_SYMLINK_NOFOLLOW) != 0) {
        if (lstat(host_path, &status) != 0)
            return -(int32_t)linux_errno_number(errno);
        if (S_ISLNK(status.st_mode)) {
            const uint32_t args[6] = { (uint32_t)directory_fd,
                (uint32_t)(uintptr_t)guest_path,
                (uint32_t)(uintptr_t)guest_times, flags, 0, 0 };
            return linux_unsupported(LINUX_NR_UTIMENSAT, LINUX_EOPNOTSUPP,
                "utimensat-symlink", args);
        }
    } else if (stat(host_path, &status) != 0)
        return -(int32_t)linux_errno_number(errno);
    now = time(0);
    if (now == (time_t)-1) return -(int32_t)linux_errno_number(errno);
    times.actime = status.st_atime;
    times.modtime = status.st_mtime;
    for (index = 0; index != 2; ++index) {
        uint32_t nanoseconds = guest_times[index * 2 + 1];
        time_t seconds;
        if (nanoseconds == 0x3fffffffu) seconds = now;
        else if (nanoseconds == 0x3ffffffeu) continue;
        else {
            if (nanoseconds >= 1000000000u) return -EINVAL;
            seconds = (time_t)(int32_t)guest_times[index * 2];
        }
        if (index == 0) times.actime = seconds;
        else times.modtime = seconds;
    }
    return linux_host_result(utime(host_path, &times));
}

static void store_guest_u32(unsigned char *buffer, size_t offset,
    uint32_t value)
{
    memcpy(buffer + offset, &value, sizeof(value));
}

static void store_guest_u64(unsigned char *buffer, size_t offset,
    uint64_t value)
{
    memcpy(buffer + offset, &value, sizeof(value));
}

static int32_t linux_fstatfs64(int descriptor, size_t guest_size,
    void *guest_buffer)
{
    struct statvfs status;
    unsigned char *buffer = guest_buffer;
    uint32_t flags = 0;

    if (guest_size < 88) return -EINVAL;
    if (fstatvfs(descriptor, &status) != 0)
        return -(int32_t)linux_errno_number(errno);
#ifdef ST_RDONLY
    if ((status.f_flag & ST_RDONLY) != 0) flags |= 1u;
#endif
#ifdef ST_NOSUID
    if ((status.f_flag & ST_NOSUID) != 0) flags |= 2u;
#endif
    memset(buffer, 0, 88);
    store_guest_u32(buffer, 4, (uint32_t)status.f_bsize);
    store_guest_u64(buffer, 8, (uint64_t)status.f_blocks);
    store_guest_u64(buffer, 16, (uint64_t)status.f_bfree);
    store_guest_u64(buffer, 24, (uint64_t)status.f_bavail);
    store_guest_u64(buffer, 32, (uint64_t)status.f_files);
    store_guest_u64(buffer, 40, (uint64_t)status.f_ffree);
    store_guest_u32(buffer, 56, (uint32_t)status.f_namemax);
    store_guest_u32(buffer, 60, (uint32_t)status.f_frsize);
    store_guest_u32(buffer, 64, flags);
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
    struct guest_fd_object *object = guest_fd_acquire(fd);
    int saved_errno;

    if (object == 0) return -EBADF;
    directory = guest_fd_object_directory(object);
    if (directory == 0) {
        saved_errno = errno;
        guest_fd_put(object);
        return -(int32_t)linux_errno_number(saved_errno);
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
    saved_errno = errno;
    guest_fd_object_directory_done(object, directory);
    guest_fd_put(object);
    if (used == 0 && saved_errno != 0)
        return -(int32_t)linux_errno_number(saved_errno);
    return (int32_t)used;
}

static int32_t linux_close_fd(int fd)
{
    guest_fd_clear(fd);
    return linux_host_result(close(fd));
}

static int32_t linux_fcntl64(int fd, int command, uint32_t argument)
{
    int result;
    int typed_status;
    switch (command) {
    case 0: result = fcntl(fd, F_DUPFD, (int)argument); break;
    case 1030:
        result = fcntl(fd, F_DUPFD, (int)argument);
        if (result >= 0 && fcntl(result, F_SETFD, FD_CLOEXEC) != 0) {
            int saved_errno = errno;
            close(result);
            errno = saved_errno;
            result = -1;
        }
        break;
    case 1: result = fcntl(fd, F_GETFD); break;
    case 2:
        /* QNX returns the previous flags; Linux returns 0. */
        result = fcntl(fd, F_SETFD, (argument & 1u) != 0 ? FD_CLOEXEC : 0);
        if (result > 0) result = 0;
        break;
    case 3:
        typed_status = guest_fd_dispatch_get_status(fd, &result);
        if (typed_status == 0) result = fcntl(fd, F_GETFL);
        if (result >= 0) return (int32_t)linux_status_flags(result);
        break;
    case 4: {
        int requested;
        int current;
        typed_status = guest_fd_dispatch_get_status(fd, &current);
        if (typed_status == 0) current = fcntl(fd, F_GETFL);
        if (current < 0) return linux_host_result(current);
        if (linux_open_flags(argument & (0x00000400u | 0x00000800u),
                &requested) != 0) return -EINVAL;
        current &= ~(O_APPEND | O_NONBLOCK);
        current |= requested & (O_APPEND | O_NONBLOCK);
        typed_status = guest_fd_dispatch_set_status(fd, current);
        result = typed_status == 0 ? fcntl(fd, F_SETFL, current) :
            typed_status > 0 ? 0 : -1;
        if (result > 0) result = 0;
        break;
    }
    case 12:
    case 13:
    case 14: {
        struct linux_flock64 guest_lock;
        struct flock host_lock;
        memcpy(&guest_lock, (const void *)argument, sizeof(guest_lock));
        memset(&host_lock, 0, sizeof(host_lock));
        if (guest_lock.type == 0) host_lock.l_type = F_RDLCK;
        else if (guest_lock.type == 1) host_lock.l_type = F_WRLCK;
        else if (guest_lock.type == 2) host_lock.l_type = F_UNLCK;
        else return -EINVAL;
        if (guest_lock.whence < SEEK_SET || guest_lock.whence > SEEK_END)
            return -EINVAL;
        host_lock.l_whence = guest_lock.whence;
        host_lock.l_start = (off_t)guest_lock.start;
        host_lock.l_len = (off_t)guest_lock.length;
        result = fcntl(fd, command == 12 ? F_GETLK :
            command == 13 ? F_SETLK : F_SETLKW, &host_lock);
        if (result == 0 && command == 12) {
            guest_lock.type = host_lock.l_type == F_RDLCK ? 0 :
                host_lock.l_type == F_WRLCK ? 1 : 2;
            guest_lock.whence = host_lock.l_whence;
            guest_lock.start = host_lock.l_start;
            guest_lock.length = host_lock.l_len;
            guest_lock.pid = host_lock.l_pid;
            memcpy((void *)argument, &guest_lock, sizeof(guest_lock));
        }
        break;
    }
    default: return -EINVAL;
    }
    if ((command == 0 || command == 1030) && result >= 0)
        (void)guest_fd_duplicate(fd, result);
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
    guest_fd_clear(descriptors[0]);
    guest_fd_clear(descriptors[1]);
    memcpy(guest_descriptors, descriptors, sizeof(descriptors));
    return 0;
}

/* The wake eventfd: the pipe-backed subset libcurl uses.  The guest holds
   the read end; the write end is an emulator-owned hidden fd kept in the
   shared object, closed when the last guest reference goes. */
struct wake_eventfd {
    int write_fd;
};

static int32_t wake_eventfd_read(struct guest_fd_object *object, int fd,
    void *buffer, size_t length)
{
    (void)object;
    if (length < sizeof(uint64_t)) return -EINVAL;
    return linux_result(read(fd, buffer, sizeof(uint64_t)));
}

static int32_t wake_eventfd_write(struct guest_fd_object *object, int fd,
    const void *buffer, size_t length)
{
    struct wake_eventfd *data = guest_fd_object_data(object);
    uint64_t value;

    (void)fd;
    if (length < sizeof(value)) return -EINVAL;
    memcpy(&value, buffer, sizeof(value));
    if (value != 1) return -EINVAL;
    return linux_result(write(data->write_fd, &value, sizeof(value)));
}

static int wake_eventfd_poll_fd(struct guest_fd_object *object, int fd)
{
    (void)object;
    return fd;
}

static int wake_eventfd_get_status(struct guest_fd_object *object, int fd)
{
    (void)object;
    return fcntl(fd, F_GETFL);
}

static int wake_eventfd_set_status(struct guest_fd_object *object, int fd,
    int host_flags)
{
    struct wake_eventfd *data = guest_fd_object_data(object);
    int write_flags = fcntl(data->write_fd, F_GETFL);
    if (fcntl(fd, F_SETFL, host_flags) != 0 || write_flags < 0) return -1;
    write_flags = (write_flags & ~O_NONBLOCK) | (host_flags & O_NONBLOCK);
    return fcntl(data->write_fd, F_SETFL, write_flags) == 0 ? 0 : -1;
}

static void wake_eventfd_release(struct guest_fd_object *object)
{
    struct wake_eventfd *data = guest_fd_object_data(object);
    close(data->write_fd);
}

static int wake_eventfd_serialize(struct guest_fd_object *object, int fd,
    char *buffer, size_t size)
{
    struct wake_eventfd *data = guest_fd_object_data(object);
    int length = snprintf(buffer, size, "%d:", data->write_fd);
    (void)fd;
    return length < 0 || (size_t)length >= size ? -1 : length;
}

static const struct guest_fd_ops wake_eventfd_ops = {
    wake_eventfd_read, wake_eventfd_write, wake_eventfd_poll_fd,
    wake_eventfd_get_status, wake_eventfd_set_status, wake_eventfd_release,
    wake_eventfd_serialize
};

static int wake_eventfd_restore(int fd, int hidden_fd, const char *host_path)
{
    struct guest_fd_object *object;
    (void)host_path;
    if (hidden_fd < 0) return -1;
    object = guest_fd_object_new(GUEST_FD_WAKE_EVENTFD, &wake_eventfd_ops,
        fd, sizeof(struct wake_eventfd));
    if (object == 0) return -1;
    ((struct wake_eventfd *)guest_fd_object_data(object))->write_fd =
        hidden_fd;
    if (guest_fd_attach(fd, object) != 0) {
        guest_fd_put(object);
        return -1;
    }
    guest_fd_put(object);
    return 0;
}

void linux_syscall_register_fd_types(void)
{
    guest_fd_register_restore(GUEST_FD_WAKE_EVENTFD, wake_eventfd_restore);
}

static int32_t linux_eventfd2(uint32_t initial_value, uint32_t flags)
{
    int descriptors[2];
    int host_status_flags = 0;
    int saved_errno;
    struct guest_fd_object *object;
    struct wake_eventfd *data;

    if ((flags & ~(LINUX_EFD_SEMAPHORE | LINUX_O_NONBLOCK |
            LINUX_O_CLOEXEC)) != 0) return -EINVAL;
    /* The pipe-backed form is the wake descriptor subset used by libcurl. */
    if (initial_value != 0 || flags != (LINUX_O_NONBLOCK | LINUX_O_CLOEXEC)) {
        const uint32_t args[6] = { initial_value, flags, 0, 0, 0, 0 };
        return linux_unsupported(LINUX_NR_EVENTFD2, LINUX_EOPNOTSUPP,
            "eventfd-form", args);
    }
    if (pipe(descriptors) != 0) return -(int32_t)linux_errno_number(errno);
    if ((flags & LINUX_O_NONBLOCK) != 0) host_status_flags |= O_NONBLOCK;
    if ((host_status_flags != 0 &&
            (fcntl(descriptors[0], F_SETFL, host_status_flags) != 0 ||
            fcntl(descriptors[1], F_SETFL, host_status_flags) != 0)) ||
        ((flags & LINUX_O_CLOEXEC) != 0 &&
            fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) < 0))
        goto fail_both;
    descriptors[1] = guest_fd_hide(descriptors[1]);
    if (descriptors[1] < 0) {
        saved_errno = errno;
        close(descriptors[0]);
        return -(int32_t)linux_errno_number(saved_errno);
    }
    object = guest_fd_object_new(GUEST_FD_WAKE_EVENTFD, &wake_eventfd_ops,
        descriptors[0], sizeof(struct wake_eventfd));
    if (object == 0) goto fail_both;
    data = guest_fd_object_data(object);
    data->write_fd = descriptors[1];
    if (guest_fd_attach(descriptors[0], object) != 0) {
        saved_errno = errno;
        guest_fd_put(object);
        close(descriptors[0]);
        return -(int32_t)linux_errno_number(saved_errno);
    }
    guest_fd_put(object);
    return descriptors[0];
fail_both:
    saved_errno = errno;
    close(descriptors[0]);
    close(descriptors[1]);
    return -(int32_t)linux_errno_number(saved_errno);
}

static int32_t linux_execve(const char *guest_path, char *const guest_argv[],
    char *const guest_envp[])
{
    char host_path[PATH_MAX];

    if (guest_path_resolve(guest_path, 0, host_path, sizeof(host_path)) != 0)
        return -(int32_t)linux_errno_number(errno);
    guest_process_exec(host_path, guest_path, guest_argv, guest_envp);
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
    uintptr_t requested_address = context->uc_mcontext.cpu.gpr[0];
    size_t requested_length = (size_t)context->uc_mcontext.cpu.gpr[1];
    uint32_t linux_flags = context->uc_mcontext.cpu.gpr[3];
    int host_flags = 0;
    int host_descriptor;
    int host_protection;
    int mapping_protection;
    int copy_executable_file;
    void *result;
    off_t offset;
    size_t mapped_length;
    uintptr_t mapped_address;
    uintptr_t host_page_size = (uintptr_t)runtime_page_size();

    if (translate_protection(context->uc_mcontext.cpu.gpr[2],
            &host_protection) != 0) return -EINVAL;
    if ((linux_flags & ~LINUX_MAP_KNOWN) != 0)
        return linux_unsupported(LINUX_NR_MMAP2, LINUX_EINVAL, "mmap-flags",
            (const uint32_t *)context->uc_mcontext.cpu.gpr);
    if ((linux_flags & (LINUX_MAP_SHARED | LINUX_MAP_PRIVATE)) ==
            (LINUX_MAP_SHARED | LINUX_MAP_PRIVATE)) return -EINVAL;
    if ((linux_flags & LINUX_MAP_SHARED) != 0) host_flags |= MAP_SHARED;
    else if ((linux_flags & LINUX_MAP_PRIVATE) != 0) host_flags |= MAP_PRIVATE;
    else return -EINVAL;
    if (requested_length == 0 ||
        requested_length > SIZE_MAX - (host_page_size - 1u)) return -EINVAL;
    mapped_length = align_up(requested_length, host_page_size);
    if ((linux_flags & LINUX_MAP_FIXED) != 0) {
        if ((requested_address & (host_page_size - 1u)) != 0 ||
            !guest_memory_runtime_owned(requested_address, mapped_length))
            return -ENOMEM;
        host_flags |= MAP_FIXED;
    }
    if ((linux_flags & LINUX_MAP_ANONYMOUS) != 0) host_flags |= MAP_ANON;
    /* Linux MAP_NORESERVE skips commit accounting; QNX's nearest is
       MAP_LAZY (acquire memory on first touch). Never pass 0x4000 through:
       on QNX it is MAP_NOINIT, which would hand out unzeroed pages. */
    if ((linux_flags & LINUX_MAP_NORESERVE) != 0) host_flags |= MAP_LAZY;
    if ((linux_flags & LINUX_MAP_SHARED) != 0 &&
        (host_protection & PROT_EXEC) != 0)
        return linux_unsupported(LINUX_NR_MMAP2, LINUX_EOPNOTSUPP,
            "mmap-shared-exec", (const uint32_t *)context->uc_mcontext.cpu.gpr);
    offset = (off_t)context->uc_mcontext.cpu.gpr[5] * 4096;
    host_descriptor = (int)context->uc_mcontext.cpu.gpr[4];
    copy_executable_file =
        (linux_flags & LINUX_MAP_PRIVATE) != 0 &&
        (linux_flags & LINUX_MAP_ANONYMOUS) == 0 &&
        (host_protection & PROT_EXEC) != 0;
    if (copy_executable_file) {
        host_flags |= MAP_ANON;
        host_descriptor = -1;
    }
    mapping_protection = host_protection;
    if ((linux_flags & LINUX_MAP_PRIVATE) != 0 &&
        (linux_flags & LINUX_MAP_ANONYMOUS) == 0)
        mapping_protection =
            (host_protection | PROT_READ | PROT_WRITE) & ~PROT_EXEC;
    result = mmap((void *)requested_address, requested_length,
        mapping_protection, host_flags,
        host_descriptor, copy_executable_file ? 0 : offset);
    if (result == MAP_FAILED) {
        trace_internal_failure("linuxemu: host mmap failed\n");
        return -(int32_t)linux_errno_number(errno);
    }
    mapped_address = (uintptr_t)result;
    if (mapped_address < GUEST_MIN_ADDRESS ||
        mapped_address >= GUEST_MAX_ADDRESS ||
        mapped_length > GUEST_MAX_ADDRESS - mapped_address) {
        trace_internal_failure("linuxemu: host mmap outside guest range\n");
        munmap(result, mapped_length);
        return -ENOMEM;
    }
    if (copy_executable_file) {
        struct stat status;
        size_t file_bytes;
        int descriptor = (int)context->uc_mcontext.cpu.gpr[4];
        if (fstat(descriptor, &status) != 0) {
            int saved_errno = errno;
            munmap(result, mapped_length);
            return -(int32_t)linux_errno_number(saved_errno);
        }
        if (status.st_size < 0 || offset < 0 || offset > status.st_size) {
            munmap(result, mapped_length);
            return -EINVAL;
        }
        file_bytes = requested_length;
        if ((off_t)file_bytes > status.st_size - offset)
            file_bytes = (size_t)(status.st_size - offset);
        if (file_bytes != 0 && read_exact_at(descriptor, result,
                file_bytes, offset) != 0) {
            int saved_errno = errno;
            munmap(result, mapped_length);
            return -(int32_t)linux_errno_number(saved_errno);
        }
    }
    arm_patch_forget_range(mapped_address, mapped_length);
    if ((linux_flags & LINUX_MAP_ANONYMOUS) == 0 &&
        arm_patch_elf_mapping((int)context->uc_mcontext.cpu.gpr[4], offset,
            mapped_address, mapped_length) != 0) {
        int saved_errno = errno;
        trace_internal_failure("linuxemu: runtime ELF patch failed\n");
        arm_patch_forget_range(mapped_address, mapped_length);
        munmap(result, mapped_length);
        return -(int32_t)linux_errno_number(saved_errno);
    }
    if ((host_protection & PROT_EXEC) != 0 &&
        msync(result, mapped_length, MS_INVALIDATE_ICACHE) != 0) {
        int saved_errno = errno;
        trace_internal_failure("linuxemu: runtime icache sync failed\n");
        arm_patch_forget_range(mapped_address, mapped_length);
        munmap(result, mapped_length);
        return -(int32_t)linux_errno_number(saved_errno);
    }
    if (mapping_protection != host_protection &&
        mprotect(result, mapped_length, host_protection) != 0) {
        int saved_errno = errno;
        trace_internal_failure("linuxemu: runtime mprotect failed\n");
        arm_patch_forget_range(mapped_address, mapped_length);
        munmap(result, mapped_length);
        return -(int32_t)linux_errno_number(saved_errno);
    }
    if (guest_memory_runtime_map(mapped_address, mapped_length,
            (host_protection & PROT_EXEC) != 0) != 0) {
        int saved_errno = errno;
        trace_internal_failure("linuxemu: runtime mapping registry full\n");
        arm_patch_forget_range(mapped_address, mapped_length);
        munmap(result, mapped_length);
        return -(int32_t)linux_errno_number(saved_errno);
    }
    return (int32_t)mapped_address;
}

static int32_t linux_mprotect_runtime(uintptr_t address, size_t length,
    uint32_t linux_protection)
{
    uintptr_t host_page_size = (uintptr_t)runtime_page_size();
    size_t mapped_length;
    size_t patches_before = arm_patch_count();
    int host_protection;
    if (translate_protection(linux_protection, &host_protection) != 0)
        return -EINVAL;
    if (length == 0 || (address & (host_page_size - 1u)) != 0 ||
        length > SIZE_MAX - (host_page_size - 1u)) return -EINVAL;
    mapped_length = align_up(length, host_page_size);
    if (!guest_memory_runtime_owned(address, mapped_length)) {
        if (!guest_memory_range_owned(address, mapped_length)) return -ENOMEM;
        if ((host_protection & PROT_EXEC) != 0 &&
            !guest_memory_is_executable(address, mapped_length)) return -EACCES;
        if (mprotect((void *)address, mapped_length, host_protection) != 0)
            return -(int32_t)linux_errno_number(errno);
        return 0;
    }
    if ((host_protection & PROT_EXEC) != 0) {
        if (mprotect((void *)address, mapped_length,
                host_protection | PROT_READ | PROT_WRITE) != 0)
            return -(int32_t)linux_errno_number(errno);
        if (arm_patch_range(address, mapped_length) != 0) {
            int saved_errno = errno;
            arm_patch_rollback(patches_before);
            mprotect((void *)address, mapped_length, host_protection);
            return -(int32_t)linux_errno_number(saved_errno);
        }
        if (msync((void *)address, mapped_length,
                MS_INVALIDATE_ICACHE) != 0) {
            int saved_errno = errno;
            arm_patch_rollback(patches_before);
            mprotect((void *)address, mapped_length, host_protection);
            return -(int32_t)linux_errno_number(saved_errno);
        }
    }
    if (mprotect((void *)address, mapped_length, host_protection) != 0) {
        int saved_errno = errno;
        if ((host_protection & PROT_EXEC) != 0)
            arm_patch_rollback(patches_before);
        return -(int32_t)linux_errno_number(saved_errno);
    }
    if (guest_memory_runtime_protect(address, mapped_length,
            (host_protection & PROT_EXEC) != 0) != 0)
        return -(int32_t)linux_errno_number(errno);
    return 0;
}

static int32_t linux_munmap_runtime(uintptr_t address, size_t length)
{
    uintptr_t host_page_size = (uintptr_t)runtime_page_size();
    size_t mapped_length;
    if (length == 0 || (address & (host_page_size - 1u)) != 0 ||
        length > SIZE_MAX - (host_page_size - 1u)) return -EINVAL;
    mapped_length = align_up(length, host_page_size);
    if (!guest_memory_runtime_owned(address, mapped_length)) return -EINVAL;
    if (guest_thread_defer_active_stack_unmap(address, mapped_length))
        return 0;
    if (munmap((void *)address, mapped_length) != 0)
        return -(int32_t)linux_errno_number(errno);
    arm_patch_forget_range(address, mapped_length);
    guest_memory_runtime_unmap(address, mapped_length);
    return 0;
}

static int32_t linux_getrandom(void *buffer, size_t length, uint32_t flags)
{
    int descriptor;
    int open_flags = O_RDONLY;
    ssize_t result;

    if ((flags & ~3u) != 0) return -EINVAL;
    if (length == 0) return 0;
    if ((flags & 1u) != 0) open_flags |= O_NONBLOCK;
    descriptor = open((flags & 2u) != 0 ? "/dev/random" : "/dev/urandom",
        open_flags);
    if (descriptor < 0) return -(int32_t)linux_errno_number(errno);
    result = read(descriptor, buffer, length);
    if (result < 0) {
        int saved_errno = errno;
        close(descriptor);
        return -(int32_t)linux_errno_number(saved_errno);
    }
    close(descriptor);
    return (int32_t)result;
}

static int32_t linux_flock(int descriptor, uint32_t operation)
{
    struct flock lock;
    int command;

    if ((operation & ~4u) != 1u && (operation & ~4u) != 2u &&
        (operation & ~4u) != 8u) return -EINVAL;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = (operation & ~4u) == 1u ? F_RDLCK :
        (operation & ~4u) == 2u ? F_WRLCK : F_UNLCK;
    lock.l_whence = SEEK_SET;
    command = (operation & 4u) != 0 ? F_SETLK : F_SETLKW;
    return linux_host_result(fcntl(descriptor, command, &lock));
}

/*
 * sched_getscheduler. Guests cannot change their policy (sched_setscheduler
 * is not implemented), so every existing task is SCHED_OTHER (0), Linux's
 * default. 0 means the caller; a negative pid is EINVAL; ESRCH if no task.
 */
static int32_t linux_sched_getscheduler(int32_t pid)
{
    if (pid < 0) return -EINVAL;
    if (pid == 0 || guest_thread_kill(pid, 0) == 0) return 0;
    if (kill((pid_t)pid, 0) == 0 || errno == EPERM) return 0;
    return -ESRCH;
}

/* prctl options bionic uses at start-up. PR_SET_VMA (anonymous mapping
   names) is not available here: EINVAL, as on a Linux kernel without
   CONFIG_ANON_VMA_NAME. Unknown options are EINVAL, as on Linux. */
static int32_t linux_prctl(const uint32_t *arguments)
{
    char name[16];
    switch (arguments[0]) {
    case LINUX_PR_SET_NAME:
        memset(name, 0, sizeof(name));
        strncpy(name, (const char *)(uintptr_t)arguments[1],
            sizeof(name) - 1);
        guest_thread_name_set(name);
        return 0;
    case LINUX_PR_GET_NAME:
        guest_thread_name_get(name);
        memcpy((void *)(uintptr_t)arguments[1], name, sizeof(name));
        return 0;
    case LINUX_PR_SET_VMA:
        return linux_unsupported(LINUX_NR_PRCTL, LINUX_EINVAL,
            "prctl-set-vma", arguments);
    default:
        return linux_unsupported(LINUX_NR_PRCTL, LINUX_EINVAL,
            "prctl-option", arguments);
    }
}

void linux_syscall_dispatch(ucontext_t *context)
{
    uint32_t number = context->uc_mcontext.cpu.gpr[7];
    int32_t result;

    trace_call(number, context);
    switch (number) {
    case LINUX_NR_FORK:
        result = linux_fork_process(context, 0);
        break;
    case LINUX_NR_READ:
        if (!guest_fd_dispatch_read((int)context->uc_mcontext.cpu.gpr[0],
                (void *)context->uc_mcontext.cpu.gpr[1],
                (size_t)context->uc_mcontext.cpu.gpr[2], &result))
            result = linux_result(read(
            (int)context->uc_mcontext.cpu.gpr[0],
            (void *)context->uc_mcontext.cpu.gpr[1],
            (size_t)context->uc_mcontext.cpu.gpr[2]));
        break;
    case LINUX_NR_WRITE:
        if (!guest_fd_dispatch_write((int)context->uc_mcontext.cpu.gpr[0],
                (const void *)context->uc_mcontext.cpu.gpr[1],
                (size_t)context->uc_mcontext.cpu.gpr[2], &result))
            result = linux_result(write(
            (int)context->uc_mcontext.cpu.gpr[0],
            (const void *)context->uc_mcontext.cpu.gpr[1],
            (size_t)context->uc_mcontext.cpu.gpr[2]));
        break;
    case LINUX_NR_PREAD64: {
        uint64_t offset = (uint64_t)context->uc_mcontext.cpu.gpr[4] |
            ((uint64_t)context->uc_mcontext.cpu.gpr[5] << 32);
        result = linux_result(pread((int)context->uc_mcontext.cpu.gpr[0],
            (void *)context->uc_mcontext.cpu.gpr[1],
            (size_t)context->uc_mcontext.cpu.gpr[2], (off_t)offset));
        break;
    }
    case LINUX_NR_PWRITE64: {
        uint64_t offset = (uint64_t)context->uc_mcontext.cpu.gpr[4] |
            ((uint64_t)context->uc_mcontext.cpu.gpr[5] << 32);
        result = linux_result(pwrite((int)context->uc_mcontext.cpu.gpr[0],
            (const void *)context->uc_mcontext.cpu.gpr[1],
            (size_t)context->uc_mcontext.cpu.gpr[2], (off_t)offset));
        break;
    }
    case LINUX_NR_FSYNC:
    case LINUX_NR_FDATASYNC:
        /* QNX 10.3 has fsync but no distinct fdatasync operation. */
        result = linux_host_result(fsync(
            (int)context->uc_mcontext.cpu.gpr[0]));
        break;
    case LINUX_NR_FTRUNCATE64: {
        uint64_t length = (uint64_t)context->uc_mcontext.cpu.gpr[2] |
            ((uint64_t)context->uc_mcontext.cpu.gpr[3] << 32);
        result = linux_host_result(ftruncate(
            (int)context->uc_mcontext.cpu.gpr[0], (off_t)length));
        break;
    }
    case LINUX_NR_READV:
        result = linux_result(readv((int)context->uc_mcontext.cpu.gpr[0],
            (const struct iovec *)context->uc_mcontext.cpu.gpr[1],
            (int)context->uc_mcontext.cpu.gpr[2]));
        break;
    case LINUX_NR_WRITEV:
        result = linux_writev_compat(
            (int)context->uc_mcontext.cpu.gpr[0],
            (const struct iovec *)context->uc_mcontext.cpu.gpr[1],
            (int)context->uc_mcontext.cpu.gpr[2]);
        break;
    case LINUX_NR_UMASK:
        result = (int32_t)umask((mode_t)context->uc_mcontext.cpu.gpr[0]);
        break;
    case LINUX_NR_FLOCK:
        result = linux_flock((int)context->uc_mcontext.cpu.gpr[0],
            context->uc_mcontext.cpu.gpr[1]);
        break;
    case LINUX_NR_OPEN:
        result = linux_open_path(LINUX_AT_FDCWD,
            (const char *)context->uc_mcontext.cpu.gpr[0],
            context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2]);
        break;
    case LINUX_NR_LINK:
        result = linux_linkat(LINUX_AT_FDCWD,
            (const char *)context->uc_mcontext.cpu.gpr[0], LINUX_AT_FDCWD,
            (const char *)context->uc_mcontext.cpu.gpr[1], 0);
        break;
    case LINUX_NR_UNLINK:
        result = linux_unlinkat(LINUX_AT_FDCWD,
            (const char *)context->uc_mcontext.cpu.gpr[0], 0);
        break;
    case LINUX_NR_SYMLINK:
        result = linux_symlinkat(
            (const char *)context->uc_mcontext.cpu.gpr[0], LINUX_AT_FDCWD,
            (const char *)context->uc_mcontext.cpu.gpr[1]);
        break;
    case LINUX_NR_RENAME:
        result = linux_renameat(LINUX_AT_FDCWD,
            (const char *)context->uc_mcontext.cpu.gpr[0], LINUX_AT_FDCWD,
            (const char *)context->uc_mcontext.cpu.gpr[1]);
        break;
    case LINUX_NR_MKDIR:
        result = linux_mkdirat(LINUX_AT_FDCWD,
            (const char *)context->uc_mcontext.cpu.gpr[0],
            context->uc_mcontext.cpu.gpr[1]);
        break;
    case LINUX_NR_RMDIR:
        result = linux_unlinkat(LINUX_AT_FDCWD,
            (const char *)context->uc_mcontext.cpu.gpr[0],
            LINUX_AT_REMOVEDIR);
        break;
    case LINUX_NR_CHMOD:
        result = linux_chmodat(LINUX_AT_FDCWD,
            (const char *)context->uc_mcontext.cpu.gpr[0],
            context->uc_mcontext.cpu.gpr[1]);
        break;
    case LINUX_NR_OPENAT:
        result = linux_open_path((int32_t)context->uc_mcontext.cpu.gpr[0],
            (const char *)context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2],
            context->uc_mcontext.cpu.gpr[3]);
        break;
    case LINUX_NR_MKDIRAT:
        result = linux_mkdirat((int32_t)context->uc_mcontext.cpu.gpr[0],
            (const char *)context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2]);
        break;
    case LINUX_NR_UNLINKAT:
        result = linux_unlinkat((int32_t)context->uc_mcontext.cpu.gpr[0],
            (const char *)context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2]);
        break;
    case LINUX_NR_RENAMEAT:
        result = linux_renameat((int32_t)context->uc_mcontext.cpu.gpr[0],
            (const char *)context->uc_mcontext.cpu.gpr[1],
            (int32_t)context->uc_mcontext.cpu.gpr[2],
            (const char *)context->uc_mcontext.cpu.gpr[3]);
        break;
    case LINUX_NR_LINKAT:
        result = linux_linkat((int32_t)context->uc_mcontext.cpu.gpr[0],
            (const char *)context->uc_mcontext.cpu.gpr[1],
            (int32_t)context->uc_mcontext.cpu.gpr[2],
            (const char *)context->uc_mcontext.cpu.gpr[3],
            context->uc_mcontext.cpu.gpr[4]);
        break;
    case LINUX_NR_SYMLINKAT:
        result = linux_symlinkat(
            (const char *)context->uc_mcontext.cpu.gpr[0],
            (int32_t)context->uc_mcontext.cpu.gpr[1],
            (const char *)context->uc_mcontext.cpu.gpr[2]);
        break;
    case LINUX_NR_FCHMODAT:
        result = linux_chmodat((int32_t)context->uc_mcontext.cpu.gpr[0],
            (const char *)context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2]);
        break;
    case LINUX_NR_FACCESSAT:
        result = linux_faccessat((int32_t)context->uc_mcontext.cpu.gpr[0],
            (const char *)context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2]);
        break;
    case LINUX_NR_UTIMENSAT:
        result = linux_utimensat((int32_t)context->uc_mcontext.cpu.gpr[0],
            (const char *)context->uc_mcontext.cpu.gpr[1],
            (const uint32_t *)context->uc_mcontext.cpu.gpr[2],
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
        if (result >= 0) (void)guest_fd_duplicate(
            (int)context->uc_mcontext.cpu.gpr[0], result);
        break;
    case LINUX_NR_DUP2:
        if ((int)context->uc_mcontext.cpu.gpr[0] ==
                (int)context->uc_mcontext.cpu.gpr[1])
            result = linux_host_result(fcntl(
                (int)context->uc_mcontext.cpu.gpr[0], F_GETFD, 0)) < 0 ?
                -(int32_t)linux_errno_number(errno) :
                (int)context->uc_mcontext.cpu.gpr[0];
        else {
            int source = (int)context->uc_mcontext.cpu.gpr[0];
            int destination = (int)context->uc_mcontext.cpu.gpr[1];
            if (fcntl(source, F_GETFD, 0) < 0)
                result = -(int32_t)linux_errno_number(errno);
            else result = linux_host_result(dup2(source, destination));
            if (result >= 0 &&
                (int)context->uc_mcontext.cpu.gpr[0] != result)
                (void)guest_fd_duplicate((int)context->uc_mcontext.cpu.gpr[0], result);
        }
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
    case LINUX_NR_IOCTL:
        result = linux_ioctl((int)context->uc_mcontext.cpu.gpr[0],
            context->uc_mcontext.cpu.gpr[1],
            (void *)context->uc_mcontext.cpu.gpr[2]);
        break;
    case LINUX_NR_PIPE:
        result = linux_pipe((void *)context->uc_mcontext.cpu.gpr[0], 0);
        break;
    case LINUX_NR_GETPPID:
        result = (int32_t)getppid();
        break;
    case LINUX_NR_GETPGRP:
        result = (int32_t)getpgrp();
        break;
    case LINUX_NR_GETPGID:
        if ((int32_t)context->uc_mcontext.cpu.gpr[0] == 0 ||
            (pid_t)(int32_t)context->uc_mcontext.cpu.gpr[0] == getpid())
            result = (int32_t)getpgrp();
        else result = linux_host_result(getpgid(
            (pid_t)(int32_t)context->uc_mcontext.cpu.gpr[0]));
        break;
    case LINUX_NR_SETPGID:
        result = linux_host_result(setpgid(
            (pid_t)(int32_t)context->uc_mcontext.cpu.gpr[0],
            (pid_t)(int32_t)context->uc_mcontext.cpu.gpr[1]));
        break;
    case LINUX_NR_KILL:
        result = guest_signal_send(
            (pid_t)(int32_t)context->uc_mcontext.cpu.gpr[0],
            (int)context->uc_mcontext.cpu.gpr[1]);
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
        result = linux_mprotect_runtime(context->uc_mcontext.cpu.gpr[0],
            (size_t)context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2]);
        break;
    case LINUX_NR_MUNMAP:
        result = linux_munmap_runtime(context->uc_mcontext.cpu.gpr[0],
            (size_t)context->uc_mcontext.cpu.gpr[1]);
        break;
    case LINUX_NR_SOCKETCALL:
    case LINUX_NR_SOCKET:
    case LINUX_NR_BIND:
    case LINUX_NR_CONNECT:
    case LINUX_NR_LISTEN:
    case LINUX_NR_ACCEPT:
    case LINUX_NR_GETSOCKNAME:
    case LINUX_NR_GETPEERNAME:
    case LINUX_NR_SOCKETPAIR:
    case LINUX_NR_SEND:
    case LINUX_NR_SENDTO:
    case LINUX_NR_RECV:
    case LINUX_NR_RECVFROM:
    case LINUX_NR_SHUTDOWN:
    case LINUX_NR_SETSOCKOPT:
    case LINUX_NR_GETSOCKOPT:
    case LINUX_NR_SENDMSG:
    case LINUX_NR_RECVMSG:
    case LINUX_NR_ACCEPT4:
        result = linux_socket_syscall(number, context->uc_mcontext.cpu.gpr);
        break;
    case LINUX_NR_GETRANDOM:
        result = linux_getrandom((void *)context->uc_mcontext.cpu.gpr[0],
            (size_t)context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2]);
        break;
    case LINUX_NR_PRLIMIT64:
        result = linux_prlimit64(
            (int32_t)context->uc_mcontext.cpu.gpr[0],
            context->uc_mcontext.cpu.gpr[1],
            (const struct linux_rlimit64 *)context->uc_mcontext.cpu.gpr[2],
            (struct linux_rlimit64 *)context->uc_mcontext.cpu.gpr[3]);
        break;
    case LINUX_NR_FSTATFS64:
        result = linux_fstatfs64((int)context->uc_mcontext.cpu.gpr[0],
            (size_t)context->uc_mcontext.cpu.gpr[1],
            (void *)context->uc_mcontext.cpu.gpr[2]);
        break;
    case LINUX_NR_WAIT4:
        result = linux_wait4((int32_t)context->uc_mcontext.cpu.gpr[0],
            (int *)context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2],
            (void *)context->uc_mcontext.cpu.gpr[3]);
        break;
    case LINUX_NR_CLONE:
        if ((context->uc_mcontext.cpu.gpr[0] & ~0xffu) == 0 &&
            ((context->uc_mcontext.cpu.gpr[0] & 0xffu) != 0 &&
            (context->uc_mcontext.cpu.gpr[0] & 0xffu) != 17u))
            result = -EINVAL;
        else if ((context->uc_mcontext.cpu.gpr[0] & ~0xffu) == 0)
            result = linux_fork_process(context, 0);
        else if (context->uc_mcontext.cpu.gpr[0] ==
                (LINUX_CLONE_VM | LINUX_CLONE_VFORK | LINUX_SIGCHLD)) {
            result = linux_fork_process(context,
                context->uc_mcontext.cpu.gpr[1]);
        }
        else result = guest_thread_clone(context);
        break;
    case LINUX_NR_UNAME:
        result = linux_uname((void *)context->uc_mcontext.cpu.gpr[0]);
        break;
    case LINUX_NR_FCHDIR: {
        int fd = (int)context->uc_mcontext.cpu.gpr[0];
        char path[PATH_MAX];
        if (guest_fd_path(fd, path, sizeof(path)) != 0)
            result = -(int32_t)linux_errno_number(errno);
        else result = guest_path_fchdir(path) == 0 ? 0 :
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
        result = linux_fork_process(context, 0);
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
    case LINUX_NR_SIGALTSTACK:
        result = guest_signal_altstack(context,
            (const void *)context->uc_mcontext.cpu.gpr[0],
            (void *)context->uc_mcontext.cpu.gpr[1]);
        break;
    case LINUX_NR_TKILL:
        result = guest_signal_send_thread(0,
            (int32_t)context->uc_mcontext.cpu.gpr[0],
            (int)context->uc_mcontext.cpu.gpr[1]);
        break;
    case LINUX_NR_TGKILL:
        result = guest_signal_send_thread(
            (int32_t)context->uc_mcontext.cpu.gpr[0],
            (int32_t)context->uc_mcontext.cpu.gpr[1],
            (int)context->uc_mcontext.cpu.gpr[2]);
        break;
    case LINUX_NR_RT_TGSIGQUEUEINFO:
        result = guest_signal_queue_thread(
            (int32_t)context->uc_mcontext.cpu.gpr[0],
            (int32_t)context->uc_mcontext.cpu.gpr[1],
            (int)context->uc_mcontext.cpu.gpr[2],
            (const void *)(uintptr_t)context->uc_mcontext.cpu.gpr[3]);
        break;
    case LINUX_NR_PERSONALITY:
        result = (int32_t)guest_process_personality(
            context->uc_mcontext.cpu.gpr[0]);
        break;
    case LINUX_NR_SCHED_GETSCHEDULER:
        result = linux_sched_getscheduler(
            (int32_t)context->uc_mcontext.cpu.gpr[0]);
        break;
    case LINUX_NR_PRCTL:
        result = linux_prctl(context->uc_mcontext.cpu.gpr);
        break;
    case LINUX_NR_TIME:
    case LINUX_NR_GETTIMEOFDAY:
    case LINUX_NR_NANOSLEEP:
    case LINUX_NR_CLOCK_GETTIME:
    case LINUX_NR_CLOCK_GETRES:
    case LINUX_NR_CLOCK_NANOSLEEP:
    case LINUX_NR_CLOCK_GETTIME64:
    case LINUX_NR_CLOCK_GETRES_TIME64:
    case LINUX_NR_CLOCK_NANOSLEEP_TIME64:
        result = linux_time_syscall(number, context->uc_mcontext.cpu.gpr);
        break;
    case LINUX_NR_SELECT:
    case LINUX_NR_NEWSELECT:
    case LINUX_NR_POLL:
    case LINUX_NR_PSELECT6:
    case LINUX_NR_PPOLL:
    case LINUX_NR_PSELECT6_TIME64:
    case LINUX_NR_PPOLL_TIME64:
        result = linux_poll_syscall(number, context->uc_mcontext.cpu.gpr);
        break;
    case LINUX_NR_GETPID:
        result = (int32_t)getpid();
        break;
    case LINUX_NR_GETTID:
        result = guest_thread_tid();
        break;
    case LINUX_NR_SET_TID_ADDRESS:
        result = guest_thread_set_tid_address(
            (uint32_t *)context->uc_mcontext.cpu.gpr[0]);
        break;
    case LINUX_NR_FUTEX:
        result = linux_futex(
            (uint32_t *)context->uc_mcontext.cpu.gpr[0],
            context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2],
            (const void *)context->uc_mcontext.cpu.gpr[3],
            (uint32_t *)context->uc_mcontext.cpu.gpr[4],
            context->uc_mcontext.cpu.gpr[5]);
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
        else if (fcntl(source, F_GETFD, 0) < 0)
            result = -(int32_t)linux_errno_number(errno);
        else {
            result = linux_host_result(dup2(source, destination));
            if (result >= 0 && (flags & LINUX_O_CLOEXEC) != 0 &&
                fcntl(destination, F_SETFD, FD_CLOEXEC) != 0)
                result = -(int32_t)linux_errno_number(errno);
            if (result >= 0) (void)guest_fd_duplicate(source, destination);
        }
        break;
    }
    case LINUX_NR_EVENTFD2:
        result = linux_eventfd2(context->uc_mcontext.cpu.gpr[0],
            context->uc_mcontext.cpu.gpr[1]);
        break;
    case LINUX_NR_PIPE2:
        result = linux_pipe((void *)context->uc_mcontext.cpu.gpr[0],
            context->uc_mcontext.cpu.gpr[1]);
        break;
    case LINUX_NR_EXIT_GROUP:
        linux_diag_flush();
        _exit((int)(context->uc_mcontext.cpu.gpr[0] & 0xffu));
        return;
    case LINUX_NR_EXIT:
        guest_thread_exit((int)(context->uc_mcontext.cpu.gpr[0] & 0xffu));
        return;
    case LINUX_ARM_NR_SET_TLS:
        runtime_set_guest_tls(context->uc_mcontext.cpu.gpr[0]);
        result = 0;
        break;
    default:
        linux_diag_record(number, LINUX_ENOSYS, 0,
            (const uint32_t *)context->uc_mcontext.cpu.gpr);
        result = -LINUX_ENOSYS;
        break;
    }
    context->uc_mcontext.cpu.gpr[0] = (uint32_t)result;
    trace_result((uint32_t)result);
}
