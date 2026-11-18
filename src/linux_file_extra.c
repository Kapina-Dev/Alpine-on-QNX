/* File calls QNX lacks: vector positional I/O and sendfile64, emulated with
   loops.  These run inside the SIGILL handler: no malloc, no stdio. */
#include "linuxemu.h"
#include "guest_fd.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#define LINUX_NR_PREADV 361
#define LINUX_NR_PWRITEV 362
#define LINUX_NR_PREADV2 392
#define LINUX_NR_PWRITEV2 393
#define LINUX_EBADF 9
#define LINUX_EFAULT 14
#define LINUX_EINVAL 22
#define LINUX_EFBIG 27
#define LINUX_NR_FALLOCATE 352
#define LINUX_EOPNOTSUPP 95
#define LINUX_RWF_DSYNC 2u
#define LINUX_RWF_SYNC 4u
#define LINUX_IOV_MAX 1024
#define SENDFILE_CHUNK (4 * 1024)
#define SENDFILE_MAX 0x7ffff000u

static int32_t host_errno_result(void)
{
    return -(int32_t)linux_errno_number(errno);
}

static int32_t vector_transfer(int fd, const struct iovec *vector,
    int32_t count, int64_t offset, uint32_t flags, int vectored_kind,
    int writing)
{
    uint32_t number = writing ?
        (vectored_kind ? LINUX_NR_PWRITEV2 : LINUX_NR_PWRITEV) :
        (vectored_kind ? LINUX_NR_PREADV2 : LINUX_NR_PREADV);
    uint32_t allowed = writing ? (LINUX_RWF_DSYNC | LINUX_RWF_SYNC) : 0;
    uint32_t arguments[6];
    int64_t total = 0;
    int32_t index;

    if (guest_fd_is_hidden(fd)) return -LINUX_EBADF;
    if (count < 0 || count > LINUX_IOV_MAX) return -LINUX_EINVAL;
    if (count > 0 && vector == 0) return -LINUX_EFAULT;
    if (!vectored_kind) flags = 0;
    if ((flags & ~allowed) != 0) {
        arguments[0] = (uint32_t)fd;
        arguments[1] = (uint32_t)(uintptr_t)vector;
        arguments[2] = (uint32_t)count;
        arguments[3] = (uint32_t)offset;
        arguments[4] = (uint32_t)((uint64_t)offset >> 32);
        arguments[5] = flags;
        return linux_report_unsupported(number, LINUX_EOPNOTSUPP,
            "rwf-flags", arguments);
    }
    if (vectored_kind && offset == -1) {
        /* the current file position, updated: the plain vector calls */
        int32_t result;
        if (writing) result = linux_writev_compat(fd, vector, count);
        else {
            ssize_t got = readv(fd, vector, count);
            result = got < 0 ? host_errno_result() : (int32_t)got;
        }
        if (writing && result > 0 && flags != 0 && fsync(fd) != 0)
            return host_errno_result();
        return result;
    }
    if (offset < 0) return -LINUX_EINVAL;
    for (index = 0; index != count; ++index) {
        if (vector[index].iov_len > (size_t)INT32_MAX - (size_t)total)
            return -LINUX_EINVAL;
        total += (int64_t)vector[index].iov_len;
    }
    if (offset > INT64_MAX - total) return -LINUX_EINVAL;

    total = 0;
    for (index = 0; index != count; ++index) {
        size_t length = vector[index].iov_len;
        ssize_t moved;
        if (length == 0) continue;
        moved = writing ?
            pwrite64(fd, vector[index].iov_base, length,
                (off64_t)(offset + total)) :
            pread64(fd, vector[index].iov_base, length,
                (off64_t)(offset + total));
        if (moved < 0) {
            if (total > 0) break;
            return host_errno_result();
        }
        total += moved;
        if ((size_t)moved < length) break;
    }
    if (writing && total > 0 && flags != 0 && fsync(fd) != 0)
        return host_errno_result();
    return (int32_t)total;
}

int32_t linux_preadv(int fd, const struct iovec *vector, int32_t count,
    int64_t offset, uint32_t flags, int vectored_kind)
{
    return vector_transfer(fd, vector, count, offset, flags, vectored_kind, 0);
}

