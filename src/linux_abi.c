#include "linuxemu.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#define LINUX_O_ACCMODE 0x00000003u
#define LINUX_O_CREAT 0x00000040u
#define LINUX_O_EXCL 0x00000080u
#define LINUX_O_NOCTTY 0x00000100u
#define LINUX_O_TRUNC 0x00000200u
#define LINUX_O_APPEND 0x00000400u
#define LINUX_O_NONBLOCK 0x00000800u
#define LINUX_O_DSYNC 0x00001000u
#define LINUX_O_DIRECTORY 0x00004000u
#define LINUX_O_NOFOLLOW 0x00008000u
#define LINUX_O_DIRECT 0x00010000u
#define LINUX_O_LARGEFILE 0x00020000u
#define LINUX_O_CLOEXEC 0x00080000u

int linux_errno_number(int value)
{
    switch (value) {
    case 0: return 0;
    case EPERM: return 1;
    case ENOENT: return 2;
    case ESRCH: return 3;
    case EINTR: return 4;
    case EIO: return 5;
    case ENXIO: return 6;
    case E2BIG: return 7;
    case ENOEXEC: return 8;
    case EBADF: return 9;
    case ECHILD: return 10;
    case EAGAIN: return 11;
    case ENOMEM: return 12;
    case EACCES: return 13;
    case EFAULT: return 14;
    case EBUSY: return 16;
    case EEXIST: return 17;
    case EXDEV: return 18;
    case ENODEV: return 19;
    case ENOTDIR: return 20;
    case EISDIR: return 21;
    case EINVAL: return 22;
    case ENFILE: return 23;
    case EMFILE: return 24;
    case ENOTTY: return 25;
    case EFBIG: return 27;
    case ENOSPC: return 28;
    case ESPIPE: return 29;
    case EROFS: return 30;
    case EMLINK: return 31;
    case EPIPE: return 32;
    case ERANGE: return 34;
    case ENAMETOOLONG: return 36;
    case ENOSYS: return 38;
    case ENOTEMPTY: return 39;
    case ELOOP: return 40;
    case ENOTSOCK: return 88;
    case EDESTADDRREQ: return 89;
    case EMSGSIZE: return 90;
    case EPROTOTYPE: return 91;
    case ENOPROTOOPT: return 92;
    case EPROTONOSUPPORT: return 93;
    case ESOCKTNOSUPPORT: return 94;
    case EOPNOTSUPP: return 95;
    case EPFNOSUPPORT: return 96;
    case EAFNOSUPPORT: return 97;
    case EADDRINUSE: return 98;
    case EADDRNOTAVAIL: return 99;
    case ENETDOWN: return 100;
    case ENETUNREACH: return 101;
    case ENETRESET: return 102;
    case ECONNABORTED: return 103;
    case ECONNRESET: return 104;
    case ENOBUFS: return 105;
    case EISCONN: return 106;
    case ENOTCONN: return 107;
    case ESHUTDOWN: return 108;
    case ETOOMANYREFS: return 109;
    case ETIMEDOUT: return 110;
    case ECONNREFUSED: return 111;
    case EHOSTDOWN: return 112;
    case EHOSTUNREACH: return 113;
#ifdef EALREADY_NEW
    case EALREADY_NEW: return 114;
#else
    case EALREADY: return 114;
#endif
    case EINPROGRESS: return 115;
    default: return 5;
    }
}

