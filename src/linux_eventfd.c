/*
 * The Linux eventfd (roadmap Phase 3) as a guest fd object.
 *
 * One eventfd() call is one Linux open-file description.  Its state lives
 * in one page of unnamed shm (struct eventfd_shared), so fork children,
 * exec images and SCM_RIGHTS receivers that map the same page see the same
 * counter and status flags.  The guest's fd number is the read end of a
 * pipe, the readiness channel: it holds a byte while the counter is
 * non-zero, so POLLIN comes from the native poll.  The pipe's write end and
 * the shm fd are emulator-owned hidden fds.
 *
 * No lock.  QNX has no robust mutexes, and a process-shared mutex whose
 * owner dies stays locked forever (FINDINGS 2026-11-19), so the counter is
 * updated only by a 64-bit compare-and-swap and the channel byte is a hint
 * kept by the rules in eventfd_channel_raise() and eventfd_channel_settle().
 */
#include "linuxemu.h"
#include "guest_fd.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define LINUX_EFD_SEMAPHORE 0x00000001u
#define LINUX_EFD_NONBLOCK 0x00000800u
#define LINUX_EFD_CLOEXEC 0x00080000u
#define EVENTFD_COUNTER_MAX 0xfffffffffffffffeULL

/* Per process: the mapping and the hidden fds.  The guest's fd (a dup of
   the pipe read end) is passed to every op. */
struct eventfd_data {
    struct eventfd_shared *shared;
    int write_fd;
    int shm_fd;
};

/* 64-bit atomics.  GCC 4.6 doesn't inline __sync on 64-bit operands and
   the QNX libgcc has no __sync_val_compare_and_swap_8, so these are the
   ARMv7 exclusive-pair sequences (as the Linux kernel's atomic64 on
   ARMv7).  ldrexd/strexd need an even/odd register pair; GCC allocates
   64-bit operands to even pairs when LDRD is available, and the assembler
   rejects an odd pair, so a wrong allocation can't build silently. */
static uint64_t load64(volatile uint64_t *pointer)
{
    uint64_t value;
    __asm__ __volatile__("ldrexd %0, %H0, [%1]"
        : "=&r"(value) : "r"(pointer), "Qo"(*pointer));
    return value;
}

/* Returns the value seen; the swap happened when it equals expected.  Full
   barrier on both sides, like the __sync builtins. */
static uint64_t cas64(volatile uint64_t *pointer, uint64_t expected,
    uint64_t desired)
{
    uint64_t seen;
    uint32_t failed;
    __sync_synchronize();
    do {
        __asm__ __volatile__(
            "ldrexd %1, %H1, [%3]\n\t"
            "mov %0, #0\n\t"
            "teq %1, %4\n\t"
            "teqeq %H1, %H4\n\t"
            "strexdeq %0, %5, %H5, [%3]"
            : "=&r"(failed), "=&r"(seen), "+Qo"(*pointer)
            : "r"(pointer), "r"(expected), "r"(desired)
            : "cc");
    } while (failed != 0);
    __sync_synchronize();
    return seen;
}

/* The readiness channel.  Invariant once operations settle: counter > 0
   implies channel_full == 1 implies the pipe holds a byte.  A byte with
   counter 0 can remain after a race; the next read that finds the counter
   empty removes it (eventfd_channel_settle), and until then a nonblocking read
   returns EAGAIN, as a spurious wakeup does on Linux. */

/* The protocol is exported (linuxemu.h) for the other counter-backed
   objects (timerfd, signalfd): each passes its page's struct
   eventfd_shared, its hidden channel write end, and the guest's fd (the
   channel read end) where the channel may be drained. */

/* After moving the counter from 0 to non-zero, or finding it non-zero
   after a settle: the winner of channel_full 0 -> 1 writes the byte. */
void eventfd_channel_raise(struct eventfd_shared *shared, int write_fd)
{
    static const char byte = 1;
    if (__sync_val_compare_and_swap(&shared->channel_full, 0, 1) == 0)
        (void)write(write_fd, &byte, 1);
}

/* After moving the counter to 0, or finding it 0: empty the pipe, then
   clear channel_full, then re-check the counter.  Draining before the
   clear matters: a writer that raised in between keeps channel_full at 1
   until our clear, and our re-check then sees its count and raises again,
   so a drained byte is always replaced. */