int32_t linux_pwritev(int fd, const struct iovec *vector, int32_t count,
    int64_t offset, uint32_t flags, int vectored_kind)
{
    return vector_transfer(fd, vector, count, offset, flags, vectored_kind, 1);
}

int32_t linux_sendfile64(int out_fd, int in_fd, int64_t *offset_pointer,
    uint32_t count)
{
    unsigned char buffer[SENDFILE_CHUNK];
    int64_t offset = 0;
    uint32_t total = 0;
    int32_t error = 0;

    if (guest_fd_is_hidden(out_fd) || guest_fd_is_hidden(in_fd))
        return -LINUX_EBADF;
    if (lseek64(in_fd, 0, SEEK_CUR) < 0) {
        uint32_t arguments[6];
        /* a bad descriptor and the like keep their own error */
        if (errno != ESPIPE) return host_errno_result();
        /* pipe, socket or tty input: Linux needs an mmappable file */
        arguments[0] = (uint32_t)out_fd;
        arguments[1] = (uint32_t)in_fd;
        arguments[2] = (uint32_t)(uintptr_t)offset_pointer;
        arguments[3] = count;
        arguments[4] = arguments[5] = 0;
        return linux_report_unsupported(239, LINUX_EINVAL,
            "sendfile-input", arguments);
    }
    if (offset_pointer != 0) {
        memcpy(&offset, offset_pointer, sizeof(offset));
        if (offset < 0) return -LINUX_EINVAL;
    }
    if (count > SENDFILE_MAX) count = SENDFILE_MAX;
    while (total < count) {
        uint32_t chunk = count - total;
        ssize_t got;
        size_t written = 0;
        ssize_t put;
        if (chunk > SENDFILE_CHUNK) chunk = SENDFILE_CHUNK;
        got = offset_pointer != 0 ?
            pread64(in_fd, buffer, chunk, (off64_t)(offset + total)) :
            read(in_fd, buffer, chunk);
        if (got < 0) {
            error = host_errno_result();
            break;
        }
        if (got == 0) break;
        /* One write: a short count means a signal arrived or a nonblocking
           output filled; writing the rest would block again with the
           signal already consumed. */
        put = write(out_fd, buffer, (size_t)got);
        if (put < 0) error = host_errno_result();
        else written = (size_t)put;
        total += (uint32_t)written;
        if (written < (size_t)got) {
            /* the input position moved past what was delivered: put it back
               so a restart or the caller's next call sees the true state */
            if (offset_pointer == 0 && lseek64(in_fd,
                    -(off64_t)((size_t)got - written), SEEK_CUR) < 0)
                error = host_errno_result();
            break;
        }
    }
    if (offset_pointer != 0 && total > 0) {
        int64_t advanced = offset + total;
        memcpy(offset_pointer, &advanced, sizeof(advanced));
    }
    if (total > 0) return (int32_t)total;
    return error;
}

int32_t linux_fallocate(int fd, uint32_t mode, int64_t offset, int64_t length)
{
    int error;
    if (guest_fd_is_hidden(fd)) return -LINUX_EBADF;
    if (offset < 0 || length <= 0) return -LINUX_EINVAL;
    if (offset > INT64_MAX - length) return -LINUX_EFBIG;
    if (mode != 0) {
        /* KEEP_SIZE, punch hole, collapse and zero range have no QNX
           equivalent; posix_fallocate only extends. */
        uint32_t arguments[6];
        arguments[0] = (uint32_t)fd;
        arguments[1] = mode;
        arguments[2] = (uint32_t)offset;
        arguments[3] = (uint32_t)((uint64_t)offset >> 32);
        arguments[4] = (uint32_t)length;
        arguments[5] = (uint32_t)((uint64_t)length >> 32);
        return linux_report_unsupported(LINUX_NR_FALLOCATE,
            LINUX_EOPNOTSUPP, "fallocate-mode", arguments);
    }
    /* posix_fallocate returns the error number and leaves errno alone.
       The core is built with a 32-bit off_t, so call the 64-bit variant
       to keep offsets above 4 GiB intact. */
    error = posix_fallocate64(fd, (off64_t)offset, (off64_t)length);
    return error == 0 ? 0 : -(int32_t)linux_errno_number(error);
}
