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
 * kept by the rules in raise_channel() and settle_channel().
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
#include <unistd.h>

#define LINUX_EFD_SEMAPHORE 0x00000001u
#define LINUX_EFD_NONBLOCK 0x00000800u
#define LINUX_EFD_CLOEXEC 0x00080000u
#define EVENTFD_COUNTER_MAX 0xfffffffffffffffeULL
#define EVENTFD_PAGE 4096

struct eventfd_shared {
    uint64_t counter;
    uint32_t channel_full;
    uint32_t nonblock;
    uint32_t semaphore;
};

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
   empty removes it (settle_channel), and until then a nonblocking read
   returns EAGAIN, as a spurious wakeup does on Linux. */

/* After moving the counter from 0 to non-zero, or finding it non-zero
   after a settle: the winner of channel_full 0 -> 1 writes the byte. */
static void raise_channel(struct eventfd_data *data)
{
    static const char byte = 1;
    if (__sync_val_compare_and_swap(&data->shared->channel_full, 0, 1) == 0)
        (void)write(data->write_fd, &byte, 1);
}

/* After moving the counter to 0, or finding it 0: empty the pipe, then
   clear channel_full, then re-check the counter.  Draining before the
   clear matters: a writer that raised in between keeps channel_full at 1
   until our clear, and our re-check then sees its count and raises again,
   so a drained byte is always replaced. */
static void settle_channel(struct eventfd_data *data, int read_fd)
{
    char sink[64];
    while (read(read_fd, sink, sizeof(sink)) > 0) {}
    (void)__sync_lock_test_and_set(&data->shared->channel_full, 0);
    __sync_synchronize();
    if (load64(&data->shared->counter) != 0) raise_channel(data);
}

/* Takes what a read returns: the whole counter, or 1 in semaphore mode.
   0 when the counter is empty. */
static uint64_t take_counter(struct eventfd_data *data, int read_fd)
{
    volatile uint64_t *counter = &data->shared->counter;
    uint64_t seen = load64(counter);
    for (;;) {
        uint64_t taken, after, swapped;
        if (seen == 0) break;
        taken = data->shared->semaphore ? 1 : seen;
        after = seen - taken;
        swapped = cas64(counter, seen, after);
        if (swapped != seen) { seen = swapped; continue; }
        if (after == 0) settle_channel(data, read_fd);
        return taken;
    }
    settle_channel(data, read_fd);
    return 0;
}

/* Adds value; 0 when it would pass the maximum. */
static int add_counter(struct eventfd_data *data, uint64_t value)
{
    volatile uint64_t *counter = &data->shared->counter;
    uint64_t seen = load64(counter);
    if (value == 0) return 1;
    for (;;) {
        uint64_t swapped;
        if (EVENTFD_COUNTER_MAX - seen < value) return 0;
        swapped = cas64(counter, seen, seen + value);
        if (swapped == seen) break;
        seen = swapped;
    }
    if (seen == 0) raise_channel(data);
    return 1;
}

static int32_t eventfd_read(struct guest_fd_object *object, int fd,
    void *buffer, size_t length)
{
    struct eventfd_data *data = guest_fd_object_data(object);
    uint64_t value;
    if (length < sizeof(value)) return -(int32_t)linux_errno_number(EINVAL);
    value = take_counter(data, fd);
    /* Blocking reads wait in Task 3; until then an empty counter is
       EAGAIN either way. */
    if (value == 0) return -(int32_t)linux_errno_number(EAGAIN);
    memcpy(buffer, &value, sizeof(value));
    return (int32_t)sizeof(value);
}