void eventfd_channel_settle(struct eventfd_shared *shared, int write_fd,
    int read_fd)
{
    char sink[64];
    while (read(read_fd, sink, sizeof(sink)) > 0) {}
    (void)__sync_lock_test_and_set(&shared->channel_full, 0);
    __sync_synchronize();
    if (load64(&shared->counter) != 0) eventfd_channel_raise(shared, write_fd);
}

uint64_t eventfd_counter_load(struct eventfd_shared *shared)
{
    return load64(&shared->counter);
}

/* Takes what a read returns: the whole counter, or 1 in semaphore mode.
   0 when the counter is empty. */
uint64_t eventfd_counter_take(struct eventfd_shared *shared, int write_fd,
    int read_fd)
{
    volatile uint64_t *counter = &shared->counter;
    uint64_t seen = load64(counter);
    for (;;) {
        uint64_t taken, after, swapped;
        if (seen == 0) break;
        taken = shared->semaphore ? 1 : seen;
        after = seen - taken;
        swapped = cas64(counter, seen, after);
        if (swapped != seen) { seen = swapped; continue; }
        if (after == 0) eventfd_channel_settle(shared, write_fd, read_fd);
        return taken;
    }
    eventfd_channel_settle(shared, write_fd, read_fd);
    return 0;
}

/* Adds value; 0 when it would pass the maximum. */
int eventfd_counter_add(struct eventfd_shared *shared, int write_fd,
    uint64_t value)
{
    volatile uint64_t *counter = &shared->counter;
    uint64_t seen = load64(counter);
    if (value == 0) return 1;
    for (;;) {
        uint64_t swapped;
        if (EVENTFD_COUNTER_MAX - seen < value) return 0;
        swapped = cas64(counter, seen, seen + value);
        if (swapped == seen) break;
        seen = swapped;
    }
    if (seen == 0) eventfd_channel_raise(shared, write_fd);
    return 1;
}

/* Replaces the counter (timerfd re-arm and TFD_IOC_SET_TICKS), then
   raises or settles the channel to match. */
void eventfd_counter_set(struct eventfd_shared *shared, int write_fd,
    int read_fd, uint64_t value)
{
    volatile uint64_t *counter = &shared->counter;
    uint64_t seen = load64(counter), swapped;
    while ((swapped = cas64(counter, seen, value)) != seen) seen = swapped;
    if (value != 0) eventfd_channel_raise(shared, write_fd);
    else eventfd_channel_settle(shared, write_fd, read_fd);
}

/* Blocking waits.  Nothing is held while waiting: the counter is only
   ever changed by a compare-and-swap, so a process killed while blocked
   (or anywhere else) leaves nothing locked.  Each wait sleeps in poll()
   with the thread's signal-wait fd beside it, so a guest signal ends the
   wait with -EINTR; the trap's syscall-restart path then applies
   SA_RESTART exactly as for an interrupted native read or write.

   Returns 0 to re-check the counter, or a negative Linux errno. */
static int32_t wait_for_change(struct eventfd_data *data, int read_fd,
    uint64_t write_value, int timeout)
{
    struct pollfd entries[2];
    int signal_wait_fd = guest_thread_signal_wait_begin(), count, result;
    uint64_t counter;
    if (signal_wait_fd < 0) return -(int32_t)linux_errno_number(errno);
    /* A signal already pending (and unblocked) fills the wait fd now; one
       arriving from here on writes to it from the host handler. */
    guest_thread_signal_wait_arm();
    /* The re-check after the wait fd exists: a change from here on either
       raises the channel (read) or is seen by the next slice (write). */
    counter = load64(&data->shared->counter);
    if (write_value == 0 ? counter != 0 :
            EVENTFD_COUNTER_MAX - counter >= write_value) {
        guest_thread_signal_wait_end();
        return 0;
    }
    count = 0;
    if (write_value == 0) {
        entries[count].fd = read_fd;
        entries[count].events = POLLIN;
        entries[count].revents = 0;
        ++count;
    }
    entries[count].fd = signal_wait_fd;
    entries[count].events = POLLIN;
    entries[count].revents = 0;
    ++count;
    result = poll(entries, (nfds_t)count, timeout);
    if ((result < 0 && errno == EINTR) ||
            (result > 0 && entries[count - 1].revents != 0))
        result = -(int32_t)linux_errno_number(EINTR);
    else if (result < 0) result = -(int32_t)linux_errno_number(errno);
    else result = 0;
    guest_thread_signal_wait_end();
    return result;
}

