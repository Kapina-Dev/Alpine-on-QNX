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
#include <time.h>

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

/* Fixed stack buffers cover small sets; larger ones map (no malloc in
   the syscall handler). */
#define POLL_STACK_ENTRIES 64

/* One poll call's arrays: host[capacity] (the guest's entries, then the
   signal-wait and probe entries, then expanded epolls), ready[count] (host
   bits ready without the native target), span[2 * count] (an epoll
   entry's expanded start and length; start -1 for other entries). */
struct poll_storage {
    struct pollfd *host;
    short *ready;
    int *span;
    size_t mapped_size;
};

static int poll_storage_fit(struct poll_storage *storage, uint32_t count,
    size_t capacity, struct pollfd *stack_host, short *stack_ready,
    int *stack_span)
{
    size_t size;
    char *memory;
    epoll_storage_free(storage->host, storage->mapped_size);
    storage->mapped_size = 0;
    if (capacity <= POLL_STACK_ENTRIES && count <= POLL_STACK_ENTRIES) {
        storage->host = stack_host;
        storage->ready = stack_ready;
        storage->span = stack_span;
        return 0;
    }
    size = capacity * sizeof(struct pollfd) + 2 * count * sizeof(int) +
        count * sizeof(short);
    memory = epoll_storage_alloc(size);
    if (memory == 0) { storage->host = 0; return -1; }
    storage->host = (struct pollfd *)(void *)memory;
    storage->span = (int *)(void *)(memory + capacity * sizeof(struct pollfd));
    storage->ready = (short *)(void *)(memory +
        capacity * sizeof(struct pollfd) + 2 * count * sizeof(int));
    storage->mapped_size = size;
    return 0;
}

static long long poll_now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* An epoll fd in the set is replaced by its items' native fds
   (linux_epoll_expand) and reported readable (POLLIN) when one of them
   would be reported, without consuming EPOLLET or EPOLLONESHOT state, as
   Linux's poll on an epoll file.  A probe channel beside them wakes the
   poll when any epoll's list changes; a poll that ends with nothing to
   report then expands again and waits for the rest of the time. */
