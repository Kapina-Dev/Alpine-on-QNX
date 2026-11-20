/*
 * The Linux inotify (roadmap Phase 3, part 3) on QNX's native inotify.
 *
 * QNX's libc has inotify_init, inotify_add_watch and inotify_rm_watch, and
 * its event records, mask bits and watch descriptors are Linux's (FINDINGS
 * 2026-11-20, QNX native inotify).  So the guest's inotify fd is the native
 * one, and the GUEST_FD_INOTIFY object holds no state of its own: the
 * queue, the watches and O_NONBLOCK all live on the native open-file
 * description.  Duplicates, fork copies and adopted copies are correct by
 * construction, and every inotify fd sharing /dev/fsnotify's identity (two
 * instances cannot be told apart by fstat) is harmless: whichever object a
 * number adopts, it behaves the same.
 *
 * What the object adds:
 *   - guest paths: add_watch resolves the guest path (relative to the
 *     guest's cwd, through guest symlinks, or to the link itself with
 *     IN_DONT_FOLLOW) to a host path before the native call;
 *   - Linux's errors where QNX differs: a mask without event bits and
 *     IN_MASK_ADD|IN_MASK_CREATE are EINVAL, IN_MASK_CREATE alone EINVAL
 *     with a diagnostic; a device node, FIFO or socket (never passed to
 *     QNX, where FIFOs and some ptys block the call) and a filesystem
 *     without change notification (native ENOTTY: /dev/shmem, /proc) are
 *     EOPNOTSUPP with a diagnostic; a bad path pointer is EFAULT;
 *   - read: a nonblocking read of an empty queue returns 0 natively, which
 *     Linux never does: it becomes EAGAIN;
 *   - dup, fork and exec: the native fd carries the queue; the object is
 *     shared by duplicates and fork copies, re-attached after exec, and
 *     attached to a received number with /dev/fsnotify's identity.
 *     SCM_RIGHTS refuses inotify fds (linux_socket.c): QNX cannot pass
 *     one (the receiver fails EACCES and loses the message).
 *
 * Event names are relative to the watched directory, so records pass
 * through untranslated.  Events arrive batched (about 250 ms late).
 * No locks.
 */
#include "linuxemu.h"
#include "guest_fd.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#define LINUX_NR_INOTIFY_ADD_WATCH 317
#define LINUX_IN_NONBLOCK 0x00000800u
#define LINUX_IN_CLOEXEC 0x00080000u
#define LINUX_IN_ALL_EVENTS 0x00000fffu
#define LINUX_IN_ONLYDIR 0x01000000u
#define LINUX_IN_DONT_FOLLOW 0x02000000u
#define LINUX_IN_EXCL_UNLINK 0x04000000u
#define LINUX_IN_MASK_CREATE 0x10000000u
#define LINUX_IN_MASK_ADD 0x20000000u
#define LINUX_IN_ONESHOT 0x80000000u
/* The bits passed to QNX (same values there); others are dropped, as
   Linux ignores unknown bits. */
#define INOTIFY_PASSED_BITS (LINUX_IN_ALL_EVENTS | LINUX_IN_ONLYDIR | \
    LINUX_IN_DONT_FOLLOW | LINUX_IN_EXCL_UNLINK | LINUX_IN_MASK_ADD | \
    LINUX_IN_ONESHOT)
#define PROBE_PAGE 4096u

/* /dev/fsnotify's identity, which every native inotify fd reports. */
static int fsnotify_known;
static dev_t fsnotify_device;
static uint64_t fsnotify_inode;

static int32_t linux_error(int host_errno)
{
    return -(int32_t)linux_errno_number(host_errno);
}

static int32_t inotify_read(struct guest_fd_object *object, int fd,
    void *buffer, size_t length)
{
    ssize_t result = read(fd, buffer, length);
    (void)object;
    /* QNX: 0 for an empty queue on a nonblocking description. */
    if (result == 0) result = -1, errno = EAGAIN;
    return result < 0 ? linux_error(errno) : (int32_t)result;
}

static int inotify_poll_fd(struct guest_fd_object *object, int fd)
{
    (void)object;
    return fd;
}

/* Exec record body: no hidden fd and no path (restore_one reads hidden -1
   and an empty hex path).  The native fd itself survives exec, and
   inotify_restore re-attaches the object in the new image. */
static int inotify_serialize(struct guest_fd_object *object, int fd,
    char *buffer, size_t size)
{
    (void)object;
    (void)fd;
    if (size < 4) return -1;
    memcpy(buffer, "-1:", 4);
    return 3;
}