/* The read side sleeps until the channel holds a byte.  The 1 s slice is a
   safety net only: a process killed between draining the channel and
   re-raising it can leave the counter above 0 with an empty channel, and
   the slice re-checks the counter then. */
#define EVENTFD_READ_SLICE_MS 1000
/* Nothing signals "the counter dropped", so a full write re-checks on a
   ladder of sleeps (1, 2, 4 ... 64 ms).  Linux programs almost never fill
   an eventfd, so this path is rare. */
#define EVENTFD_WRITE_SLICE_MAX_MS 64

/* After a wait: another thread may have closed the guest's fd number, and
   the number may now be something else.  The channel drain reads through
   that number, so stop (EBADF) rather than read a stranger's data.  Linux
   would finish the read on the old file; this is the safe approximation. */
static int still_attached(struct guest_fd_object *object, int fd)
{
    struct guest_fd_object *current = guest_fd_acquire(fd);
    int same = current == object;
    guest_fd_put(current);
    return same;
}

static int32_t eventfd_read(struct guest_fd_object *object, int fd,
    void *buffer, size_t length)
{
    struct eventfd_data *data = guest_fd_object_data(object);
    uint64_t value;
    if (length < sizeof(value)) return -(int32_t)linux_errno_number(EINVAL);
    for (;;) {
        int32_t waited;
        value = eventfd_counter_take(data->shared, data->write_fd, fd);
        if (value != 0) break;
        if (data->shared->nonblock)
            return -(int32_t)linux_errno_number(EAGAIN);
        waited = wait_for_change(data, fd, 0, EVENTFD_READ_SLICE_MS);
        if (waited == 0 && !still_attached(object, fd))
            waited = -(int32_t)linux_errno_number(EBADF);
        if (waited != 0) return waited;
    }
    memcpy(buffer, &value, sizeof(value));
    return (int32_t)sizeof(value);
}

static int32_t eventfd_write(struct guest_fd_object *object, int fd,
    const void *buffer, size_t length)
{
    struct eventfd_data *data = guest_fd_object_data(object);
    uint64_t value;
    int slice = 1;
    if (length < sizeof(value)) return -(int32_t)linux_errno_number(EINVAL);
    memcpy(&value, buffer, sizeof(value));
    if (value == 0xffffffffffffffffULL)
        return -(int32_t)linux_errno_number(EINVAL);
    while (!eventfd_counter_add(data->shared, data->write_fd, value)) {
        int32_t waited;
        if (data->shared->nonblock)
            return -(int32_t)linux_errno_number(EAGAIN);
        /* value != 0 here: a write of 0 always succeeds. */
        waited = wait_for_change(data, fd, value, slice);
        if (waited == 0 && !still_attached(object, fd))
            waited = -(int32_t)linux_errno_number(EBADF);
        if (waited != 0) return waited;
        if (slice < EVENTFD_WRITE_SLICE_MAX_MS) slice *= 2;
    }
    return (int32_t)sizeof(value);
}

static int eventfd_poll_fd(struct guest_fd_object *object, int fd)
{
    (void)object;
    return fd;
}

/* The native read end stays nonblocking always; O_NONBLOCK as the guest
   sees it lives in the shared page (one per open-file description). */
static int eventfd_get_status(struct guest_fd_object *object, int fd)
{
    struct eventfd_data *data = guest_fd_object_data(object);
    (void)fd;
    return O_RDWR | (data->shared->nonblock ? O_NONBLOCK : 0);
}

static int eventfd_set_status(struct guest_fd_object *object, int fd,
    int host_flags)
{
    struct eventfd_data *data = guest_fd_object_data(object);
    (void)fd;
    data->shared->nonblock = (host_flags & O_NONBLOCK) != 0;
    __sync_synchronize();
    return 0;
}

static void eventfd_release(struct guest_fd_object *object)
{
    struct eventfd_data *data = guest_fd_object_data(object);
    munmap(data->shared, EVENTFD_PAGE);
    close(data->write_fd);
    close(data->shm_fd);
}

