/*
 * The full Linux eventfd: counter, semaphore mode, nonblocking I/O
 * (guest fixture, musl).
 *
 * Checks (the exit code is the check number, also printed on stderr with
 * errno):
 *   10 setup failed
 *   11 eventfd2(5, 0): read returns 8 bytes holding 5; on an EFD_NONBLOCK
 *      instance a second read fails EAGAIN
 *   12 writes of 3 and 4, then one read returns 7 (sum, read resets);
 *      writev and readv split 4 + 4 move a value too
 *   13 reserved (blocking I/O is checked in eventfd-blocking.c)
 *   14 EFD_SEMAPHORE with initial 3: three reads return 1, the fourth
 *      (nonblocking) fails EAGAIN
 *   15 a 7-byte read and a 7-byte write fail EINVAL; writing
 *      0xffffffffffffffff fails EINVAL
 *   16 nonblocking, counter at 0xfffffffffffffffe - 1: writing 2 fails
 *      EAGAIN, writing 1 succeeds
 *   17 poll (timeout 0) on a fresh eventfd reports nothing; after a write
 *      it reports POLLIN
 *   18 the libcurl pattern: eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC), write 1
 *      twice, one read returns 2, F_GETFD has FD_CLOEXEC
 *   19 eventfd2(0, 0x4) fails EINVAL; eventfd (351) with initial 2, then a
 *      read returns 2
 *   20 F_SETFL O_NONBLOCK on a blocking instance: the next empty read fails
 *      EAGAIN and F_GETFL shows O_NONBLOCK; ioctl FIONBIO clears and sets
 *      it the same way
 *   21 dup shares the counter and the status flags
 *   22 creating eventfds in a loop reaches at least 50 before failing, the
 *      failure is EMFILE, and after closing them all a new one is created
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#define COUNTER_MAX 0xfffffffffffffffeULL
#define CREATE_CAP 4096

static int fail(int check, const char *what)
{
    fprintf(stderr, "eventfd-semantics check %d failed: %s (errno %d %s)\n",
        check, what, errno, strerror(errno));
    return check;
}

static int read_value(int fd, uint64_t *value)
{
    return read(fd, value, sizeof(*value)) == (ssize_t)sizeof(*value);
}

static int write_value(int fd, uint64_t value)
{
    return write(fd, &value, sizeof(value)) == (ssize_t)sizeof(value);
}

static int read_eagain(int fd)
{
    uint64_t value;
    errno = 0;
    return read(fd, &value, sizeof(value)) == -1 && errno == EAGAIN;
}

static int check_basic(void)
{
    uint64_t value = 0;
    int a = (int)syscall(SYS_eventfd2, 5, 0), b;
    if (a < 0) return fail(11, "eventfd2(5, 0)");
    if (!read_value(a, &value) || value != 5)
        return fail(11, "read of initial 5");
    close(a);
    b = eventfd(5, EFD_NONBLOCK);
    if (b < 0) return fail(11, "eventfd(5, EFD_NONBLOCK)");
    if (!read_value(b, &value) || value != 5)
        return fail(11, "nonblocking read of 5");
    if (!read_eagain(b)) return fail(11, "second nonblocking read not EAGAIN");
    close(b);
    return 0;
}

static int check_sum(void)
{
    uint64_t value = 0;
    int e = eventfd(0, 0);
    if (e < 0) return fail(10, "eventfd(0, 0)");
    if (!write_value(e, 3) || !write_value(e, 4))
        return fail(12, "writes of 3 and 4");
    if (!read_value(e, &value) || value != 7) return fail(12, "read of 7");
    close(e);
    e = eventfd(0, EFD_NONBLOCK);
    if (e < 0) return fail(10, "eventfd(0, EFD_NONBLOCK)");
    if (!write_value(e, 3) || !write_value(e, 4) ||
            !read_value(e, &value) || value != 7 || !read_eagain(e))
        return fail(12, "read did not reset the counter");
    {
        uint64_t five = 5;
        unsigned char *bytes = (unsigned char *)&value;
        struct iovec parts[2];
        parts[0].iov_base = &five;
        parts[0].iov_len = 4;
        parts[1].iov_base = (unsigned char *)&five + 4;
        parts[1].iov_len = 4;
        if (writev(e, parts, 2) != 8) return fail(12, "writev 4 + 4");
        value = 0;
        parts[0].iov_base = bytes;
        parts[1].iov_base = bytes + 4;
        if (readv(e, parts, 2) != 8 || value != 5)
            return fail(12, "readv 4 + 4 of 5");
    }
    close(e);
    return 0;
}

static int check_semaphore(void)
{
    uint64_t value;
    int i, e = eventfd(3, EFD_SEMAPHORE | EFD_NONBLOCK);
    if (e < 0) return fail(14, "eventfd(3, EFD_SEMAPHORE | EFD_NONBLOCK)");
    for (i = 0; i < 3; ++i) {
        value = 0;
        if (!read_value(e, &value) || value != 1)
            return fail(14, "semaphore read did not return 1");
    }
    if (!read_eagain(e)) return fail(14, "fourth semaphore read not EAGAIN");
    close(e);
    return 0;
}

static int check_invalid(void)
{
    char small[7];
    uint64_t all_ones = 0xffffffffffffffffULL;
    int e = eventfd(1, EFD_NONBLOCK);
    if (e < 0) return fail(10, "eventfd(1, EFD_NONBLOCK)");
    memset(small, 0, sizeof(small));
    errno = 0;
    if (read(e, small, sizeof(small)) != -1 || errno != EINVAL)
        return fail(15, "7-byte read not EINVAL");
    errno = 0;
    if (write(e, small, sizeof(small)) != -1 || errno != EINVAL)
        return fail(15, "7-byte write not EINVAL");
    errno = 0;
    if (write(e, &all_ones, sizeof(all_ones)) != -1 || errno != EINVAL)
        return fail(15, "write of 0xffffffffffffffff not EINVAL");
    close(e);
    return 0;
}

static int check_full(void)
{
    uint64_t value = 0;
    int e = eventfd(0, EFD_NONBLOCK);
    if (e < 0) return fail(10, "eventfd(0, EFD_NONBLOCK)");
    if (!write_value(e, COUNTER_MAX - 1))
        return fail(16, "write of max - 1");
    errno = 0;
    if (write_value(e, 2) || errno != EAGAIN)
        return fail(16, "write of 2 past max not EAGAIN");
    if (!write_value(e, 1)) return fail(16, "write of 1 up to max");
    if (!read_value(e, &value) || value != COUNTER_MAX)
        return fail(16, "read of max");
    close(e);
    return 0;
}

static int check_poll(void)
{
    struct pollfd entry;
    int e = eventfd(0, EFD_NONBLOCK), n;
    if (e < 0) return fail(10, "eventfd(0, EFD_NONBLOCK)");
    entry.fd = e;
    entry.events = POLLIN;
    entry.revents = 0;
    n = poll(&entry, 1, 0);
    if (n != 0 || entry.revents != 0) return fail(17, "fresh eventfd readable");
    if (!write_value(e, 1)) return fail(17, "write of 1");
    entry.revents = 0;
    n = poll(&entry, 1, 0);
    if (n != 1 || (entry.revents & POLLIN) == 0)
        return fail(17, "written eventfd not POLLIN");
    close(e);
    return 0;
}

static int check_libcurl(void)
{
    uint64_t value = 0;
    int e = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC), flags;
    if (e < 0) return fail(18, "eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)");
    if (!write_value(e, 1) || !write_value(e, 1))
        return fail(18, "two writes of 1");
    if (!read_value(e, &value) || value != 2) return fail(18, "read of 2");
    flags = fcntl(e, F_GETFD);
    if (flags < 0 || (flags & FD_CLOEXEC) == 0)
        return fail(18, "FD_CLOEXEC not set");
    close(e);
    return 0;
}

static int check_flags_and_eventfd(void)
{
    uint64_t value = 0;
    int e;
    errno = 0;
    if (syscall(SYS_eventfd2, 0, 0x4) != -1 || errno != EINVAL)
        return fail(19, "eventfd2(0, 0x4) not EINVAL");
    e = (int)syscall(SYS_eventfd, 2);
    if (e < 0) return fail(19, "eventfd(2)");
    if (!read_value(e, &value) || value != 2)
        return fail(19, "read of 2 from eventfd(2)");
    close(e);
    return 0;
}

static int check_setfl(void)
{
    int e = eventfd(0, 0), flags;
    if (e < 0) return fail(10, "eventfd(0, 0)");
    flags = fcntl(e, F_GETFL);
    if (flags < 0 || (flags & O_NONBLOCK) != 0)
        return fail(20, "fresh blocking instance shows O_NONBLOCK");
    if (fcntl(e, F_SETFL, flags | O_NONBLOCK) != 0)
        return fail(20, "F_SETFL O_NONBLOCK");
    flags = fcntl(e, F_GETFL);
    if (flags < 0 || (flags & O_NONBLOCK) == 0)
        return fail(20, "F_GETFL lacks O_NONBLOCK");
    if (!read_eagain(e)) return fail(20, "empty read not EAGAIN");
    {
        int off = 0, on = 1;
        if (ioctl(e, FIONBIO, &off) != 0) return fail(20, "FIONBIO 0");
        flags = fcntl(e, F_GETFL);
        if (flags < 0 || (flags & O_NONBLOCK) != 0)
            return fail(20, "FIONBIO 0 left O_NONBLOCK");
        if (ioctl(e, FIONBIO, &on) != 0) return fail(20, "FIONBIO 1");
        flags = fcntl(e, F_GETFL);
        if (flags < 0 || (flags & O_NONBLOCK) == 0)
            return fail(20, "FIONBIO 1 did not set O_NONBLOCK");
        if (!read_eagain(e)) return fail(20, "empty read after FIONBIO");
    }
    close(e);
    return 0;
}

static int check_dup(void)
{
    uint64_t value = 0;
    int e = eventfd(0, 0), d, flags;
    if (e < 0) return fail(10, "eventfd(0, 0)");
    d = dup(e);
    if (d < 0) return fail(21, "dup");
    if (!write_value(d, 6) || !read_value(e, &value) || value != 6)
        return fail(21, "write through dup not read through original");
    flags = fcntl(d, F_GETFL);
    if (flags < 0 || fcntl(d, F_SETFL, flags | O_NONBLOCK) != 0)
        return fail(21, "F_SETFL on dup");
    flags = fcntl(e, F_GETFL);
    if (flags < 0 || (flags & O_NONBLOCK) == 0)
        return fail(21, "O_NONBLOCK set on dup not seen on original");
    if (!read_eagain(e)) return fail(21, "original not nonblocking");
    close(d);
    close(e);
    return 0;
}

static int created[CREATE_CAP];

static int check_exhaust(void)
{
    int count = 0, saved_errno = 0, i, e;
    while (count < CREATE_CAP) {
        e = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (e < 0) { saved_errno = errno; break; }
        created[count++] = e;
    }
    for (i = 0; i < count; ++i) close(created[i]);
    fprintf(stderr, "eventfd-semantics created %d eventfds (errno %d)\n",
        count, saved_errno);
    errno = saved_errno;
    if (count < 50) return fail(22, "fewer than 50 eventfds");
    if (count == CREATE_CAP) return fail(22, "never failed");
    if (saved_errno != EMFILE) return fail(22, "failure not EMFILE");
    e = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (e < 0) return fail(22, "no eventfd after closing them all");
    if (!write_value(e, 1)) return fail(22, "new eventfd does not work");
    close(e);
    return 0;
}

int main(void)
{
    int result;
    if ((result = check_basic()) != 0) return result;
    if ((result = check_sum()) != 0) return result;
    if ((result = check_semaphore()) != 0) return result;
    if ((result = check_invalid()) != 0) return result;
    if ((result = check_full()) != 0) return result;
    if ((result = check_poll()) != 0) return result;
    if ((result = check_libcurl()) != 0) return result;
    if ((result = check_flags_and_eventfd()) != 0) return result;
    if ((result = check_setfl()) != 0) return result;
    if ((result = check_dup()) != 0) return result;
    if ((result = check_exhaust()) != 0) return result;
    return 0;
}
