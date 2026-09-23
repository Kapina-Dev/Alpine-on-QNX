#include "linuxemu.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>

#define LINUX_NR_SELECT 82
#define LINUX_NR_NEWSELECT 142
#define LINUX_NR_POLL 168
#define LINUX_NR_PSELECT6 335
#define LINUX_NR_PPOLL 336
#define LINUX_NR_PSELECT6_TIME64 413
#define LINUX_NR_PPOLL_TIME64 414

struct linux_pollfd { int32_t fd; int16_t events; int16_t revents; };

static short poll_events_to_host(short value)
{
    short result = 0;
    if (value & (0x001 | 0x040)) result |= POLLRDNORM;
    if (value & 0x080) result |= POLLRDBAND;
    if (value & 0x002) result |= POLLPRI;
    if (value & (0x004 | 0x100)) result |= POLLOUT;
    if (value & 0x200) result |= POLLWRBAND;
    return result;
}

static short poll_events_to_linux(short value)
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

static int timespec_timeout(const void *address, int time64, int *milliseconds)
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

static int32_t do_poll(struct linux_pollfd *guest, uint32_t count, int timeout,
    int signal_wait_fd, int *signal_interrupted)
{
    struct pollfd *host;
    uint32_t host_count = count + (signal_wait_fd >= 0 ? 1u : 0u);
    uint32_t i;
    int result;
    if (count > 65536u) return -EINVAL;
    host = host_count == 0 ? 0 : calloc(host_count, sizeof(*host));
    if (host_count != 0 && host == 0) return -ENOMEM;
    for (i = 0; i != count; ++i) {
        host[i].fd = guest[i].fd;
        host[i].events = poll_events_to_host(guest[i].events);
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
    if (result >= 0)
        for (i = 0; i != count; ++i)
            guest[i].revents = poll_events_to_linux(host[i].revents);
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
    fd_set read_set, write_set, except_set;
    struct timeval timeout, *timeout_pointer = 0;
    sigset_t requested, old;
    uint32_t requested_words[2], old_words[2];
    int replace_mask = 0;
    int signal_wait_fd = -1;
    int signal_interrupted = 0;
    int host_count = count;
    int result;
    if (count < 0 || count > FD_SETSIZE) return -EINVAL;
    fdset_to_host(guest_read, count, &read_set);
    fdset_to_host(guest_write, count, &write_set);
    fdset_to_host(guest_except, count, &except_set);
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
        timeout_pointer);
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
        int result = timespec_timeout((void *)arguments[2],
            number == LINUX_NR_PPOLL_TIME64, &timeout);
        sigset_t requested, old;
        uint32_t requested_words[2], old_words[2];
        int replace_mask = 0;
        int signal_wait_fd = -1;
        int signal_interrupted = 0;
        if (result != 0) return result;
        if (arguments[3] != 0) {
            result = guest_signal_host_mask((void *)arguments[3], arguments[4],
                &requested);
            if (result != 0) return result;
            memcpy(requested_words, (void *)arguments[3],
                sizeof(requested_words));
            requested_words[0] &=
                ~((1u << (9 - 1)) | (1u << (19 - 1)));
            guest_thread_signal_mask_get(old_words);
            signal_wait_fd = guest_thread_signal_wait_begin();
            if (signal_wait_fd < 0)
                return -(int32_t)linux_errno_number(errno);
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
        result = do_poll((struct linux_pollfd *)arguments[0], arguments[1],
            timeout, signal_wait_fd, &signal_interrupted);
        if (replace_mask) {
            int saved_errno = errno;
            if (signal_interrupted)
                guest_thread_signal_wait_interrupted(old_words);
            else guest_thread_signal_wait_complete(old_words);
            sigprocmask(SIG_SETMASK, &old, 0);
            guest_thread_signal_wait_end();
            errno = saved_errno;
        }
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