/* Exec record body "shm:channel:" (two hidden fds). */
static int eventfd_serialize(struct guest_fd_object *object, int fd,
    char *buffer, size_t size)
{
    struct eventfd_data *data = guest_fd_object_data(object);
    int length = snprintf(buffer, size, "%d:%d:", data->shm_fd,
        data->write_fd);
    (void)fd;
    return length < 0 || (size_t)length >= size ? -1 : length;
}

/* From the shared counter, not the channel: POLLOUT below the maximum,
   and POLLRDNORM (Linux POLLIN) whenever the counter is above 0, which
   also covers a channel emptied by a process killed before it re-raised
   it (every guest wait goes through here before sleeping). */
static short eventfd_ready_now(struct guest_fd_object *object, int fd)
{
    struct eventfd_data *data = guest_fd_object_data(object);
    uint64_t counter = load64(&data->shared->counter);
    short ready = 0;
    (void)fd;
    if (counter < EVENTFD_COUNTER_MAX) ready |= POLLOUT;
    if (counter != 0) ready |= POLLRDNORM;
    return ready;
}

/* SCM_RIGHTS companions: the channel write end, then the shm page.  The
   guest's fd (the channel read end) travels as itself. */
static int eventfd_companions(struct guest_fd_object *object, int fd,
    int *out, int max)
{
    struct eventfd_data *data = guest_fd_object_data(object);
    (void)fd;
    if (max < 2 || data->write_fd < 0 || data->shm_fd < 0) return -1;
    out[0] = data->write_fd;
    out[1] = data->shm_fd;
    return 2;
}

static const struct guest_fd_ops eventfd_ops = {
    eventfd_read, eventfd_write, eventfd_poll_fd, eventfd_get_status,
    eventfd_set_status, eventfd_release, eventfd_serialize, eventfd_ready_now,
    eventfd_companions
};

static void *map_page(int shm_fd)
{
    void *page = mmap(0, EVENTFD_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED,
        shm_fd, 0);
    return page == MAP_FAILED ? 0 : page;
}

/* Wraps a channel read end, write end, shm fd and its mapping (all owned
   by the caller until success) into an object attached to read_fd.  On
   failure the mapping is gone and the fds are still the caller's. */
static int attach_object(int read_fd, int write_fd, int shm_fd, void *page)
{
    struct guest_fd_object *object;
    struct eventfd_data *data;
    object = guest_fd_object_new(GUEST_FD_EVENTFD, &eventfd_ops, read_fd,
        sizeof(struct eventfd_data));
    if (object == 0) {
        int saved_errno = errno;
        munmap(page, EVENTFD_PAGE);
        errno = saved_errno;
        return -1;
    }
    data = guest_fd_object_data(object);
    data->shared = page;
    data->write_fd = write_fd;
    data->shm_fd = shm_fd;
    if (guest_fd_attach(read_fd, object) != 0) {
        int saved_errno = errno;
        /* The object owns the fds now; take them back before the put
           closes them, so the caller's cleanup stays the single owner. */
        data->write_fd = -1;
        data->shm_fd = -1;
        guest_fd_put(object);
        errno = saved_errno;
        return -1;
    }
    guest_fd_put(object);
    return 0;
}

/* Moves an emulator-owned fd into the hidden range.  No free hidden
   number means the process is out of fds: EMFILE, as Linux at the limit. */
static int hide(int fd)
{
    int hidden = guest_fd_hide(fd);
    if (hidden < 0 && errno == EINVAL) errno = EMFILE;
    return hidden;
}

/* A fresh unnamed shm page, hidden. */
static int create_page(const char *prefix)
{
    int fd = guest_fd_unnamed_shm(prefix, EVENTFD_PAGE), hidden;
    if (fd < 0) return -1;
    hidden = hide(fd);
    if (hidden < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
    }
    return hidden;
}

static int set_status_flag(int fd, int flag)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | flag);
}