/* No write op (the native fd is read-only: QNX's error), no status ops
   (the native fcntl serves O_NONBLOCK on the shared description), no
   release (nothing owned). */
static const struct guest_fd_ops inotify_ops = {
    inotify_read, 0, inotify_poll_fd, 0, 0, 0, inotify_serialize, 0, 0
};

int linux_inotify_attach(int fd)
{
    struct guest_fd_object *object = guest_fd_object_new(GUEST_FD_INOTIFY,
        &inotify_ops, fd, 0);
    int result;
    if (object == 0) return -1;
    result = guest_fd_attach(fd, object);
    guest_fd_put(object);
    return result;
}

int linux_inotify_is_native(int fd)
{
    struct stat64 status;
    return fsnotify_known && fstat64(fd, &status) == 0 &&
        status.st_dev == fsnotify_device &&
        (uint64_t)status.st_ino == fsnotify_inode;
}

int32_t linux_inotify_init(uint32_t linux_flags)
{
    int fd, saved_errno;
    if ((linux_flags & ~(LINUX_IN_NONBLOCK | LINUX_IN_CLOEXEC)) != 0)
        return linux_error(EINVAL);
    fd = guest_fd_admit(inotify_init());
    if (fd < 0) return linux_error(errno);
    if ((linux_flags & LINUX_IN_NONBLOCK) != 0) {
        int flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
            goto fail;
    }
    if ((linux_flags & LINUX_IN_CLOEXEC) != 0 &&
            fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) goto fail;
    if (linux_inotify_attach(fd) != 0) goto fail;
    return fd;
fail:
    saved_errno = errno;
    close(fd);
    return linux_error(saved_errno);
}

/* The guest's inotify fd: EBADF for a closed or hidden number, EINVAL for
   any other kind of fd.  1 when it is one.  A number with no object but
   /dev/fsnotify's identity (its exec record was dropped, or the identity
   was unknown when it arrived) gets its object here. */
static int32_t check_inotify_fd(int fd)
{
    struct guest_fd_object *object;
    int is_inotify;
    if (fd < 0 || guest_fd_is_hidden(fd) || fcntl(fd, F_GETFD) < 0)
        return linux_error(EBADF);
    object = guest_fd_acquire(fd);
    is_inotify = object != 0 &&
        guest_fd_object_type(object) == GUEST_FD_INOTIFY;
    if (object == 0 && linux_inotify_is_native(fd) &&
            linux_inotify_attach(fd) == 0)
        is_inotify = 1;
    guest_fd_put(object);
    return is_inotify ? 1 : linux_error(EINVAL);
}

/* Guest memory at address is readable: the loader's and the guest's own
   mappings are known; anything else is probed (msync fails on an unmapped
   page). */
static int page_mapped(uintptr_t address)
{
    uintptr_t first = address & ~(uintptr_t)(PROBE_PAGE - 1);
    if (guest_memory_range_owned(address, 1)) return 1;
    return first != 0 && msync((void *)first, PROBE_PAGE, MS_ASYNC) == 0;
}

/* A guest path string: 0 when its bytes up to the NUL are mapped, else a
   negative Linux errno (EFAULT, or ENAMETOOLONG past PATH_MAX). */
static int32_t check_guest_string(const char *path)
{
    uintptr_t address = (uintptr_t)path;
    size_t scanned = 0;
    if (path == 0) return linux_error(EFAULT);
    for (;;) {
        uintptr_t end = (address | (PROBE_PAGE - 1)) + 1;
        if (!page_mapped(address)) return linux_error(EFAULT);
        for (; address != end; ++address) {
            if (*(const char *)address == '\0') return 0;
            if (++scanned >= PATH_MAX) return linux_error(ENAMETOOLONG);
        }
        if (end == 0) return linux_error(EFAULT);
    }
}

/* A Linux error (EOPNOTSUPP unless given) with a diagnostic note. */
static int32_t unsupported_error(int fd, const char *guest_path,
    uint32_t mask, int host_errno, const char *detail)
{
    uint32_t args[6] = { 0, 0, 0, 0, 0, 0 };
    args[0] = (uint32_t)fd;
    args[1] = (uint32_t)(uintptr_t)guest_path;
    args[2] = mask;
    return linux_report_unsupported(LINUX_NR_INOTIFY_ADD_WATCH,
        (int32_t)linux_errno_number(host_errno), detail, args);
}

static int32_t unsupported(int fd, const char *guest_path, uint32_t mask,
    const char *detail)
{
    return unsupported_error(fd, guest_path, mask, EOPNOTSUPP, detail);
}

