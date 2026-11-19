/* Host fd_sets cover every fd number (guest and hidden, GUEST_FD_SLOTS in
   guest_fd.c): pselect6 watches the thread's signal-wait fd, which lives
   in the hidden range above QNX's default 256.  QNX select() takes sets of
   any size (measured on the device: a pipe at fd 800 is reported). */
#define FD_SETSIZE 4096

#include "linuxemu.h"
#include "guest_fd.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>

#define LINUX_NR_SELECT 82
#define LINUX_NR_NEWSELECT 142
#define LINUX_NR_POLL 168
#define LINUX_NR_PSELECT6 335
#define LINUX_NR_PPOLL 336
#define LINUX_NR_PSELECT6_TIME64 413
#define LINUX_NR_PPOLL_TIME64 414

struct linux_pollfd { int32_t fd; int16_t events; int16_t revents; };

short linux_poll_events_to_host(short value)
{
    short result = 0;
    if (value & (0x001 | 0x040)) result |= POLLRDNORM;
    if (value & 0x080) result |= POLLRDBAND;
    if (value & 0x002) result |= POLLPRI;
    if (value & (0x004 | 0x100)) result |= POLLOUT;
    if (value & 0x200) result |= POLLWRBAND;
    return result;
}

short linux_poll_events_to_linux(short value)
{
    short result = 0;
    if (value & POLLRDNORM) result |= 0x001 | 0x040;
    if (value & POLLRDBAND) result |= 0x001 | 0x080;
    if (value & POLLPRI) result |= 0x002;
    if (value & POLLOUT) result |= 0x004 | 0x100;
    if (value & POLLWRBAND) result |= 0x200;
    if (value & POLLERR) result |= 0x008;
    if (value & POLLHUP) result |= 0x010;
    if (value & POLLNVAL) result |= 0x020;
    return result;
}

int linux_poll_timespec_timeout(const void *address, int time64,
    int *milliseconds)
{
    int64_t seconds;
    int64_t nanoseconds;
    if (address == 0) { *milliseconds = -1; return 0; }
    if (time64) {
        int64_t fields[2];
        memcpy(fields, address, sizeof(fields));
        seconds = fields[0]; nanoseconds = fields[1];
    } else {
        int32_t fields[2];
        memcpy(fields, address, sizeof(fields));
        seconds = fields[0]; nanoseconds = fields[1];
    }
    if (seconds < 0 || nanoseconds < 0 || nanoseconds >= 1000000000)
        return -EINVAL;
    if (seconds > (INT_MAX - 1) / 1000) *milliseconds = INT_MAX;
    else *milliseconds = (int)(seconds * 1000 + (nanoseconds + 999999) / 1000000);
    return 0;
}

/* QNX reports a pipe read end whose last writer closed as readable
   (POLLRDNORM) with nothing to read, never POLLHUP (measured 2026-11-19 on
   the Passport, docs/FINDINGS.md).  Linux reports POLLHUP there and, the
   pipe being empty, no POLLIN.  An untyped FIFO that QNX calls readable
   with FIONREAD 0 is at EOF: nothing else makes an empty pipe readable.
   With data left Linux also sets POLLHUP; QNX gives no way to see that the
   writer is gone, so it is reported only once the data is read.  Sockets
   are left alone (their EOF is EPOLLRDHUP, not a hangup). */
short linux_poll_pipe_hangup(int fd, short revents,
    const struct stat64 *status)
{
    struct stat64 own;
    int available = 0;
    if ((revents & POLLRDNORM) == 0 || (revents & POLLHUP) != 0 ||
            guest_fd_is_typed(fd) || ioctl(fd, FIONREAD, &available) != 0 ||
            available != 0)
        return revents;
    if (status == 0) {
        if (fstat64(fd, &own) != 0) return revents;
        status = &own;
    }
    if (!S_ISFIFO(status->st_mode)) return revents;
    return (short)((revents & ~(POLLIN | POLLRDNORM)) | POLLHUP);
}

/* Host write bits a typed fd's ready_now decides instead of the native
   poll target. */
#define READY_NOW_WRITE_BITS (POLLOUT | POLLWRBAND)

short linux_poll_target_entry(int fd, short events, struct pollfd *entry)
{
    int now = guest_fd_ready_now(fd);
    entry->fd = guest_fd_poll_target(fd);
    entry->events = events;
    entry->revents = 0;
    if (now < 0) return 0;
    /* Readiness the native target can't show (an eventfd's POLLOUT, its
       counter): reported as is, and nothing to wait for when it meets the
       request. */
    entry->events = (short)(events & ~READY_NOW_WRITE_BITS);
    return (short)(now & events);
}