static int32_t eventfd_write(struct guest_fd_object *object, int fd,
    const void *buffer, size_t length)
{
    struct eventfd_data *data = guest_fd_object_data(object);
    uint64_t value;
    (void)fd;
    if (length < sizeof(value)) return -(int32_t)linux_errno_number(EINVAL);
    memcpy(&value, buffer, sizeof(value));
    if (value == 0xffffffffffffffffULL)
        return -(int32_t)linux_errno_number(EINVAL);
    if (!add_counter(data, value))
        return -(int32_t)linux_errno_number(EAGAIN);
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

static short eventfd_ready_now(struct guest_fd_object *object, int fd)
{
    struct eventfd_data *data = guest_fd_object_data(object);
    (void)fd;
    return load64(&data->shared->counter) < EVENTFD_COUNTER_MAX ?
        POLLOUT : 0;
}

static const struct guest_fd_ops eventfd_ops = {
    eventfd_read, eventfd_write, eventfd_poll_fd, eventfd_get_status,
    eventfd_set_status, eventfd_release, eventfd_serialize, eventfd_ready_now
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

static char *append_decimal(char *cursor, unsigned value)
{
    char digits[12];
    int count = 0;
    do { digits[count++] = (char)('0' + value % 10); value /= 10; }
    while (value != 0);
    while (count != 0) *cursor++ = digits[--count];
    return cursor;
}

/* A fresh unnamed shm page, hidden.  The name (from the pid and a
   sequence number; no stdio here) is unlinked at once. */
static int create_page(void)
{
    static const char prefix[] = "/linuxemu-eventfd-";
    static unsigned sequence;
    char name[64], *cursor = name + sizeof(prefix) - 1;
    int fd, hidden;
    memcpy(name, prefix, sizeof(prefix) - 1);
    cursor = append_decimal(cursor, (unsigned)getpid());
    *cursor++ = '-';
    cursor = append_decimal(cursor, __sync_fetch_and_add(&sequence, 1));
    *cursor = '\0';
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    shm_unlink(name);
    if (ftruncate(fd, EVENTFD_PAGE) != 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
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

int32_t linux_eventfd_create(uint32_t initial, uint32_t linux_flags)
{
    int channel[2] = { -1, -1 }, shm_fd = -1, saved_errno;
    struct eventfd_shared *shared;

    if ((linux_flags & ~(LINUX_EFD_SEMAPHORE | LINUX_EFD_NONBLOCK |
            LINUX_EFD_CLOEXEC)) != 0)
        return -(int32_t)linux_errno_number(EINVAL);
    if (pipe(channel) != 0) return -(int32_t)linux_errno_number(errno);
    if (guest_fd_admit(channel[0]) < 0) {
        saved_errno = errno;
        close(channel[1]);
        return -(int32_t)linux_errno_number(saved_errno);
    }
    if (set_status_flag(channel[0], O_NONBLOCK) != 0 ||
            set_status_flag(channel[1], O_NONBLOCK) != 0 ||
            ((linux_flags & LINUX_EFD_CLOEXEC) != 0 &&
            fcntl(channel[0], F_SETFD, FD_CLOEXEC) != 0))
        goto fail;
    {
        int hidden = hide(channel[1]);
        if (hidden < 0) goto fail;
        channel[1] = hidden;
    }
    shm_fd = create_page();
    if (shm_fd < 0) goto fail;
    shared = map_page(shm_fd);
    if (shared == 0) goto fail;
    /* The page is fresh (zero) and nobody else sees it yet. */
    shared->semaphore = (linux_flags & LINUX_EFD_SEMAPHORE) != 0;
    shared->nonblock = (linux_flags & LINUX_EFD_NONBLOCK) != 0;
    if (initial != 0) {
        static const char byte = 1;
        shared->counter = initial;
        shared->channel_full = 1;
        if (write(channel[1], &byte, 1) != 1) {
            saved_errno = errno;
            munmap(shared, EVENTFD_PAGE);
            errno = saved_errno;
            goto fail;
        }
    }
    __sync_synchronize();
    if (attach_object(channel[0], channel[1], shm_fd, shared) != 0) goto fail;
    return channel[0];
fail:
    saved_errno = errno;
    close(channel[0]);
    if (channel[1] >= 0) close(channel[1]);
    if (shm_fd >= 0) close(shm_fd);
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

void linux_eventfd_register(void)
{
    guest_fd_register_restore(GUEST_FD_EVENTFD, eventfd_restore);
}
