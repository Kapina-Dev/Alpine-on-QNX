#include "linuxemu.h"
#include "linux_diagnostics.h"
#include "guest_fd.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
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
#define LINUX_EPERM 1
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
#define LINUX_NR_RT_SIGQUEUEINFO 178
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
#define LINUX_NR_CLOCK_SETTIME 262
#define LINUX_NR_CLOCK_GETTIME 263
#define LINUX_NR_CLOCK_GETRES 264
#define LINUX_NR_CLOCK_NANOSLEEP 265
#define LINUX_NR_FSTATFS64 267
#define LINUX_NR_TGKILL 268
#define LINUX_NR_PERSONALITY 136
#define LINUX_NR_RT_TGSIGQUEUEINFO 363
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
#define LINUX_NR_EPOLL_CREATE 250
#define LINUX_NR_EPOLL_CTL 251
#define LINUX_NR_EPOLL_WAIT 252
#define LINUX_NR_EPOLL_PWAIT 346
#define LINUX_NR_EPOLL_CREATE1 357
#define LINUX_NR_EPOLL_PWAIT2 441
#define LINUX_NR_SIGNALFD 349
#define LINUX_NR_SIGNALFD4 355
#define LINUX_NR_EVENTFD 351
#define LINUX_NR_TIMERFD_CREATE 350
#define LINUX_NR_TIMERFD_SETTIME 353
#define LINUX_NR_TIMERFD_GETTIME 354
#define LINUX_NR_TIMERFD_GETTIME64 410
#define LINUX_NR_TIMERFD_SETTIME64 411
#define LINUX_NR_INOTIFY_INIT 316
#define LINUX_NR_INOTIFY_ADD_WATCH 317
#define LINUX_NR_INOTIFY_RM_WATCH 318
#define LINUX_NR_INOTIFY_INIT1 360
#define LINUX_NR_EVENTFD2 356
#define LINUX_NR_PSELECT6 335
#define LINUX_NR_PPOLL 336
#define LINUX_NR_DUP3 358
#define LINUX_NR_PIPE2 359
#define LINUX_NR_RECVMMSG 365
#define LINUX_NR_ACCEPT4 366
#define LINUX_NR_SENDMMSG 374
#define LINUX_NR_PRLIMIT64 369
#define LINUX_NR_GETRANDOM 384
#define LINUX_NR_CLOCK_GETTIME64 403
#define LINUX_NR_CLOCK_SETTIME64 404
#define LINUX_NR_CLOCK_GETRES_TIME64 406
#define LINUX_NR_CLOCK_NANOSLEEP_TIME64 407
#define LINUX_NR_PSELECT6_TIME64 413
#define LINUX_NR_PPOLL_TIME64 414
#define LINUX_ARM_NR_CACHEFLUSH 0x000f0002u
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
#define LINUX_NR_MKNOD 14
#define LINUX_NR_FCHMOD 94
#define LINUX_NR_LCHOWN32 198
#define LINUX_NR_FCHOWN32 207
#define LINUX_NR_CHOWN32 212
#define LINUX_NR_MKNODAT 324
#define LINUX_NR_FCHOWNAT 325
#define LINUX_NR_FACCESSAT2 439
#define LINUX_AT_FDCWD (-100)
#define LINUX_AT_EACCESS 0x200u
#define LINUX_AT_EMPTY_PATH 0x1000u
#define LINUX_S_IFMT 0170000u
#define LINUX_S_IFREG 0100000u
#define LINUX_S_IFCHR 0020000u
#define LINUX_S_IFBLK 0060000u
#define LINUX_S_IFIFO 0010000u
#define LINUX_S_IFSOCK 0140000u
#define LINUX_AT_SYMLINK_NOFOLLOW 0x100u
#define LINUX_AT_REMOVEDIR 0x200u
#define LINUX_AT_SYMLINK_FOLLOW 0x400u
#define LINUX_O_CREAT 0x00000040u
#define LINUX_O_EXCL 0x00000080u
#define LINUX_O_NOFOLLOW 0x00008000u
#define LINUX_O_NONBLOCK 0x00000800u
#define LINUX_O_CLOEXEC 0x00080000u
#define LINUX_O_DIRECT 0x00010000u
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

int32_t linux_report_unsupported(uint32_t number, int32_t linux_error,
    const char *detail, const uint32_t *args)
{
    return linux_unsupported(number, linux_error, detail, args);
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

/* QNX dup2 is close plus dup, not atomic against another thread closing or
   replacing the destination: it fails with EBADF or EINVAL.  Linux orders
   dup2, dup3 and close on one number, so the emulator does too. */
static pthread_mutex_t fd_replace_lock = PTHREAD_MUTEX_INITIALIZER;

/* epoll_ctl takes it too, so a registration never races a close or dup2
   of its number (see linux_epoll.c). */
void linux_fd_replace_lock(void)
{
    pthread_mutex_lock(&fd_replace_lock);
}

void linux_fd_replace_unlock(void)
{
    pthread_mutex_unlock(&fd_replace_lock);
}

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
        /* another thread may have held it at fork; its owner is gone here */
        pthread_mutex_init(&fd_replace_lock, 0);
        if (child_stack != 0)
            context->uc_mcontext.cpu.gpr[13] = child_stack;
    }
    return (int32_t)process;
}