int32_t linux_poll_mask_begin(const void *guest_set, uint32_t size,
    struct linux_poll_mask *mask)
{
    sigset_t requested;
    uint32_t requested_words[2];
    int result = guest_signal_host_mask(guest_set, size, &requested);
    mask->signal_wait_fd = -1;
    if (result != 0) return result;
    memcpy(requested_words, guest_set, sizeof(requested_words));
    /* SIGKILL and SIGSTOP can't be blocked. */
    requested_words[0] &= ~((1u << (9 - 1)) | (1u << (19 - 1)));
    guest_thread_signal_mask_get(mask->old_words);
    mask->signal_wait_fd = guest_thread_signal_wait_begin();
    if (mask->signal_wait_fd < 0)
        return -(int32_t)linux_errno_number(errno);
    if (sigprocmask(SIG_SETMASK, &requested, &mask->old) != 0) {
        int saved_errno = errno;
        guest_thread_signal_wait_end();
        mask->signal_wait_fd = -1;
        return -(int32_t)linux_errno_number(saved_errno);
    }
    guest_thread_signal_mask_set(requested_words);
    guest_thread_signal_wait_arm();
    return 0;
}

void linux_poll_mask_end(struct linux_poll_mask *mask, int interrupted)
{
    int saved_errno = errno;
    if (mask->signal_wait_fd < 0) return;
    if (interrupted) guest_thread_signal_wait_interrupted(mask->old_words);
    else guest_thread_signal_wait_complete(mask->old_words);
    sigprocmask(SIG_SETMASK, &mask->old, 0);
    guest_thread_signal_wait_end();
    mask->signal_wait_fd = -1;
    errno = saved_errno;
}

static int32_t do_poll(struct linux_pollfd *guest, uint32_t count, int timeout,
    int signal_wait_fd, int *signal_interrupted)
{
    struct pollfd *host;
    short *ready;
    uint32_t host_count = count + (signal_wait_fd >= 0 ? 1u : 0u);
    uint32_t i;
    int result;
    if (count > 65536u) return -EINVAL;
    /* One allocation: the host array, then the ready_now bits per entry. */
    host = host_count == 0 ? 0 : calloc(1,
        host_count * sizeof(*host) + count * sizeof(*ready));
    if (host_count != 0 && host == 0) return -ENOMEM;
    ready = host == 0 ? 0 : (short *)(void *)(host + host_count);
    for (i = 0; i != count; ++i)
        if (guest[i].fd >= 0 && guest_fd_is_hidden(guest[i].fd)) timeout = 0;
    for (i = 0; i != count; ++i) {
        short events = linux_poll_events_to_host(guest[i].events);
        /* Hidden numbers are not the guest's: QNX skips a negative fd and
           the guest sees POLLNVAL, as Linux reports for a closed one. */
        if (guest[i].fd < 0 || guest_fd_is_hidden(guest[i].fd)) {
            host[i].fd = -1;
            host[i].events = events;
            continue;
        }
        ready[i] = linux_poll_target_entry(guest[i].fd, events, &host[i]);
        if (ready[i] != 0) timeout = 0;
    }
    if (signal_wait_fd >= 0) {
        host[count].fd = signal_wait_fd;
        host[count].events = POLLIN;
    }
    result = poll(host, host_count, timeout);
    if (signal_interrupted != 0)
        *signal_interrupted = result < 0 && errno == EINTR;
    if (result > 0 && signal_wait_fd >= 0 && host[count].revents != 0) {
        if (signal_interrupted != 0) *signal_interrupted = 1;
        result = -1;
        errno = EINTR;
    }
    if (result >= 0) {
        result = 0;
        for (i = 0; i != count; ++i) {
            short revents = host[i].revents;
            if (host[i].fd >= 0 && host[i].fd == guest[i].fd)
                revents = linux_poll_pipe_hangup(guest[i].fd, revents, 0);
            guest[i].revents = linux_poll_events_to_linux(
                (short)(revents | ready[i]));
            if (guest[i].fd >= 0 && guest_fd_is_hidden(guest[i].fd))
                guest[i].revents = 0x20;
            if (guest[i].revents != 0) ++result;
        }
    }
    free(host);
    return result < 0 ? -(int32_t)linux_errno_number(errno) : result;
}