int32_t linux_inotify_add_watch(int fd, const char *guest_path, uint32_t mask)
{
    struct stat64 status;
    char host_path[PATH_MAX];
    int32_t checked;
    int wd;
    /* Linux's order: the mask, the fd, then the path. */
    if ((mask & LINUX_IN_ALL_EVENTS) == 0 ||
            (mask & (LINUX_IN_MASK_CREATE | LINUX_IN_MASK_ADD)) ==
            (LINUX_IN_MASK_CREATE | LINUX_IN_MASK_ADD))
        return linux_error(EINVAL);
    checked = check_inotify_fd(fd);
    if (checked < 0) return checked;
    checked = check_guest_string(guest_path);
    if (checked < 0) return checked;
    /* IN_MASK_CREATE alone (Linux 4.18+: fail if the inode already has a
       watch) has no QNX counterpart: EINVAL with a diagnostic. */
    if ((mask & LINUX_IN_MASK_CREATE) != 0)
        return unsupported_error(fd, guest_path, mask, EINVAL,
            "inotify: IN_MASK_CREATE unsupported");
    if (((mask & LINUX_IN_DONT_FOLLOW) != 0 ?
            guest_path_resolve_nofollow(guest_path, host_path,
                sizeof(host_path)) :
            guest_path_resolve(guest_path, 0, host_path,
                sizeof(host_path))) != 0)
        return linux_error(errno);
    /* Device nodes, FIFOs and sockets never reach the native call: on
       some ptys (/dev/ttyp1, /dev/ttyp5) and on a FIFO it blocks until
       a signal arrives (FINDINGS 2026-11-20), and the others (and a bound
       unix socket's path) give ENOTTY. */
    if (((mask & LINUX_IN_DONT_FOLLOW) != 0 ? lstat64(host_path, &status) :
            stat64(host_path, &status)) != 0)
        return linux_error(errno);
    if (S_ISCHR(status.st_mode) || S_ISBLK(status.st_mode))
        return unsupported(fd, guest_path, mask,
            "inotify: device nodes cannot be watched on QNX");
    if (S_ISFIFO(status.st_mode) || S_ISSOCK(status.st_mode))
        return unsupported(fd, guest_path, mask,
            "inotify: FIFOs and sockets cannot be watched on QNX");
    wd = inotify_add_watch(fd, host_path, mask & INOTIFY_PASSED_BITS);
    if (wd >= 0) return wd;
    if (errno == ENOTTY)
        return unsupported(fd, guest_path, mask,
            "inotify: filesystem has no change notification");
    return linux_error(errno);
}

int32_t linux_inotify_rm_watch(int fd, int32_t wd)
{
    int32_t checked = check_inotify_fd(fd);
    if (checked < 0) return checked;
    return inotify_rm_watch(fd, wd) == 0 ? 0 : linux_error(errno);
}

/* readv on an inotify fd is the native readv (the typed vector path's
   small bounce buffer would split records): 0 when fd is no inotify fd. */
int linux_inotify_readv(int fd, const struct iovec *vectors, int count,
    int32_t *result)
{
    struct guest_fd_object *object = guest_fd_acquire(fd);
    ssize_t got;
    int is_inotify = object != 0 &&
        guest_fd_object_type(object) == GUEST_FD_INOTIFY;
    guest_fd_put(object);
    if (!is_inotify) return 0;
    if (count < 0 || count > 1024) {    /* Linux's UIO_MAXIOV */
        *result = linux_error(EINVAL);
        return 1;
    }
    got = readv(fd, vectors, count);
    if (got == 0) got = -1, errno = EAGAIN;
    *result = got < 0 ? linux_error(errno) : (int32_t)got;
    return 1;
}

/* A number with /dev/fsnotify's identity and no record (a received fd
   that adopted nothing) is an inotify fd: 0 when it now has its object. */
static int inotify_identify(int fd)
{
    if (!linux_inotify_is_native(fd)) return -1;
    return linux_inotify_attach(fd);
}

/* Exec restore: the native fd survived exec with its queue and watches;
   the record only says it was an inotify fd, which its identity must
   confirm. */
static int inotify_restore(int fd, int hidden_fd, int second_hidden_fd,
    const char *host_path)
{
    (void)hidden_fd;
    (void)second_hidden_fd;
    (void)host_path;
    return inotify_identify(fd);
}

void linux_inotify_register(void)
{
    struct stat64 status;
    if (stat64("/dev/fsnotify", &status) == 0) {
        fsnotify_device = status.st_dev;
        fsnotify_inode = status.st_ino;
        fsnotify_known = 1;
    }
    guest_fd_register_restore(GUEST_FD_INOTIFY, inotify_restore);
    guest_fd_register_identify(inotify_identify);
}