static int32_t do_poll(struct linux_pollfd *guest, uint32_t count, int timeout,
    int signal_wait_fd, int *signal_interrupted)
{
    struct pollfd stack_host[POLL_STACK_ENTRIES];
    short stack_ready[POLL_STACK_ENTRIES];
    int stack_span[2 * POLL_STACK_ENTRIES];
    struct poll_storage storage;
    size_t capacity;
    long long deadline = 0;
    uint32_t i;
    int result, typed_any = 0, hidden_any = 0;
    if (count > 65536u) return -EINVAL;
    for (i = 0; i != count; ++i) {
        if (guest[i].fd < 0) continue;
        if (guest_fd_is_hidden(guest[i].fd)) hidden_any = 1;
        else if (guest_fd_is_typed(guest[i].fd)) typed_any = 1;
    }
    /* Room for expansions: the rest of the stack buffer when the set is
       small, so a small poll on an epoll doesn't map. */
    capacity = (size_t)count + 2 + (!typed_any ? 0 : count + 2 <
        POLL_STACK_ENTRIES ? POLL_STACK_ENTRIES - count - 2 :
        POLL_STACK_ENTRIES);
    if (timeout > 0) deadline = poll_now_ms() + timeout;
    memset(&storage, 0, sizeof(storage));
    if (signal_interrupted != 0) *signal_interrupted = 0;
    for (;;) {
        struct pollfd *host;
        short *ready;
        int *span;
        unsigned probe_generation = 0;
        int wait_ms = timeout, epoll_any = 0, expand_error = 0;
        size_t used;
        if (deadline != 0) {
            long long left = deadline - poll_now_ms();
            wait_ms = left < 0 ? 0 : (int)left;
        }
        if (hidden_any) wait_ms = 0;
        if (poll_storage_fit(&storage, count, capacity, stack_host,
                stack_ready, stack_span) != 0)
            return -ENOMEM;
        host = storage.host;
        ready = storage.ready;
        span = storage.span;
        for (i = 0; i != count; ++i) {
            short events = linux_poll_events_to_host(guest[i].events);
            struct guest_fd_object *epoll;
            ready[i] = 0;
            span[2 * i] = -1;
            span[2 * i + 1] = 0;
            /* Hidden numbers are not the guest's: QNX skips a negative fd
               and the guest sees POLLNVAL, as Linux reports for a closed
               one. */
            if (guest[i].fd < 0 || guest_fd_is_hidden(guest[i].fd)) {
                host[i].fd = -1;
                host[i].events = events;
                host[i].revents = 0;
                continue;
            }
            epoll = typed_any ? linux_epoll_acquire(guest[i].fd) : 0;
            if (epoll != 0) {
                host[i].fd = -1;
                host[i].events = 0;
                host[i].revents = 0;
                span[2 * i] = 0;
                epoll_any = 1;
                guest_fd_put(epoll);
                continue;
            }
            ready[i] = linux_poll_target_entry(guest[i].fd, events, &host[i]);
            if (ready[i] != 0) wait_ms = 0;
        }
        host[count].fd = signal_wait_fd;
        host[count].events = POLLIN;
        host[count].revents = 0;
        host[count + 1].fd = -1;
        host[count + 1].events = POLLIN;
        host[count + 1].revents = 0;
        used = (size_t)count + 2;
        if (epoll_any) {
            host[count + 1].fd = linux_epoll_probe_begin(&probe_generation);
            for (i = 0; i != count && !expand_error; ++i) {
                struct guest_fd_object *epoll;
                int expanded;
                if (span[2 * i] < 0) continue;
                epoll = linux_epoll_acquire(guest[i].fd);
                if (epoll == 0) { span[2 * i] = -1; continue; }
                expanded = linux_epoll_expand(epoll, host + used,
                    (int)(capacity - used), 0);
                if (expanded < 0) expand_error = errno;
                else {
                    span[2 * i] = (int)used;
                    span[2 * i + 1] = expanded;
                    used += (size_t)expanded;
                    if (linux_epoll_expanded_ready(epoll, host + span[2 * i],
                            expanded)) {
                        ready[i] = (short)(POLLRDNORM &
                            linux_poll_events_to_host(guest[i].events));
                        wait_ms = 0;
                    }
                }
                guest_fd_put(epoll);
            }
            if (expand_error != 0) {
                (void)linux_epoll_probe_end(probe_generation, 0);
                if (expand_error == ENOMEM && capacity < (size_t)1 << 20) {
                    capacity *= 2;
                    continue;
                }
                epoll_storage_free(storage.host, storage.mapped_size);
                return -(int32_t)linux_errno_number(expand_error);
            }
        }
        result = poll(host, (nfds_t)used, wait_ms);
        if (signal_interrupted != 0)
            *signal_interrupted = result < 0 && errno == EINTR;
        if (result > 0 && signal_wait_fd >= 0 && host[count].revents != 0) {
            if (signal_interrupted != 0) *signal_interrupted = 1;
            result = -1;
            errno = EINTR;
        }
        if (epoll_any) {
            int saved_errno = errno;
            for (i = 0; i != count; ++i) {
                struct guest_fd_object *epoll;
                if (span[2 * i] < 0) continue;
                epoll = linux_epoll_acquire(guest[i].fd);
                ready[i] = 0;
                if (epoll != 0 && linux_epoll_expanded_ready(epoll,
                        host + span[2 * i], span[2 * i + 1]))
                    ready[i] = (short)(POLLRDNORM &
                        linux_poll_events_to_host(guest[i].events));
                guest_fd_put(epoll);
            }
            (void)linux_epoll_probe_end(probe_generation,
                result > 0 && host[count + 1].revents != 0);
            errno = saved_errno;
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
            /* Woken by a list change, or by an expanded fd whose event the
               epoll wouldn't report: wait again for the rest of the time. */
            if (result == 0 && epoll_any && wait_ms != 0 &&
                    (timeout < 0 || poll_now_ms() < deadline))
                continue;
        }
        break;
    }
    if (result < 0) result = -(int32_t)linux_errno_number(errno);
    epoll_storage_free(storage.host, storage.mapped_size);
    return result;
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

static int in_set(const uint32_t *set, int fd)
{
    return set != 0 && ((set[fd / 32] >> (fd % 32)) & 1u) != 0;
}

/* An epoll fd in one of the sets: select then goes through do_poll, which
   expands it (QNX select() can't take the expanded fds' per-entry results:
   one closed item would fail the whole call with EBADF).  The same for a
   typed fd that waits on another host fd (a signalfd waits on its
   process's own channel): native select would watch the wrong fd. */
static int sets_need_poll(int count, const uint32_t *read_set,
    const uint32_t *write_set, const uint32_t *except_set)
{
    int fd;
    for (fd = 0; fd < count; ++fd) {
        struct guest_fd_object *epoll;
        if (!in_set(read_set, fd) && !in_set(write_set, fd) &&
                !in_set(except_set, fd))
            continue;
        if (!guest_fd_is_typed(fd)) continue;
        epoll = linux_epoll_acquire(fd);
        guest_fd_put(epoll);
        if (epoll != 0 || guest_fd_poll_target(fd) != fd) return 1;
    }
    return 0;
}

/* select through do_poll, with Linux's mapping (fs/select.c): readable on
   IN/RDNORM/RDBAND/HUP/ERR, writable on OUT/WRNORM/WRBAND/ERR, exceptional
   on PRI; a closed fd fails EBADF. */
static int32_t select_by_poll(int count, uint32_t *read_set,
    uint32_t *write_set, uint32_t *except_set, int timeout_ms,
    int signal_wait_fd, int *signal_interrupted)
{
    struct linux_pollfd stack_entries[POLL_STACK_ENTRIES], *entries;
    size_t mapped = 0;
    uint32_t n = 0, i;
    int32_t result;
    int fd;
    for (fd = 0; fd < count; ++fd)
        if (in_set(read_set, fd) || in_set(write_set, fd) ||
                in_set(except_set, fd)) ++n;
    entries = stack_entries;
    if (n > POLL_STACK_ENTRIES) {
        mapped = n * sizeof(*entries);
        entries = epoll_storage_alloc(mapped);
        if (entries == 0) return -ENOMEM;
    }
    for (fd = 0, i = 0; fd < count; ++fd) {
        int16_t events = 0;
        if (in_set(read_set, fd)) events |= 0x001;
        if (in_set(write_set, fd)) events |= 0x004;
        if (in_set(except_set, fd)) events |= 0x002;
        if (events == 0) continue;
        entries[i].fd = fd;
        entries[i].events = events;
        entries[i++].revents = 0;
    }
    result = do_poll(entries, n, timeout_ms, signal_wait_fd,
        signal_interrupted);
    for (i = 0; result >= 0 && i != n; ++i)
        if ((entries[i].revents & 0x020) != 0) result = -EBADF;
    if (result >= 0) {
        size_t words = ((size_t)count + 31) / 32;
        if (read_set != 0) memset(read_set, 0, words * sizeof(uint32_t));
        if (write_set != 0) memset(write_set, 0, words * sizeof(uint32_t));
        if (except_set != 0) memset(except_set, 0, words * sizeof(uint32_t));
        result = 0;
        for (i = 0; i != n; ++i) {
            int16_t revents = entries[i].revents;
            uint32_t bit = 1u << (entries[i].fd % 32);
            size_t word = (size_t)entries[i].fd / 32;
            if ((entries[i].events & 0x001) != 0 &&
                    (revents & (0x001 | 0x040 | 0x080 | 0x010 | 0x008))) {
                read_set[word] |= bit;
                ++result;
            }
            if ((entries[i].events & 0x004) != 0 &&
                    (revents & (0x004 | 0x100 | 0x200 | 0x008))) {
                write_set[word] |= bit;
                ++result;
            }
            if ((entries[i].events & 0x002) != 0 && (revents & 0x002)) {
                except_set[word] |= bit;
                ++result;
            }
        }
    }
    epoll_storage_free(mapped != 0 ? entries : 0, mapped);
    return result;
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
    if (sets_need_poll(count, guest_read, guest_write, guest_except)) {
        struct linux_poll_mask mask;
        int timeout_ms = -1, interrupted = 0;
        if (timeout_pointer != 0) {
            int64_t milliseconds = (int64_t)timeout.tv_sec * 1000 +
                (timeout.tv_usec + 999) / 1000;
            timeout_ms = milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
        }
        mask.signal_wait_fd = -1;
        if (pselect_call && arguments[5] != 0) {
            uint32_t pair[2];
            memcpy(pair, (void *)arguments[5], sizeof(pair));
            if (pair[0] != 0) {
                result = linux_poll_mask_begin((void *)pair[0], pair[1],
                    &mask);
                if (result != 0) return result;
            }
        }
        result = select_by_poll(count, guest_read, guest_write, guest_except,
            timeout_ms, mask.signal_wait_fd, &interrupted);
        linux_poll_mask_end(&mask, interrupted);
        if (result >= 0 && !pselect_call && arguments[4] != 0) {
            int32_t fields[2] = { (int32_t)timeout.tv_sec,
                (int32_t)timeout.tv_usec };
            memcpy((void *)arguments[4], fields, sizeof(fields));
        }
        return result;
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