int linux_open_flags(uint32_t linux_flags, int *host_flags)
{
    uint32_t known = LINUX_O_ACCMODE | LINUX_O_CREAT | LINUX_O_EXCL |
        LINUX_O_NOCTTY | LINUX_O_TRUNC | LINUX_O_APPEND | LINUX_O_NONBLOCK |
        LINUX_O_DSYNC | LINUX_O_DIRECTORY | LINUX_O_NOFOLLOW |
        LINUX_O_DIRECT | LINUX_O_LARGEFILE | LINUX_O_CLOEXEC;
    int result;

    if ((linux_flags & ~known) != 0 ||
        (linux_flags & LINUX_O_ACCMODE) == LINUX_O_ACCMODE) return -1;
    switch (linux_flags & LINUX_O_ACCMODE) {
    case 0: result = O_RDONLY; break;
    case 1: result = O_WRONLY; break;
    default: result = O_RDWR; break;
    }
    if ((linux_flags & LINUX_O_CREAT) != 0) result |= O_CREAT;
    if ((linux_flags & LINUX_O_EXCL) != 0) result |= O_EXCL;
    if ((linux_flags & LINUX_O_NOCTTY) != 0) result |= O_NOCTTY;
    if ((linux_flags & LINUX_O_TRUNC) != 0) result |= O_TRUNC;
    if ((linux_flags & LINUX_O_APPEND) != 0) result |= O_APPEND;
    if ((linux_flags & LINUX_O_NONBLOCK) != 0) result |= O_NONBLOCK;
#ifdef O_DSYNC
    if ((linux_flags & LINUX_O_DSYNC) != 0) result |= O_DSYNC;
#endif
#ifdef O_DIRECTORY
    if ((linux_flags & LINUX_O_DIRECTORY) != 0) result |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    if ((linux_flags & LINUX_O_NOFOLLOW) != 0) result |= O_NOFOLLOW;
#endif
#ifdef O_DIRECT
    if ((linux_flags & LINUX_O_DIRECT) != 0) result |= O_DIRECT;
#endif
#ifdef O_CLOEXEC
    if ((linux_flags & LINUX_O_CLOEXEC) != 0) result |= O_CLOEXEC;
#endif
    *host_flags = result;
    return 0;
}

uint32_t linux_status_flags(int host_flags)
{
    uint32_t result;

    switch (host_flags & O_ACCMODE) {
    case O_WRONLY: result = 1; break;
    case O_RDWR: result = 2; break;
    default: result = 0; break;
    }
    if ((host_flags & O_APPEND) != 0) result |= LINUX_O_APPEND;
    if ((host_flags & O_NONBLOCK) != 0) result |= LINUX_O_NONBLOCK;
#ifdef O_DSYNC
    if ((host_flags & O_DSYNC) != 0) result |= LINUX_O_DSYNC;
#endif
    return result | LINUX_O_LARGEFILE;
}

static void store_u32(unsigned char *buffer, size_t offset, uint32_t value)
{
    memcpy(buffer + offset, &value, sizeof(value));
}

static void store_u64(unsigned char *buffer, size_t offset, uint64_t value)
{
    memcpy(buffer + offset, &value, sizeof(value));
}

void linux_stat64_store(void *guest_buffer, const struct stat *status)
{
    unsigned char *buffer = guest_buffer;
    memset(buffer, 0, 96);
    store_u64(buffer, 0, (uint64_t)status->st_dev);
    store_u32(buffer, 12, (uint32_t)status->st_ino);
    store_u32(buffer, 16, (uint32_t)status->st_mode);
    store_u32(buffer, 20, (uint32_t)status->st_nlink);
    store_u32(buffer, 24, (uint32_t)status->st_uid);
    store_u32(buffer, 28, (uint32_t)status->st_gid);
    store_u64(buffer, 32, (uint64_t)status->st_rdev);
    store_u64(buffer, 44, (uint64_t)status->st_size);
    store_u32(buffer, 52, (uint32_t)status->st_blksize);
    store_u64(buffer, 56, (uint64_t)status->st_blocks);
    store_u32(buffer, 64, (uint32_t)status->st_atime);
    store_u32(buffer, 72, (uint32_t)status->st_mtime);
    store_u32(buffer, 80, (uint32_t)status->st_ctime);
    store_u64(buffer, 88, (uint64_t)status->st_ino);
}

void linux_rusage_store(void *guest_buffer, const struct rusage *usage)
{
    unsigned char *buffer = guest_buffer;
    const long values[14] = {
        usage->ru_maxrss, usage->ru_ixrss, usage->ru_idrss, usage->ru_isrss,
        usage->ru_minflt, usage->ru_majflt, usage->ru_nswap, usage->ru_inblock,
        usage->ru_oublock, usage->ru_msgsnd, usage->ru_msgrcv,
        usage->ru_nsignals, usage->ru_nvcsw, usage->ru_nivcsw
    };
    size_t i;

    memset(buffer, 0, 72);
    store_u32(buffer, 0, (uint32_t)usage->ru_utime.tv_sec);
    store_u32(buffer, 4, (uint32_t)usage->ru_utime.tv_usec);
    store_u32(buffer, 8, (uint32_t)usage->ru_stime.tv_sec);
    store_u32(buffer, 12, (uint32_t)usage->ru_stime.tv_usec);
    for (i = 0; i != 14; ++i)
        store_u32(buffer, 16 + i * 4, (uint32_t)values[i]);
}