int32_t linux_writev_compat(int descriptor, const struct iovec *vectors,
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

/* readv/writev on a typed object with read/write ops (eventfd): the
   first bytes of the vector, at most a small fixed buffer's worth, go
   through the object's op.  Returns 0 when the fd isn't such an object. */
static int typed_vector_io(int fd, const struct iovec *vectors, int count,
    int writing, int32_t *result)
{
    unsigned char buffer[64];
    size_t length = 0, position, part;
    int index, handled;

    if (!guest_fd_is_typed(fd)) return 0;
    if (!writing && linux_inotify_readv(fd, vectors, count, result))
        return 1;
    if (count < 0 || count > 1024) { *result = -EINVAL; return 1; }
    for (index = 0; index != count && length != sizeof(buffer); ++index) {
        part = vectors[index].iov_len;
        if (part > sizeof(buffer) - length) part = sizeof(buffer) - length;
        if (writing) memcpy(buffer + length, vectors[index].iov_base, part);
        length += part;
    }
    handled = writing ?
        guest_fd_dispatch_write(fd, buffer, length, result) :
        guest_fd_dispatch_read(fd, buffer, length, result);
    if (!handled || writing || *result <= 0) return handled;
    for (index = 0, position = 0; position != (size_t)*result; ++index) {
        part = vectors[index].iov_len;
        if (part > (size_t)*result - position)
            part = (size_t)*result - position;
        memcpy(vectors[index].iov_base, buffer + position, part);
        position += part;
    }
    return 1;
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
            fd = guest_fd_admit(open(host_path, host_flags,
                (mode_t)(mode & 07777u)));
            if (fd >= 0) {
                if (guest_fd_record_path(fd, host_path) != 0)
                    guest_fd_clear(fd);
                return fd;
            }
            if (errno == ENOENT || errno == EMFILE) {
                saved_errno = errno;
                break;
            }
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
    fd = guest_fd_admit(open(host_path, host_flags, (mode_t)(mode & 07777u)));
    if (fd >= 0 && guest_fd_record_path(fd, host_path) != 0)
        guest_fd_clear(fd);
    return linux_host_result(fd);
}

int32_t linux_host_stat_at(int directory_fd, const char *guest_path,
    uint32_t at_flags, struct stat64 *out)
{
    char host_path[PATH_MAX];
    int follow = (at_flags & LINUX_AT_SYMLINK_NOFOLLOW) == 0;
    int result;

    if ((at_flags & ~(LINUX_AT_SYMLINK_NOFOLLOW | 0x1000u)) != 0)
        return -EINVAL;
    if (guest_path == 0) return -EFAULT;
    if ((at_flags & 0x1000u) != 0 && guest_path[0] == '\0') {
        if (directory_fd == LINUX_AT_FDCWD) {
            if (resolve_at(directory_fd, ".", 0, 0, host_path,
                    sizeof(host_path)) != 0)
                return -(int32_t)linux_errno_number(errno);
            result = stat64(host_path, out);
        } else {
            if (guest_fd_is_hidden(directory_fd)) return -EBADF;
            result = fstat64(directory_fd, out);
        }
        return result != 0 ? -(int32_t)linux_errno_number(errno) : 0;
    }
    if (resolve_at(directory_fd, guest_path, 0, !follow, host_path,
            sizeof(host_path)) != 0)
        return -(int32_t)linux_errno_number(errno);
    result = follow ? stat64(host_path, out) : lstat64(host_path, out);
    if (result != 0) return -(int32_t)linux_errno_number(errno);
    return 0;
}

static int32_t linux_stat_path(int directory_fd, const char *guest_path,
    void *guest_status, int follow)
{
    struct stat64 status;
    int32_t result = linux_host_stat_at(directory_fd, guest_path,
        follow ? 0 : LINUX_AT_SYMLINK_NOFOLLOW, &status);
    if (result != 0) return result;
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

static int32_t linux_fchmod(int descriptor, uint32_t mode)
{
    struct stat64 status;
    int saved_errno;

    if (guest_fd_is_hidden(descriptor)) return -EBADF;
    if (fchmod(descriptor, (mode_t)(mode & 07777u)) == 0) return 0;
    saved_errno = errno;
    /* Linux changes only a socket inode's mode, which no path shows; QNX
       refuses with EPERM, which callers read as a permission problem.
       Refuse as unsupported instead, so they chmod the bound path. */
    if (saved_errno == EPERM && fstat64(descriptor, &status) == 0 &&
            S_ISSOCK(status.st_mode)) {
        const uint32_t args[6] = { (uint32_t)descriptor, mode, 0, 0, 0, 0 };
        return linux_unsupported(LINUX_NR_FCHMOD, LINUX_EOPNOTSUPP,
            "fchmod-socket", args);
    }
    return -(int32_t)linux_errno_number(saved_errno);
}

/* Permission check against a status record, for the AT_EMPTY_PATH forms. */
static int32_t access_from_status(const struct stat64 *status, uint32_t mode)
{
    uint32_t bits;
    if (mode == 0 || geteuid() == 0) return 0;
    if (status->st_uid == geteuid()) bits = (status->st_mode >> 6) & 7u;
    else if (status->st_gid == getegid()) bits = (status->st_mode >> 3) & 7u;
    else bits = status->st_mode & 7u;
    return (mode & ~bits) == 0 ? 0 : -EACCES;
}

static int32_t linux_faccessat2(int directory_fd, const char *guest_path,
    uint32_t mode, uint32_t flags)
{
    char host_path[PATH_MAX];
    struct stat64 status;
    if ((mode & ~7u) != 0 || (flags & ~(LINUX_AT_EACCESS |
            LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_EMPTY_PATH)) != 0)
        return -EINVAL;
    if (guest_path == 0) return -EFAULT;
    if ((flags & LINUX_AT_EMPTY_PATH) != 0 && guest_path[0] == '\0') {
        int32_t result = linux_host_stat_at(directory_fd, guest_path,
            LINUX_AT_EMPTY_PATH, &status);
        return result != 0 ? result : access_from_status(&status, mode);
    }
    if ((flags & LINUX_AT_SYMLINK_NOFOLLOW) != 0) {
        /* A symbolic link itself is always accessible (mode 0777). */
        int32_t result = linux_host_stat_at(directory_fd, guest_path,
            LINUX_AT_SYMLINK_NOFOLLOW, &status);
        if (result != 0) return result;
        if (S_ISLNK(status.st_mode)) return 0;
    }
    if (resolve_at(directory_fd, guest_path, 0, 0, host_path,
            sizeof(host_path)) != 0)
        return -(int32_t)linux_errno_number(errno);
    return linux_host_result(access(host_path, (int)mode));
}

int32_t linux_chown_at(int directory_fd, const char *guest_path,
    uint32_t uid, uint32_t gid, uint32_t at_flags)
{
    char host_path[PATH_MAX];
    int nofollow = (at_flags & LINUX_AT_SYMLINK_NOFOLLOW) != 0;
    if ((at_flags & ~(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_EMPTY_PATH)) != 0)
        return -EINVAL;
    if (guest_path == 0) return -EFAULT;
    if ((at_flags & LINUX_AT_EMPTY_PATH) != 0 && guest_path[0] == '\0') {
        if (directory_fd == LINUX_AT_FDCWD) {
            if (resolve_at(directory_fd, ".", 0, 0, host_path,
                    sizeof(host_path)) != 0)
                return -(int32_t)linux_errno_number(errno);
            return linux_host_result(chown(host_path, (uid_t)uid,
                (gid_t)gid));
        }
        if (guest_fd_is_hidden(directory_fd)) return -EBADF;
        return linux_host_result(fchown(directory_fd, (uid_t)uid,
            (gid_t)gid));
    }
    if (resolve_at(directory_fd, guest_path, 0, nofollow, host_path,
            sizeof(host_path)) != 0)
        return -(int32_t)linux_errno_number(errno);
    return linux_host_result(nofollow ?
        lchown(host_path, (uid_t)uid, (gid_t)gid) :
        chown(host_path, (uid_t)uid, (gid_t)gid));
}

static int32_t linux_fchown(int descriptor, uint32_t uid, uint32_t gid)
{
    if (guest_fd_is_hidden(descriptor)) return -EBADF;
    return linux_host_result(fchown(descriptor, (uid_t)uid, (gid_t)gid));
}

int32_t linux_mknod_at(int directory_fd, const char *guest_path,
    uint32_t mode, uint32_t device)
{
    char host_path[PATH_MAX];
    uint32_t type = mode & LINUX_S_IFMT;
    mode_t permissions = (mode_t)(mode & 07777u);
    if (guest_path == 0) return -EFAULT;
    if (type == LINUX_S_IFSOCK) {
        const uint32_t args[6] = { (uint32_t)directory_fd,
            (uint32_t)(uintptr_t)guest_path, mode, device, 0, 0 };
        return linux_unsupported(LINUX_NR_MKNODAT, LINUX_EPERM,
            "mknod-socket", args);
    }
    if (type == LINUX_S_IFCHR || type == LINUX_S_IFBLK) return -EPERM;
    if (type != 0 && type != LINUX_S_IFREG && type != LINUX_S_IFIFO)
        return -EINVAL;
    if (resolve_at(directory_fd, guest_path, 1, 1, host_path,
            sizeof(host_path)) != 0)
        return -(int32_t)linux_errno_number(errno);
    if (type == LINUX_S_IFIFO)
        return linux_host_result(mkfifo(host_path, permissions));
    {
        int fd = open(host_path, O_WRONLY | O_CREAT | O_EXCL | O_LARGEFILE,
            permissions);
        if (fd < 0) return -(int32_t)linux_errno_number(errno);
        close(fd);
        return 0;
    }
}

static int32_t linux_utimensat(int directory_fd, const char *guest_path,
    const uint32_t *guest_times, uint32_t flags)
{
    char host_path[PATH_MAX];
    struct stat64 status;
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
        if (lstat64(host_path, &status) != 0)
            return -(int32_t)linux_errno_number(errno);
        if (S_ISLNK(status.st_mode)) {
            const uint32_t args[6] = { (uint32_t)directory_fd,
                (uint32_t)(uintptr_t)guest_path,
                (uint32_t)(uintptr_t)guest_times, flags, 0, 0 };
            return linux_unsupported(LINUX_NR_UTIMENSAT, LINUX_EOPNOTSUPP,
                "utimensat-symlink", args);
        }
    } else if (stat64(host_path, &status) != 0)
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

static int32_t linux_fstatfs64(int descriptor, size_t guest_size,
    void *guest_buffer)
{
    struct statvfs64 status;

    if (guest_fd_is_hidden(descriptor)) return -EBADF;
    if (guest_size != 84 && guest_size != 88) return -EINVAL;
    if (guest_buffer == 0) return -EFAULT;
    if (fstatvfs64(descriptor, &status) != 0)
        return -(int32_t)linux_errno_number(errno);
    return linux_statfs64_store(&status, guest_size, guest_buffer);
}

static int32_t linux_fstat_fd(int fd, void *guest_status)
{
    struct stat64 status;
    if (fstat64(fd, &status) != 0) return -(int32_t)linux_errno_number(errno);
    linux_stat64_store(guest_status, &status);
    return 0;
}

static int32_t linux_getdents64(int fd, void *guest_buffer, size_t capacity)
{
    unsigned char *buffer = guest_buffer;
    size_t used = 0;
    struct guest_fd_object *object = guest_fd_acquire(fd);
    int32_t result = 0;

    if (object == 0) return -EBADF;
    for (;;) {
        uint64_t inode;
        int64_t offset;
        const char *name;
        size_t name_length, record_length;
        uint16_t short_length;
        int status = guest_fd_object_directory_entry(object, &inode, &name,
            &offset);
        if (status < 0) {
            if (used == 0) result = -(int32_t)linux_errno_number(errno);
            break;
        }
        if (status == 0) break;
        name_length = strlen(name) + 1;
        record_length = (19 + name_length + 7) & ~(size_t)7;
        if (record_length > capacity - used) {
            /* Linux: a buffer too small for one entry is EINVAL. */
            if (used == 0) result = -EINVAL;
            break;
        }
        short_length = (uint16_t)record_length;
        memset(buffer + used, 0, record_length);
        memcpy(buffer + used, &inode, 8);
        memcpy(buffer + used + 8, &offset, 8);
        memcpy(buffer + used + 16, &short_length, 2);
        buffer[used + 18] = 0;
        memcpy(buffer + used + 19, name, name_length);
        used += record_length;
        guest_fd_object_directory_advance(object);
    }
    guest_fd_put(object);
    return result != 0 ? result : (int32_t)used;
}

static int32_t linux_close_fd(int fd)
{
    int32_t result;
    if (guest_fd_is_hidden(fd)) return -EBADF;
    pthread_mutex_lock(&fd_replace_lock);
    guest_fd_clear(fd);
    result = linux_host_result(close(fd));
    pthread_mutex_unlock(&fd_replace_lock);
    return result;
}

/* Linux O_ASYNC (ARM EABI) and QNX's own value. */
#define LINUX_O_ASYNC 0x00002000u
#ifdef O_ASYNC
#define HOST_O_ASYNC O_ASYNC
#else
#define HOST_O_ASYNC 0x10000
#endif

static int32_t linux_fcntl64(int fd, int command, uint32_t argument)
{
    int result;
    int typed_status;
    if (guest_fd_is_hidden(fd)) return -EBADF;
    if ((command == 0 || command == 1030) &&
            (int)argument >= guest_fd_hidden_base()) return -EINVAL;
    switch (command) {
    case 0: result = guest_fd_admit(fcntl(fd, F_DUPFD, (int)argument)); break;
    case 1030:
        result = guest_fd_admit(fcntl(fd, F_DUPFD, (int)argument));
        if (result >= 0 && fcntl(result, F_SETFD, FD_CLOEXEC) < 0) {
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
        if (result >= 0)
            return (int32_t)(linux_status_flags(result) |
                ((result & HOST_O_ASYNC) != 0 ? LINUX_O_ASYNC : 0u));
        break;
    case 4: {
        int requested;
        int current;
        int async_allowed = 0;
        typed_status = guest_fd_dispatch_get_status(fd, &current);
        if (typed_status == 0) current = fcntl(fd, F_GETFL);
        if (current < 0) return linux_host_result(current);
        if (linux_open_flags(argument & (0x00000400u | 0x00000800u),
                &requested) != 0) return -EINVAL;
        if ((argument & LINUX_O_ASYNC) != 0) {
            /* Only AF_UNIX sockets (linux_socket_async_capable):
               elsewhere QNX would never send SIGIO, or (AF_INET) would
               send it to other processes.  Clearing it is fine. */
            if (!linux_socket_async_capable(fd)) {
                uint32_t arguments[6] = { 0, 0, 0, 0, 0, 0 };
                arguments[0] = (uint32_t)fd;
                arguments[1] = (uint32_t)command;
                arguments[2] = argument;
                return linux_report_unsupported(221, EINVAL,
                    "async I/O (O_ASYNC) is only supported on AF_UNIX sockets",
                    arguments);
            }
            async_allowed = 1;
        }
        current &= ~(O_APPEND | O_NONBLOCK | HOST_O_ASYNC);
        current |= requested & (O_APPEND | O_NONBLOCK);
        if (async_allowed) current |= HOST_O_ASYNC;
        typed_status = guest_fd_dispatch_set_status(fd, current);
        result = typed_status == 0 ? fcntl(fd, F_SETFL, current) :
            typed_status > 0 ? 0 : -1;
        if (result > 0) result = 0;
        break;
    }
    case 12:
    case 13:
    case 14:
    case 36:
    case 37:
    case 38: {
        /* 36/37/38 are the open-file-description locks.  QNX has only
           per-process record locks, so they reuse the 12/13/14 path:
           an approximation (locks belong to the process, not the open
           file description; closing any descriptor of the file drops
           them).  The request pid must be 0, as on Linux. */
        int ofd = command >= 36;
        int base = ofd ? command - 24 : command;
        struct linux_flock64 guest_lock;
        struct flock64 host_lock;
        if (argument == 0) return -EFAULT;
        memcpy(&guest_lock, (const void *)argument, sizeof(guest_lock));
        memset(&host_lock, 0, sizeof(host_lock));
        if (guest_lock.type == 0) host_lock.l_type = F_RDLCK;
        else if (guest_lock.type == 1) host_lock.l_type = F_WRLCK;
        else if (guest_lock.type == 2) host_lock.l_type = F_UNLCK;
        else return -EINVAL;
        if (guest_lock.whence < SEEK_SET || guest_lock.whence > SEEK_END)
            return -EINVAL;
        if (ofd && guest_lock.pid != 0) return -EINVAL;
        host_lock.l_whence = guest_lock.whence;
        host_lock.l_start = (off64_t)guest_lock.start;
        host_lock.l_len = (off64_t)guest_lock.length;
        result = fcntl(fd, base == 12 ? F_GETLK64 :
            base == 13 ? F_SETLK64 : F_SETLKW64, &host_lock);
        if (result == 0 && base == 12) {
            guest_lock.type = host_lock.l_type == F_RDLCK ? 0 :
                host_lock.l_type == F_WRLCK ? 1 : 2;
            guest_lock.whence = host_lock.l_whence;
            guest_lock.start = (int64_t)host_lock.l_start;
            guest_lock.length = (int64_t)host_lock.l_len;
            guest_lock.pid = ofd ? -1 : host_lock.l_pid;
            memcpy((void *)argument, &guest_lock, sizeof(guest_lock));
        }
        break;
    }
    case 8:
    case 9: {
        /* F_SETOWN/F_GETOWN: forwarded for AF_UNIX sockets only.
           Guest pids are QNX pids and QNX's F_GETOWN returns exactly the
           pid set, so nothing is translated.  QNX refuses a process-group
           owner (negative: EOPNOTSUPP, and no signal); pipes and ptys
           answer ENOSYS, and AF_INET is refused (io-pkt SIGIO leak): all
           are Linux EINVAL, reported. */
        uint32_t arguments[6] = { 0, 0, 0, 0, 0, 0 };
        arguments[0] = (uint32_t)fd;
        arguments[1] = (uint32_t)command;
        arguments[2] = argument;
        if (!linux_socket_async_capable(fd)) {
            if (fcntl(fd, F_GETFD) < 0) return linux_host_result(-1);
            return linux_report_unsupported(221, EINVAL,
                "fcntl-owner: only AF_UNIX sockets support F_SETOWN", arguments);
        }
        if (command == 9) {
            result = fcntl(fd, F_GETOWN);
            break;
        }
        if ((int32_t)argument < 0)
            return linux_report_unsupported(221, EINVAL,
                "fcntl-owner: process-group owners are not supported",
                arguments);
        result = fcntl(fd, F_SETOWN, (int)argument);
        if (result > 0) result = 0;
        break;
    }
    case 1032:
    case 1031: {
        /* F_GETPIPE_SZ / F_SETPIPE_SZ: the QNX pipe buffer is fixed, so
           report it and accept a request that fits in it. */
        struct stat64 status;
        long size;
        if (fstat64(fd, &status) != 0) return linux_host_result(-1);
        if (!S_ISFIFO(status.st_mode)) return -EBADF;
        size = fpathconf(fd, _PC_PIPE_BUF);
        if (size <= 0) size = 5120;
        if (command == 1031 && argument > (uint32_t)size) {
            uint32_t arguments[6] = { 0, 0, 0, 0, 0, 0 };
            arguments[0] = (uint32_t)fd;
            arguments[1] = (uint32_t)command;
            arguments[2] = argument;
            return linux_report_unsupported(221, EPERM, "pipe-size",
                arguments);
        }
        return (int32_t)size;
    }
    case 1033:
    case 1034: {
        /* F_ADD_SEALS / F_GET_SEALS: no memfd, so no sealable file. */
        uint32_t arguments[6] = { 0, 0, 0, 0, 0, 0 };
        arguments[0] = (uint32_t)fd;
        arguments[1] = (uint32_t)command;
        arguments[2] = argument;
        return linux_report_unsupported(221, EINVAL, "fcntl-seals",
            arguments);
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

    if ((flags & LINUX_O_DIRECT) != 0) {
        /* packet-mode pipes have no QNX equivalent */
        const uint32_t args[6] = { (uint32_t)(uintptr_t)guest_descriptors,
            flags, 0, 0, 0, 0 };
        return linux_unsupported(LINUX_NR_PIPE2, LINUX_EINVAL,
            "pipe2-packet", args);
    }
    if ((flags & ~(LINUX_O_NONBLOCK | LINUX_O_CLOEXEC)) != 0) return -EINVAL;
    if (pipe(descriptors) != 0) return -(int32_t)linux_errno_number(errno);
    if (guest_fd_is_hidden(descriptors[0]) || guest_fd_is_hidden(descriptors[1])) {
        close(descriptors[0]);
        close(descriptors[1]);
        return -EMFILE;
    }
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

void linux_syscall_register_fd_types(void)
{
    linux_eventfd_register();
    linux_timerfd_register();
    linux_signalfd_register();
    linux_inotify_register();
    linux_epoll_register();
}

static int32_t linux_execve(const char *guest_path, char *const guest_argv[],
    char *const guest_envp[])
{
    char host_path[PATH_MAX];
    uint32_t report_args[6] = { (uint32_t)(uintptr_t)guest_path,
        (uint32_t)(uintptr_t)guest_argv, (uint32_t)(uintptr_t)guest_envp,
        0, 0, 0 };

    if (guest_path_resolve(guest_path, 0, host_path, sizeof(host_path)) != 0)
        return -(int32_t)linux_errno_number(errno);
    /* Known limit: QNX drops an armed ITIMER_REAL at exec, Linux keeps it.
       Record it for LINUXEMU_DIAGNOSTICS; the exec itself proceeds. */
    linux_resource_note_exec(report_args);
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

/* QNX's waitpid can return a reaped child while procnto is still tearing
   it down (more often for a large, multi-threaded process, which every
   Linuxemu guest is), and kill(pid, 0) then still succeeds for a moment.
   On Linux the pid is gone once wait returns, and callers check that
   (libuv's spawn tests do), so wait for ESRCH: yield a little, then sleep
   in 1 ms steps, for at most about 200 ms. */
static void wait_until_reaped(pid_t child)
{
    const struct timespec step = { 0, 1000000 };
    int saved_errno = errno;
    unsigned attempt;

    for (attempt = 0; attempt != 300; ++attempt) {
        if (kill(child, 0) != 0) break;
        if (attempt < 100) sched_yield();
        else nanosleep(&step, 0);
    }
    errno = saved_errno;
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
    if (result != 0 && (WIFEXITED(host_status) || WIFSIGNALED(host_status)))
        wait_until_reaped(result);
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
    int copy_file;
    void *result;
    off64_t offset;
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
    offset = (off64_t)context->uc_mcontext.cpu.gpr[5] * 4096;
    host_descriptor = (int)context->uc_mcontext.cpu.gpr[4];
    /* Private file mappings that are executable or writable are read in
       with pread instead of mapped from the file: executable ones because
       of the code patching, writable ones because QNX's MAP_PRIVATE file
       mapping can return wrong pages (FINDINGS 2026-11-21: a library's
       data segment, its dynamic section included, differed from pread of
       the same bytes for a few minutes, and musl crashed in symbol lookup).
       Read-only private mappings stay mapped from the file. */
    copy_file =
        (linux_flags & LINUX_MAP_PRIVATE) != 0 &&
        (linux_flags & LINUX_MAP_ANONYMOUS) == 0 &&
        (host_protection & (PROT_EXEC | PROT_WRITE)) != 0;
    if (copy_file) {
        host_flags |= MAP_ANON;
        host_descriptor = -1;
    }
    mapping_protection = host_protection;
    if ((linux_flags & LINUX_MAP_PRIVATE) != 0 &&
        (linux_flags & LINUX_MAP_ANONYMOUS) == 0)
        mapping_protection =
            (host_protection | PROT_READ | PROT_WRITE) & ~PROT_EXEC;
    result = mmap64((void *)requested_address, requested_length,
        mapping_protection, host_flags,
        host_descriptor, copy_file ? 0 : offset);
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
    if (copy_file) {
        struct stat64 status;
        size_t file_bytes;
        int descriptor = (int)context->uc_mcontext.cpu.gpr[4];
        if (fstat64(descriptor, &status) != 0) {
            int saved_errno = errno;
            munmap(result, mapped_length);
            return -(int32_t)linux_errno_number(saved_errno);
        }
        if (status.st_size < 0 || offset < 0 ||
            (offset > status.st_size && (host_protection & PROT_EXEC) != 0)) {
            munmap(result, mapped_length);
            return -EINVAL;
        }
        /* Writable: a mapping that starts past the end of the file is
           zeros (Linux maps it; a touch would raise SIGBUS there). */
        file_bytes = requested_length;
        if (offset >= status.st_size) file_bytes = 0;
        else if ((off64_t)file_bytes > status.st_size - offset)
            file_bytes = (size_t)(status.st_size - offset);
        if (file_bytes != 0 && read_exact_at(descriptor, result,
                file_bytes, offset) != 0) {
            int saved_errno = errno;
            munmap(result, mapped_length);
            /* A write-only fd: Linux's mmap refuses it with EACCES. */
            if (saved_errno == EBADF) return -EACCES;
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
    if (guest_memory_runtime_map_kind(mapped_address, mapped_length,
            (host_protection & PROT_EXEC) != 0,
            (linux_flags & LINUX_MAP_ANONYMOUS) != 0 &&
                (linux_flags & LINUX_MAP_PRIVATE) != 0,
            host_protection == (PROT_READ | PROT_WRITE)) != 0) {
        int saved_errno = errno;
        trace_internal_failure("linuxemu: runtime mapping registry full\n");
        arm_patch_forget_range(mapped_address, mapped_length);
        munmap(result, mapped_length);
        return -(int32_t)linux_errno_number(saved_errno);
    }
    return (int32_t)mapped_address;
}

/* LINUXEMU_PATCH_ANONYMOUS=1: patch anonymous memory made executable, as
   before Phase 4 part 1, for a program that generates system calls. */
static int patch_anonymous_memory;

void linux_syscall_set_patch_anonymous(int enabled)
{
    patch_anonymous_memory = enabled != 0;
}

/* With LINUXEMU_DIAGNOSTICS on, report anonymous memory being made
   executable that holds a word arm_patch_range would rewrite: a generated
   Linux svc, or a JIT data word that only looks like one. Read-only. */
static void report_anonymous_exec_candidates(uintptr_t address, size_t length)
{
    uintptr_t end = address + length;
    uintptr_t cursor = address;

    if (!linux_diag_is_enabled()) return;
    while (cursor < end) {
        uintptr_t run_end;
        uintptr_t first;
        int anonymous = guest_memory_runtime_anonymous_at(cursor, end,
            &run_end);
        if (anonymous && arm_patch_candidate(cursor, run_end - cursor,
                &first)) {
            uint32_t args[6] = { (uint32_t)first, (uint32_t)address,
                (uint32_t)length, 0, 0, 0 };
            linux_diag_record(125, 0, "anon-exec-svc", args);
            return;
        }
        cursor = run_end;
    }
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
        /* Run by run. File-backed memory was patched, by mapping symbol,
           when it was mapped, so it isn't scanned again (an ARM-only rescan
           would rewrite Thumb code's data). Anonymous memory (a JIT's code
           and data) is left alone unless LINUXEMU_PATCH_ANONYMOUS=1,
           because a data word can't be told from an svc and JIT code makes
           no system calls itself. */
        uintptr_t end = address + mapped_length;
        uintptr_t cursor = address;
        while (cursor < end) {
            uintptr_t run_end;
            int anonymous = guest_memory_runtime_anonymous_at(cursor, end,
                &run_end);
            /* Every run is made writable for the icache sync: QNX's
               msync(MS_INVALIDATE_ICACHE) fails ENOMEM on a range that
               isn't writable and holds a page never touched (a JIT's
               PROT_NONE reservation, a partly written code area). */
            int failed = mprotect((void *)cursor, run_end - cursor,
                host_protection | PROT_READ | PROT_WRITE) != 0 ||
                (anonymous && patch_anonymous_memory &&
                    arm_patch_range(cursor, run_end - cursor) != 0) ||
                msync((void *)cursor, run_end - cursor,
                    MS_INVALIDATE_ICACHE) != 0;
            if (failed) {
                int saved_errno = errno;
                arm_patch_rollback(patches_before);
                mprotect((void *)address, mapped_length, host_protection);
                return -(int32_t)linux_errno_number(saved_errno);
            }
            cursor = run_end;
        }
    }
    if (mprotect((void *)address, mapped_length, host_protection) != 0) {
        int saved_errno = errno;
        if ((host_protection & PROT_EXEC) != 0)
            arm_patch_rollback(patches_before);
        return -(int32_t)linux_errno_number(saved_errno);
    }
    /* After the final protection, so the read can't fault on memory that
       was PROT_NONE; execute-only memory isn't read. */
    if ((host_protection & (PROT_EXEC | PROT_READ)) ==
            (PROT_EXEC | PROT_READ))
        report_anonymous_exec_candidates(address, mapped_length);
    if (guest_memory_runtime_protect(address, mapped_length,
            (host_protection & PROT_EXEC) != 0,
            host_protection == (PROT_READ | PROT_WRITE)) != 0)
        return -(int32_t)linux_errno_number(errno);
    return 0;
}

static int32_t linux_cacheflush(uint32_t start, uint32_t end, uint32_t flags)
{
    uintptr_t page = (uintptr_t)runtime_page_size();
    uintptr_t first, last;
    if (flags != 0 || start >= end) return -EINVAL;
    first = (uintptr_t)start & ~(page - 1u);
    last = ((uintptr_t)end + page - 1u) & ~(page - 1u);
    if (last <= first) return -EINVAL;
    if (!guest_memory_runtime_owned(first, last - first) &&
        !guest_memory_range_owned(first, last - first)) return -EINVAL;
    if (msync((void *)first, last - first, MS_INVALIDATE_ICACHE) != 0)
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

#define LINUX_NR_MREMAP 163
#define LINUX_MREMAP_MAYMOVE 1u
#define LINUX_MREMAP_FIXED 2u
#define LINUX_MREMAP_DONTUNMAP 4u

/* mremap for private anonymous read-write guest memory only. An old range
   that is not wholly a tracked mapping gets EFAULT (Linux's error for an
   unmapped range) unless other guest memory covers it, which is the
   unsupported-kind EINVAL; executable, file-backed and non-read-write
   mappings keep patch, icache or protection state and are refused. */
static int32_t linux_mremap(uintptr_t old_address, size_t old_length,
    size_t new_length, uint32_t flags, uintptr_t new_address)
{
    uintptr_t page = (uintptr_t)runtime_page_size();
    uint32_t args[5] = { (uint32_t)old_address, (uint32_t)old_length,
        (uint32_t)new_length, flags, (uint32_t)new_address };
    size_t old_size;
    size_t new_size;
    void *result;
    uintptr_t mapped;
    int32_t status;

    if ((flags & ~(LINUX_MREMAP_MAYMOVE | LINUX_MREMAP_FIXED |
            LINUX_MREMAP_DONTUNMAP)) != 0) return -EINVAL;
    if ((flags & (LINUX_MREMAP_FIXED | LINUX_MREMAP_DONTUNMAP)) != 0)
        return linux_unsupported(LINUX_NR_MREMAP, LINUX_EINVAL,
            "mremap-kind", args);
    if ((old_address & (page - 1u)) != 0 || new_length == 0 ||
        old_length > SIZE_MAX - (page - 1u) ||
        new_length > SIZE_MAX - (page - 1u)) return -EINVAL;
    if (old_length == 0)
        return linux_unsupported(LINUX_NR_MREMAP, LINUX_EINVAL,
            "mremap-kind", args);
    old_size = align_up(old_length, page);
    new_size = align_up(new_length, page);
    if (!guest_memory_runtime_owned(old_address, old_size)) {
        if (guest_memory_range_owned(old_address, old_size))
            return linux_unsupported(LINUX_NR_MREMAP, LINUX_EINVAL,
                "mremap-kind", args);
        return -EFAULT;
    }
    if (!guest_memory_runtime_is_plain(old_address, old_size))
        return linux_unsupported(LINUX_NR_MREMAP, LINUX_EINVAL,
            "mremap-kind", args);
    /* Memory that was ever executable carries UDF traps and site records
       (a W^X flip back to RW leaves them): never shrink, grow or move it. */
    if (arm_patch_any_in_range(old_address, old_size))
        return linux_unsupported(LINUX_NR_MREMAP, LINUX_EINVAL,
            "mremap-kind", args);
    if (new_size == old_size) return (int32_t)old_address;
    if (new_size < old_size) {
        status = linux_munmap_runtime(old_address + new_size,
            old_size - new_size);
        return status != 0 ? status : (int32_t)old_address;
    }
    if (old_size <= GUEST_MAX_ADDRESS - old_address &&
        new_size - old_size <=
            GUEST_MAX_ADDRESS - (old_address + old_size)) {
        /* Grow in place: a hinted, non-fixed mmap only lands on the hint when
           that range is free, so no other mapping can be overwritten. */
        result = mmap((void *)(old_address + old_size), new_size - old_size,
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (result != MAP_FAILED) {
            if ((uintptr_t)result == old_address + old_size) {
                if (guest_memory_runtime_map_kind(old_address + old_size,
                        new_size - old_size, 0, 1, 1) != 0) {
                    munmap(result, new_size - old_size);
                    return -ENOMEM;
                }
                return (int32_t)old_address;
            }
            munmap(result, new_size - old_size);
        }
    }
    if ((flags & LINUX_MREMAP_MAYMOVE) == 0) return -ENOMEM;
    result = mmap(NULL, new_size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (result == MAP_FAILED) return -ENOMEM;
    mapped = (uintptr_t)result;
    if (mapped < GUEST_MIN_ADDRESS || mapped >= GUEST_MAX_ADDRESS ||
        new_size > GUEST_MAX_ADDRESS - mapped) {
        munmap(result, new_size);
        return -ENOMEM;
    }
    /* A racing guest munmap of the old range can fault the emulator in this
       copy; Linux serialises mremap against munmap with mmap_lock, we do not. */
    memcpy(result, (const void *)old_address, old_size);
    if (guest_memory_runtime_map_kind(mapped, new_size, 0, 1, 1) != 0) {
        munmap(result, new_size);
        return -ENOMEM;
    }
    status = linux_munmap_runtime(old_address, old_size);
    if (status != 0) {
        guest_memory_runtime_unmap(mapped, new_size);
        munmap(result, new_size);
        return status;
    }
    return (int32_t)mapped;
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
        /* After each guest I/O call: an EPOLLET item on the fd re-arms
           (one load when none waits for it). */
        linux_epoll_note_io((int)context->uc_mcontext.cpu.gpr[0]);
        break;
    case LINUX_NR_WRITE:
        if (!guest_fd_dispatch_write((int)context->uc_mcontext.cpu.gpr[0],
                (const void *)context->uc_mcontext.cpu.gpr[1],
                (size_t)context->uc_mcontext.cpu.gpr[2], &result))
            result = linux_result(write(
            (int)context->uc_mcontext.cpu.gpr[0],
            (const void *)context->uc_mcontext.cpu.gpr[1],
            (size_t)context->uc_mcontext.cpu.gpr[2]));
        linux_epoll_note_io((int)context->uc_mcontext.cpu.gpr[0]);
        break;
    case LINUX_NR_PREAD64: {
        uint64_t offset = (uint64_t)context->uc_mcontext.cpu.gpr[4] |
            ((uint64_t)context->uc_mcontext.cpu.gpr[5] << 32);
        result = linux_result(pread64((int)context->uc_mcontext.cpu.gpr[0],
            (void *)context->uc_mcontext.cpu.gpr[1],
            (size_t)context->uc_mcontext.cpu.gpr[2], (off64_t)offset));
        linux_epoll_note_io((int)context->uc_mcontext.cpu.gpr[0]);
        break;
    }
    case LINUX_NR_PWRITE64: {
        uint64_t offset = (uint64_t)context->uc_mcontext.cpu.gpr[4] |
            ((uint64_t)context->uc_mcontext.cpu.gpr[5] << 32);
        /* QNX extends (zero-fills) a file to the offset of a zero-length
           pwrite; Linux leaves it alone.  Offset 0 keeps QNX's descriptor
           checks without moving the size. */
        if (context->uc_mcontext.cpu.gpr[2] == 0) {
            if ((int64_t)offset < 0) { result = -EINVAL; break; }
            offset = 0;
        }
        result = linux_result(pwrite64((int)context->uc_mcontext.cpu.gpr[0],
            (const void *)context->uc_mcontext.cpu.gpr[1],
            (size_t)context->uc_mcontext.cpu.gpr[2], (off64_t)offset));
        linux_epoll_note_io((int)context->uc_mcontext.cpu.gpr[0]);
        break;
    }
    case 361: case 362: case 392: case 393: {
        /* preadv/pwritev: pos_l, pos_h in r3, r4 (no pair alignment); the
           *2 calls add flags in r5 */
        uint64_t offset = (uint64_t)context->uc_mcontext.cpu.gpr[3] |
            ((uint64_t)context->uc_mcontext.cpu.gpr[4] << 32);
        int kind = number >= 392;
        uint32_t flags = kind ? context->uc_mcontext.cpu.gpr[5] : 0;
        if (number == 361 || number == 392)
            result = linux_preadv((int)context->uc_mcontext.cpu.gpr[0],
                (const struct iovec *)context->uc_mcontext.cpu.gpr[1],
                (int32_t)context->uc_mcontext.cpu.gpr[2], (int64_t)offset,
                flags, kind);
        else
            result = linux_pwritev((int)context->uc_mcontext.cpu.gpr[0],
                (const struct iovec *)context->uc_mcontext.cpu.gpr[1],
                (int32_t)context->uc_mcontext.cpu.gpr[2], (int64_t)offset,
                flags, kind);
        break;
    }
    case 352:
        result = linux_fallocate((int)context->uc_mcontext.cpu.gpr[0],
            context->uc_mcontext.cpu.gpr[1],
            (int64_t)(((uint64_t)context->uc_mcontext.cpu.gpr[3] << 32) |
                context->uc_mcontext.cpu.gpr[2]),
            (int64_t)(((uint64_t)context->uc_mcontext.cpu.gpr[5] << 32) |
                context->uc_mcontext.cpu.gpr[4]));
        break;
    case 239:
        result = linux_sendfile64((int)context->uc_mcontext.cpu.gpr[0],
            (int)context->uc_mcontext.cpu.gpr[1],
            (int64_t *)context->uc_mcontext.cpu.gpr[2],
            context->uc_mcontext.cpu.gpr[3]);
        break;
    case LINUX_NR_FSYNC:
    case LINUX_NR_FDATASYNC:
        /* QNX 10.3 has fsync but no distinct fdatasync operation. */
        result = linux_host_result(fsync(
            (int)context->uc_mcontext.cpu.gpr[0]));
        break;
    case LINUX_NR_FTRUNCATE64: {
        uint64_t length = (uint64_t)context->uc_mcontext.cpu.gpr[2] |
            ((uint64_t)context->uc_mcontext.cpu.gpr[3] << 32);
        result = linux_host_result(ftruncate64(
            (int)context->uc_mcontext.cpu.gpr[0], (off64_t)length));
        break;
    }
    case LINUX_NR_READV:
        if (!typed_vector_io((int)context->uc_mcontext.cpu.gpr[0],
                (const struct iovec *)context->uc_mcontext.cpu.gpr[1],
                (int)context->uc_mcontext.cpu.gpr[2], 0, &result))
            result = linux_result(readv((int)context->uc_mcontext.cpu.gpr[0],
                (const struct iovec *)context->uc_mcontext.cpu.gpr[1],
                (int)context->uc_mcontext.cpu.gpr[2]));
        linux_epoll_note_io((int)context->uc_mcontext.cpu.gpr[0]);
        break;
    case LINUX_NR_WRITEV:
        if (!typed_vector_io((int)context->uc_mcontext.cpu.gpr[0],
                (const struct iovec *)context->uc_mcontext.cpu.gpr[1],
                (int)context->uc_mcontext.cpu.gpr[2], 1, &result))
            result = linux_writev_compat(
                (int)context->uc_mcontext.cpu.gpr[0],
                (const struct iovec *)context->uc_mcontext.cpu.gpr[1],
                (int)context->uc_mcontext.cpu.gpr[2]);
        linux_epoll_note_io((int)context->uc_mcontext.cpu.gpr[0]);
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
    case LINUX_NR_FCHMOD:
        result = linux_fchmod((int32_t)context->uc_mcontext.cpu.gpr[0],
            context->uc_mcontext.cpu.gpr[1]);
        break;
    case LINUX_NR_FACCESSAT2:
        result = linux_faccessat2((int32_t)context->uc_mcontext.cpu.gpr[0],
            (const char *)context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2], context->uc_mcontext.cpu.gpr[3]);
        break;
    case LINUX_NR_CHOWN32:
    case LINUX_NR_LCHOWN32:
        result = linux_chown_at(LINUX_AT_FDCWD,
            (const char *)context->uc_mcontext.cpu.gpr[0],
            context->uc_mcontext.cpu.gpr[1], context->uc_mcontext.cpu.gpr[2],
            number == LINUX_NR_LCHOWN32 ? LINUX_AT_SYMLINK_NOFOLLOW : 0);
        break;
    case LINUX_NR_FCHOWN32:
        result = linux_fchown((int32_t)context->uc_mcontext.cpu.gpr[0],
            context->uc_mcontext.cpu.gpr[1], context->uc_mcontext.cpu.gpr[2]);
        break;
    case LINUX_NR_FCHOWNAT:
        result = linux_chown_at((int32_t)context->uc_mcontext.cpu.gpr[0],
            (const char *)context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2], context->uc_mcontext.cpu.gpr[3],
            context->uc_mcontext.cpu.gpr[4]);
        break;
    case LINUX_NR_MKNOD:
        result = linux_mknod_at(LINUX_AT_FDCWD,
            (const char *)context->uc_mcontext.cpu.gpr[0],
            context->uc_mcontext.cpu.gpr[1], context->uc_mcontext.cpu.gpr[2]);
        break;
    case LINUX_NR_MKNODAT:
        result = linux_mknod_at((int32_t)context->uc_mcontext.cpu.gpr[0],
            (const char *)context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2], context->uc_mcontext.cpu.gpr[3]);
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
        off64_t offset = lseek64((int)context->uc_mcontext.cpu.gpr[0],
            (off64_t)unsigned_offset, (int)context->uc_mcontext.cpu.gpr[4]);
        if (offset == (off64_t)-1) result = -linux_errno_number(errno);
        else {
            uint64_t stored = (uint64_t)offset;
            memcpy((void *)context->uc_mcontext.cpu.gpr[3], &stored, 8);
            result = 0;
        }
        break;
    }
    case LINUX_NR_DUP:
        result = linux_host_result(guest_fd_admit(
            dup((int)context->uc_mcontext.cpu.gpr[0])));
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
        else if (guest_fd_is_hidden((int)context->uc_mcontext.cpu.gpr[0]) ||
                guest_fd_is_hidden((int)context->uc_mcontext.cpu.gpr[1]))
            result = -EBADF;
        else {
            int source = (int)context->uc_mcontext.cpu.gpr[0];
            int destination = (int)context->uc_mcontext.cpu.gpr[1];
            pthread_mutex_lock(&fd_replace_lock);
            if (fcntl(source, F_GETFD, 0) < 0)
                result = -(int32_t)linux_errno_number(errno);
            else {
                /* before the number names the new file */
                guest_fd_retire(destination);
                result = linux_host_result(dup2(source, destination));
            }
            if (result >= 0 &&
                (int)context->uc_mcontext.cpu.gpr[0] != result)
                (void)guest_fd_duplicate((int)context->uc_mcontext.cpu.gpr[0], result);
            pthread_mutex_unlock(&fd_replace_lock);
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
            (int)context->uc_mcontext.cpu.gpr[1], LINUX_NR_KILL,
            (const uint32_t *)context->uc_mcontext.cpu.gpr);
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
    case LINUX_NR_MREMAP:
        result = linux_mremap(context->uc_mcontext.cpu.gpr[0],
            (size_t)context->uc_mcontext.cpu.gpr[1],
            (size_t)context->uc_mcontext.cpu.gpr[2],
            context->uc_mcontext.cpu.gpr[3],
            context->uc_mcontext.cpu.gpr[4]);
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
    case LINUX_NR_RECVMMSG:
    case LINUX_NR_SENDMMSG:
        result = linux_socket_syscall(number, context->uc_mcontext.cpu.gpr);
        break;
    case LINUX_NR_GETRANDOM:
        result = linux_getrandom((void *)context->uc_mcontext.cpu.gpr[0],
            (size_t)context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2]);
        break;
    case 75:
    case 77:
    case 104:
    case 105:
    case 116:
    case 191:
    case 96:
    case 97:
    case 154:
    case 155:
    case 156:
    case 157:
    case 172:
    case 184:
    case 185:
    case 241:
    case 242:
    case 389:
    case LINUX_NR_PRLIMIT64:
        result = linux_resource_syscall(number, context->uc_mcontext.cpu.gpr);
        break;
    case 266:
        result = linux_statfs64((const char *)context->uc_mcontext.cpu.gpr[0],
            (size_t)context->uc_mcontext.cpu.gpr[1],
            (void *)context->uc_mcontext.cpu.gpr[2]);
        break;
    case 397:
        result = linux_statx((int32_t)context->uc_mcontext.cpu.gpr[0],
            (const char *)context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2], context->uc_mcontext.cpu.gpr[3],
            (void *)context->uc_mcontext.cpu.gpr[4]);
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
            const char *link_path =
                (const char *)context->uc_mcontext.cpu.gpr[1];
            /* Paths from the guest's root or cwd take readlink's route,
               which knows the emulated /proc links. */
            if ((int32_t)context->uc_mcontext.cpu.gpr[0] == LINUX_AT_FDCWD ||
                    (link_path != 0 && link_path[0] == '/')) {
                result = guest_path_readlink(link_path,
                    (char *)context->uc_mcontext.cpu.gpr[2],
                    (size_t)context->uc_mcontext.cpu.gpr[3]);
                if (result < 0) result = -linux_errno_number(errno);
            } else if (resolve_at((int32_t)context->uc_mcontext.cpu.gpr[0],
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
            (int)context->uc_mcontext.cpu.gpr[1], LINUX_NR_TKILL,
            (const uint32_t *)context->uc_mcontext.cpu.gpr);
        break;
    case LINUX_NR_TGKILL:
        result = guest_signal_send_thread(
            (int32_t)context->uc_mcontext.cpu.gpr[0],
            (int32_t)context->uc_mcontext.cpu.gpr[1],
            (int)context->uc_mcontext.cpu.gpr[2], LINUX_NR_TGKILL,
            (const uint32_t *)context->uc_mcontext.cpu.gpr);
        break;
    case LINUX_NR_RT_TGSIGQUEUEINFO:
        result = guest_signal_queue_thread(
            (int32_t)context->uc_mcontext.cpu.gpr[0],
            (int32_t)context->uc_mcontext.cpu.gpr[1],
            (int)context->uc_mcontext.cpu.gpr[2],
            (const void *)(uintptr_t)context->uc_mcontext.cpu.gpr[3],
            LINUX_NR_RT_TGSIGQUEUEINFO, (const uint32_t *)context->uc_mcontext.cpu.gpr);
        break;
    case LINUX_NR_RT_SIGQUEUEINFO:
        result = guest_signal_queue_process(
            (pid_t)(int32_t)context->uc_mcontext.cpu.gpr[0],
            (int)context->uc_mcontext.cpu.gpr[1],
            (const void *)(uintptr_t)context->uc_mcontext.cpu.gpr[2],
            LINUX_NR_RT_SIGQUEUEINFO,
            (const uint32_t *)context->uc_mcontext.cpu.gpr);
        break;
    case LINUX_NR_SIGNALFD:
        result = linux_signalfd((int)context->uc_mcontext.cpu.gpr[0],
            (const void *)(uintptr_t)context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2], 0);
        break;
    case LINUX_NR_SIGNALFD4:
        result = linux_signalfd((int)context->uc_mcontext.cpu.gpr[0],
            (const void *)(uintptr_t)context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2], context->uc_mcontext.cpu.gpr[3]);
        break;
    case LINUX_NR_PERSONALITY:
        result = (int32_t)guest_process_personality(
            context->uc_mcontext.cpu.gpr[0]);
        break;
    case LINUX_NR_TIME:
    case LINUX_NR_GETTIMEOFDAY:
    case LINUX_NR_NANOSLEEP:
    case LINUX_NR_CLOCK_SETTIME:
    case LINUX_NR_CLOCK_SETTIME64:
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
        else if (guest_fd_is_hidden(source) || guest_fd_is_hidden(destination))
            result = -EBADF;
        else if (fcntl(source, F_GETFD, 0) < 0)
            result = -(int32_t)linux_errno_number(errno);
        else {
            pthread_mutex_lock(&fd_replace_lock);
            guest_fd_retire(destination);
            result = linux_host_result(dup2(source, destination));
            if (result >= 0 && (flags & LINUX_O_CLOEXEC) != 0 &&
                fcntl(destination, F_SETFD, FD_CLOEXEC) != 0)
                result = -(int32_t)linux_errno_number(errno);
            if (result >= 0) (void)guest_fd_duplicate(source, destination);
            pthread_mutex_unlock(&fd_replace_lock);
        }
        break;
    }
    case LINUX_NR_EVENTFD:
        result = linux_eventfd_create(context->uc_mcontext.cpu.gpr[0], 0);
        break;
    case LINUX_NR_EVENTFD2:
        result = linux_eventfd_create(context->uc_mcontext.cpu.gpr[0],
            context->uc_mcontext.cpu.gpr[1]);
        break;
    case LINUX_NR_INOTIFY_INIT:
        result = linux_inotify_init(0);
        break;
    case LINUX_NR_INOTIFY_INIT1:
        result = linux_inotify_init(context->uc_mcontext.cpu.gpr[0]);
        break;
    case LINUX_NR_INOTIFY_ADD_WATCH:
        result = linux_inotify_add_watch((int)context->uc_mcontext.cpu.gpr[0],
            (const char *)context->uc_mcontext.cpu.gpr[1],
            context->uc_mcontext.cpu.gpr[2]);
        break;
    case LINUX_NR_INOTIFY_RM_WATCH:
        result = linux_inotify_rm_watch((int)context->uc_mcontext.cpu.gpr[0],
            (int32_t)context->uc_mcontext.cpu.gpr[1]);
        break;
    case LINUX_NR_TIMERFD_CREATE:
        result = linux_timerfd_create((int32_t)context->uc_mcontext.cpu.gpr[0],
            context->uc_mcontext.cpu.gpr[1]);
        break;
    case LINUX_NR_TIMERFD_SETTIME:
    case LINUX_NR_TIMERFD_SETTIME64:
        result = linux_timerfd_settime((int)context->uc_mcontext.cpu.gpr[0],
            context->uc_mcontext.cpu.gpr[1],
            (const void *)context->uc_mcontext.cpu.gpr[2],
            (void *)context->uc_mcontext.cpu.gpr[3],
            number == LINUX_NR_TIMERFD_SETTIME64);
        break;
    case LINUX_NR_TIMERFD_GETTIME:
    case LINUX_NR_TIMERFD_GETTIME64:
        result = linux_timerfd_gettime((int)context->uc_mcontext.cpu.gpr[0],
            (void *)context->uc_mcontext.cpu.gpr[1],
            number == LINUX_NR_TIMERFD_GETTIME64);
        break;
    case LINUX_NR_EPOLL_CREATE:
        result = (int32_t)context->uc_mcontext.cpu.gpr[0] <= 0 ? -LINUX_EINVAL :
            linux_epoll_create(0);
        break;
    case LINUX_NR_EPOLL_CREATE1:
        result = linux_epoll_create(context->uc_mcontext.cpu.gpr[0]);
        break;
    case LINUX_NR_EPOLL_CTL:
        result = linux_epoll_ctl((int)context->uc_mcontext.cpu.gpr[0],
            (int)context->uc_mcontext.cpu.gpr[1],
            (int)context->uc_mcontext.cpu.gpr[2],
            (const void *)context->uc_mcontext.cpu.gpr[3]);
        break;
    case LINUX_NR_EPOLL_WAIT:
        result = linux_epoll_wait((int)context->uc_mcontext.cpu.gpr[0],
            (void *)context->uc_mcontext.cpu.gpr[1],
            (int)context->uc_mcontext.cpu.gpr[2],
            (int)context->uc_mcontext.cpu.gpr[3], 0, 0);
        break;
    case LINUX_NR_EPOLL_PWAIT:
        result = linux_epoll_wait((int)context->uc_mcontext.cpu.gpr[0],
            (void *)context->uc_mcontext.cpu.gpr[1],
            (int)context->uc_mcontext.cpu.gpr[2],
            (int)context->uc_mcontext.cpu.gpr[3],
            (const void *)context->uc_mcontext.cpu.gpr[4],
            context->uc_mcontext.cpu.gpr[5]);
        break;
    case LINUX_NR_EPOLL_PWAIT2: {
        int timeout;
        result = linux_poll_timespec_timeout(
            (const void *)context->uc_mcontext.cpu.gpr[3], 1, &timeout);
        if (result == 0)
            result = linux_epoll_wait((int)context->uc_mcontext.cpu.gpr[0],
                (void *)context->uc_mcontext.cpu.gpr[1],
                (int)context->uc_mcontext.cpu.gpr[2], timeout,
                (const void *)context->uc_mcontext.cpu.gpr[4],
                context->uc_mcontext.cpu.gpr[5]);
        break;
    }
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
    case LINUX_ARM_NR_CACHEFLUSH:
        result = linux_cacheflush(context->uc_mcontext.cpu.gpr[0],
            context->uc_mcontext.cpu.gpr[1], context->uc_mcontext.cpu.gpr[2]);
        break;
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