static void fdset_to_host(const uint32_t *guest, int count, fd_set *host)
{
    int fd;
    FD_ZERO(host);
    if (guest == 0) return;
    for (fd = 0; fd < count; ++fd)
        if (guest[fd / 32] & (1u << (fd % 32))) FD_SET(fd, host);
}

static void fdset_to_guest(uint32_t *guest, int count, const fd_set *host)
{
    int fd;
    if (guest == 0) return;
    memset(guest, 0, ((count + 31) / 32) * sizeof(uint32_t));
    for (fd = 0; fd < count; ++fd)
        if (FD_ISSET(fd, host)) guest[fd / 32] |= 1u << (fd % 32);
}

static int32_t do_select(uint32_t *arguments, int pselect_call, int time64)
{
    int count = (int)arguments[0];
    uint32_t *guest_read = (uint32_t *)arguments[1];
    uint32_t *guest_write = (uint32_t *)arguments[2];
    uint32_t *guest_except = (uint32_t *)arguments[3];
    fd_set read_set, write_set, except_set, ready_read, ready_write;
    struct timeval timeout, *timeout_pointer = 0, no_wait = { 0, 0 };
    int ready_any = 0;
    sigset_t requested, old;
    uint32_t requested_words[2], old_words[2];
    int replace_mask = 0;
    int signal_wait_fd = -1;
    int signal_interrupted = 0;
    int host_count = count;
    int result;
    if (count < 0 || count > FD_SETSIZE) return -EINVAL;
    {
        int fd;
        for (fd = guest_fd_hidden_base(); fd < count; ++fd)
            if ((guest_read != 0 && (guest_read[fd / 32] >> (fd % 32)) & 1u) ||
                (guest_write != 0 && (guest_write[fd / 32] >> (fd % 32)) & 1u) ||
                (guest_except != 0 &&
                    (guest_except[fd / 32] >> (fd % 32)) & 1u))
                return -EBADF;
    }
    fdset_to_host(guest_read, count, &read_set);
    fdset_to_host(guest_write, count, &write_set);
    fdset_to_host(guest_except, count, &except_set);
    FD_ZERO(&ready_read);
    FD_ZERO(&ready_write);
    {
        /* Typed fds with ready_now: their write readiness comes from the
           op (the native target leaves the write set), and readiness
           reported here means select doesn't wait. */
        int fd;
        for (fd = 0; fd < count; ++fd) {
            int now;
            if (!FD_ISSET(fd, &read_set) && !FD_ISSET(fd, &write_set))
                continue;
            now = guest_fd_ready_now(fd);
            if (now < 0) continue;
            if (FD_ISSET(fd, &write_set)) {
                FD_CLR(fd, &write_set);
                if ((now & POLLOUT) != 0) {
                    FD_SET(fd, &ready_write);
                    ready_any = 1;
                }
            }
            if (FD_ISSET(fd, &read_set) && (now & POLLRDNORM) != 0) {
                FD_SET(fd, &ready_read);
                ready_any = 1;
            }
        }
    }
    if (arguments[4] != 0) {
        int64_t seconds, fraction;
        if (time64) {
            int64_t fields[2]; memcpy(fields, (void *)arguments[4], 16);
            seconds = fields[0]; fraction = fields[1];
        } else {
            int32_t fields[2]; memcpy(fields, (void *)arguments[4], 8);
            seconds = fields[0]; fraction = fields[1];
        }
        if (seconds < 0 || fraction < 0 ||
            fraction >= (pselect_call ? 1000000000 : 1000000)) return -EINVAL;
        if (seconds > LONG_MAX) return -EINVAL;
        timeout.tv_sec = (long)seconds;
        timeout.tv_usec = (long)(pselect_call ? fraction / 1000 : fraction);
        timeout_pointer = &timeout;
    }
    if (pselect_call && arguments[5] != 0) {
        uint32_t pair[2];
        memcpy(pair, (void *)arguments[5], sizeof(pair));
        result = guest_signal_host_mask((void *)pair[0], pair[1], &requested);
        if (result != 0) return result;
        memcpy(requested_words, (void *)pair[0], sizeof(requested_words));
        requested_words[0] &= ~((1u << (9 - 1)) | (1u << (19 - 1)));
        guest_thread_signal_mask_get(old_words);
        signal_wait_fd = guest_thread_signal_wait_begin();
        if (signal_wait_fd < 0)
            return -(int32_t)linux_errno_number(errno);
        if (signal_wait_fd >= FD_SETSIZE) {
            guest_thread_signal_wait_end();
            return -EMFILE;
        }
        FD_SET(signal_wait_fd, &read_set);
        if (signal_wait_fd >= host_count) host_count = signal_wait_fd + 1;
        if (sigprocmask(SIG_SETMASK, &requested, &old) != 0)
            {
                int saved_errno = errno;
                guest_thread_signal_wait_end();
                return -(int32_t)linux_errno_number(saved_errno);
            }
        guest_thread_signal_mask_set(requested_words);
        guest_thread_signal_wait_arm();
        replace_mask = 1;
    }
    result = select(host_count, (guest_read || signal_wait_fd >= 0) ?
        &read_set : 0,
        guest_write ? &write_set : 0, guest_except ? &except_set : 0,
        ready_any ? &no_wait : timeout_pointer);
    signal_interrupted = result < 0 && errno == EINTR;
    if (result > 0 && signal_wait_fd >= 0 &&
        FD_ISSET(signal_wait_fd, &read_set)) {
        signal_interrupted = 1;
        result = -1;
        errno = EINTR;
    }
    if (replace_mask) {
        int saved_errno = errno;
        if (signal_interrupted)
            guest_thread_signal_wait_interrupted(old_words);
        else guest_thread_signal_wait_complete(old_words);
        sigprocmask(SIG_SETMASK, &old, 0);
        guest_thread_signal_wait_end();
        errno = saved_errno;
    }
    if (result < 0) return -(int32_t)linux_errno_number(errno);
    if (ready_any) {
        int fd;
        result = 0;
        for (fd = 0; fd < count; ++fd) {
            if (FD_ISSET(fd, &ready_read)) FD_SET(fd, &read_set);
            if (FD_ISSET(fd, &ready_write)) FD_SET(fd, &write_set);
            if (guest_read != 0 && FD_ISSET(fd, &read_set)) ++result;
            if (guest_write != 0 && FD_ISSET(fd, &write_set)) ++result;
            if (guest_except != 0 && FD_ISSET(fd, &except_set)) ++result;
        }
    }
    fdset_to_guest(guest_read, count, &read_set);
    fdset_to_guest(guest_write, count, &write_set);
    fdset_to_guest(guest_except, count, &except_set);
    if (!pselect_call && arguments[4] != 0) {
        int32_t fields[2] = { (int32_t)timeout.tv_sec,
            (int32_t)timeout.tv_usec };
        memcpy((void *)arguments[4], fields, sizeof(fields));
    }
    return result;
}