int eventfd_channel_open(const char *shm_prefix, int cloexec, int fds[3],
    void **page)
{
    int channel[2] = { -1, -1 }, shm_fd = -1, saved_errno;
    void *mapped;
    if (pipe(channel) != 0) return -1;
    if (guest_fd_admit(channel[0]) < 0) {
        saved_errno = errno;
        close(channel[1]);
        errno = saved_errno;
        return -1;
    }
    if (set_status_flag(channel[0], O_NONBLOCK) != 0 ||
            set_status_flag(channel[1], O_NONBLOCK) != 0 ||
            (cloexec && fcntl(channel[0], F_SETFD, FD_CLOEXEC) != 0))
        goto fail;
    {
        int hidden = hide(channel[1]);
        if (hidden < 0) goto fail;
        channel[1] = hidden;
    }
    shm_fd = create_page(shm_prefix);
    if (shm_fd < 0) goto fail;
    mapped = map_page(shm_fd);
    if (mapped == 0) goto fail;
    fds[0] = channel[0];
    fds[1] = channel[1];
    fds[2] = shm_fd;
    *page = mapped;
    return 0;
fail:
    saved_errno = errno;
    close(channel[0]);
    if (channel[1] >= 0) close(channel[1]);
    if (shm_fd >= 0) close(shm_fd);
    errno = saved_errno;
    return -1;
}

int32_t linux_eventfd_create(uint32_t initial, uint32_t linux_flags)
{
    int fds[3], saved_errno;
    void *page;
    struct eventfd_shared *shared;

    if ((linux_flags & ~(LINUX_EFD_SEMAPHORE | LINUX_EFD_NONBLOCK |
            LINUX_EFD_CLOEXEC)) != 0)
        return -(int32_t)linux_errno_number(EINVAL);
    if (eventfd_channel_open("/linuxemu-eventfd-",
            (linux_flags & LINUX_EFD_CLOEXEC) != 0, fds, &page) != 0)
        return -(int32_t)linux_errno_number(errno);
    shared = page;
    /* The page is fresh (zero) and nobody else sees it yet. */
    shared->semaphore = (linux_flags & LINUX_EFD_SEMAPHORE) != 0;
    shared->nonblock = (linux_flags & LINUX_EFD_NONBLOCK) != 0;
    if (initial != 0) {
        static const char byte = 1;
        shared->counter = initial;
        shared->channel_full = 1;
        if (write(fds[1], &byte, 1) != 1) {
            saved_errno = errno;
            munmap(shared, EVENTFD_PAGE);
            errno = saved_errno;
            goto fail;
        }
    }
    __sync_synchronize();
    if (attach_object(fds[0], fds[1], fds[2], shared) != 0) goto fail;
    return fds[0];
fail:
    saved_errno = errno;
    close(fds[0]);
    close(fds[1]);
    close(fds[2]);
    return -(int32_t)linux_errno_number(saved_errno);
}

static int eventfd_restore(int fd, int hidden_fd, int second_hidden_fd,
    const char *host_path)
{
    void *page;
    (void)host_path;
    if (hidden_fd < 0 || second_hidden_fd < 0) return -1;
    page = map_page(hidden_fd);
    if (page == 0) return -1;
    return attach_object(fd, second_hidden_fd, hidden_fd, page);
}

/* A hidden copy of a companion that arrived over SCM_RIGHTS (the caller
   closes the original). */
static int hidden_copy(int fd)
{
    int copy = fcntl(fd, F_DUPFD, 0), hidden;
    if (copy < 0) return -1;
    hidden = hide(copy);
    if (hidden < 0) close(copy);
    return hidden;
}

/* An eventfd received from another process: fd is the channel read end
   (its native O_NONBLOCK belongs to the shared description), and the
   companions are the channel write end and the shm page. */
static int eventfd_receive(int fd, const int *companions, int count)
{
    struct stat64 status;
    int write_fd, shm_fd;
    void *page;
    if (count != 2 || fstat64(companions[1], &status) != 0 ||
            status.st_size < EVENTFD_PAGE) return -1;
    write_fd = hidden_copy(companions[0]);
    if (write_fd < 0) return -1;
    shm_fd = hidden_copy(companions[1]);
    if (shm_fd < 0) {
        close(write_fd);
        return -1;
    }
    page = map_page(shm_fd);
    if (page == 0 || attach_object(fd, write_fd, shm_fd, page) != 0) {
        close(write_fd);
        close(shm_fd);
        return -1;
    }
    return 0;
}

void linux_eventfd_register(void)
{
    guest_fd_register_restore(GUEST_FD_EVENTFD, eventfd_restore);
    guest_fd_register_receive(GUEST_FD_EVENTFD, eventfd_receive);
}