int32_t linux_poll_syscall(uint32_t number, uint32_t arguments[6])
{
    if (number == LINUX_NR_POLL)
        return do_poll((struct linux_pollfd *)arguments[0], arguments[1],
            (int)arguments[2], -1, 0);
    if (number == LINUX_NR_PPOLL || number == LINUX_NR_PPOLL_TIME64) {
        int timeout;
        int result = linux_poll_timespec_timeout((void *)arguments[2],
            number == LINUX_NR_PPOLL_TIME64, &timeout);
        struct linux_poll_mask mask;
        int signal_interrupted = 0;
        if (result != 0) return result;
        mask.signal_wait_fd = -1;
        if (arguments[3] != 0) {
            result = linux_poll_mask_begin((void *)arguments[3], arguments[4],
                &mask);
            if (result != 0) return result;
        }
        result = do_poll((struct linux_pollfd *)arguments[0], arguments[1],
            timeout, mask.signal_wait_fd, &signal_interrupted);
        linux_poll_mask_end(&mask, signal_interrupted);
        return result;
    }
    if (number == LINUX_NR_SELECT) {
        uint32_t old_arguments[5];
        uint32_t expanded[6] = { 0, 0, 0, 0, 0, 0 };
        if (arguments[0] == 0) return -EFAULT;
        memcpy(old_arguments, (void *)arguments[0], sizeof(old_arguments));
        memcpy(expanded, old_arguments, sizeof(old_arguments));
        return do_select(expanded, 0, 0);
    }
    return do_select(arguments,
        number == LINUX_NR_PSELECT6 || number == LINUX_NR_PSELECT6_TIME64,
        number == LINUX_NR_PSELECT6_TIME64);
}
