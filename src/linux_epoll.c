/*
 * The Linux epoll (roadmap Phase 3) as a guest fd object: create, ctl and
 * the level-triggered wait.
 *
 * One epoll instance is one guest fd object (GUEST_FD_EPOLL).  Its native
 * backing is the read end of a private pipe, the wake channel: the guest's
 * fd number is that read end (a real number for dup, close, fork and exec)
 * and the write end is an emulator-owned hidden fd.  The interest list is
 * process-local, in mmap-backed storage (this runs inside the syscall
 * handler: no malloc or stdio).
 *
 * Items are keyed by guest fd number plus the number's epoch
 * (guest_fd_epoch) and the identity (st_dev, st_ino) of the file it named
 * at ADD.  The fd layer bumps the epoch whenever the number is closed,
 * replaced (dup2/dup3) or handed out again, and calls
 * linux_epoll_note_close, which drops the number's items from every
 * instance and wakes their waiters: QNX poll() is not woken by a close
 * from another thread, and keeps watching the old file.  A wait also drops,
 * without reporting it, an item whose epoch changed, whose number is closed
 * (POLLNVAL) or whose identity differs.  The epoch is what catches one
 * socket replacing another: QNX gives every socket of a family the same
 * (st_dev, st_ino).  ADD runs under the fd-replace lock, so a close or
 * dup2 of its number is wholly before or after it (see linux_epoll_ctl).
 * The identity is checked only for items with something to report, so a
 * wait costs one fstat per reported item, not per registered one.
 *
 * epoll_wait builds a pollfd array from the items (the native poll target
 * and translated events for each, typed fds' ready_now bits beside it),
 * plus the wake channel and, for epoll_pwait with a mask, the thread's
 * signal-wait fd, then polls with the instance unlocked.  Results are
 * taken only when the interest list didn't change during the poll
 * (generation); otherwise the set is rebuilt and polled again with the
 * remaining timeout.  The first item examined rotates between calls, so a
 * busy fd can't starve the others when maxevents is small.
 *
 * Every change to the list (ctl, a close, a dropped stale item) bumps
 * generation and, while a thread is waiting, writes one byte into the wake
 * channel.  The byte is drained only when no waiter still polls a set
 * older than the current generation, so with several waiters none misses
 * the wake.
 *
 * Modes:
 * - EPOLLONESHOT: a reported item is disabled (not polled, nothing
 *   reported, still registered) until a MOD re-arms it, as on Linux.
 * - EPOLLET (an approximation; QNX poll() is level-triggered): the bits an
 *   item reported are "quiet" and left out of its poll set until the guest
 *   does I/O on that fd number (read, write, the vector and positional
 *   forms, send*, recv*, accept*, sendfile: linux_epoll_note_io) or a MOD.
 *   Once HUP or ERR is quiet the item isn't polled at all (poll() reports
 *   them whatever is asked).  Unlike Linux, new data that arrives while the
 *   guest hasn't touched the fd gives no new event until it does; ET
 *   programs read or write until EAGAIN, which re-arms.  Clearing quiet
 *   bits wakes the instance's waiters (changed()), so a write to an eventfd
 *   from another thread wakes a wait blocked on its quiet item.
 * - EPOLLRDHUP: QNX poll() shows a peer's shutdown(SHUT_WR) only as
 *   readable.  For a socket with EPOLLRDHUP requested and readable, a
 *   one-byte recv(MSG_PEEK|MSG_DONTWAIT) that returns 0 on a stream socket
 *   is the shutdown; POLLHUP (a full close) also gives it.  Unlike Linux,
 *   while unread data remains the shutdown isn't seen until it is read.
 * - EPOLLHUP at pipe EOF, which QNX reports only as readable
 *   (linux_poll_pipe_hangup).
 * - EPOLLEXCLUSIVE: Linux's validation (ADD only, with IN/OUT/ERR/HUP/
 *   WAKEUP/ET only, never on an epoll; a MOD of an exclusive item fails
 *   EINVAL), then a plain registration: every waiter may wake.
 * - EPOLLWAKEUP: accepted and dropped (Linux without CAP_BLOCK_SUSPEND).
 *
 * Nesting (an epoll watching an epoll, and an epoll in poll/select):
 * the watched epoll's items are expanded into the poll set and it is
 * readable (EPOLLIN) when one of them would be reported, without consuming
 * EPOLLET or EPOLLONESHOT state (linux_epoll_expand).  epoll_ctl refuses a
 * cycle or more than 5 nested levels with ELOOP, as Linux, from a table of
 * nesting edges.  A process-wide probe channel wakes every expansion when
 * any list changes.  Never more than one instance lock is held at a time.
 *
 * fork: the child's lists are its own copies (mmap private); it gets a
 * private wake channel and fresh locks (linux_epoll_after_fork).  Changes
 * in one process are not seen by the other (Linux shares the instance).
 * exec: an epoll without EPOLL_CLOEXEC carries its list in the exec record
 * when it fits (epoll_serialize); otherwise it arrives empty.  An epoll fd
 * received over SCM_RIGHTS stays native (no companions).
 */
#include "linuxemu.h"
#include "guest_fd.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LINUX_NR_EXECVE 11
#define LINUX_NR_EPOLL_CTL 251

#define LINUX_EPOLL_CLOEXEC 0x00080000u
#define LINUX_EPOLL_CTL_ADD 1
#define LINUX_EPOLL_CTL_DEL 2
#define LINUX_EPOLL_CTL_MOD 3

#define LINUX_EPOLLERR 0x008u
#define LINUX_EPOLLHUP 0x010u
/* IN PRI OUT ERR HUP RDNORM RDBAND WRNORM WRBAND MSG: the poll bits,
   translated by linux_poll_events_to_host. */
#define LINUX_EPOLL_POLL_BITS 0x7dfu
#define LINUX_EPOLLRDHUP 0x2000u
#define LINUX_EPOLLEXCLUSIVE (1u << 28)
#define LINUX_EPOLLWAKEUP (1u << 29)
#define LINUX_EPOLLONESHOT (1u << 30)
#define LINUX_EPOLLET (1u << 31)
/* Linux's EPOLLEXCLUSIVE_OK_BITS: IN, OUT, ERR, HUP, WAKEUP, ET and
   EXCLUSIVE itself. */
#define LINUX_EPOLLEXCLUSIVE_OK_BITS (0x001u | 0x004u | LINUX_EPOLLERR | \
    LINUX_EPOLLHUP | LINUX_EPOLLWAKEUP | LINUX_EPOLLET | LINUX_EPOLLEXCLUSIVE)

/* ARM EABI: 16 bytes, not packed; data at offset 8. */
struct linux_epoll_event {
    uint32_t events;
    uint32_t padding;
    uint64_t data;
};
typedef char linux_epoll_event_is_16_bytes
    [sizeof(struct linux_epoll_event) == 16 ? 1 : -1];

struct epoll_item {
    int fd;                 /* guest fd number as registered */
    unsigned epoch;         /* guest_fd_epoch(fd), read before ADD's fstat */
    dev_t device;           /* identity of its file at ADD */
    ino64_t inode;
    uint32_t events;        /* Linux mask as registered (EPOLLWAKEUP off) */
    uint64_t data;          /* guest user data, returned unchanged */
    uint32_t quiet;         /* EPOLLET: Linux bits reported, until the
                               guest's next I/O on fd or a MOD (set_quiet) */
    int disabled;           /* EPOLLONESHOT: reported, until a MOD */
    /* fd named an epoll at ADD: that instance, for the nesting edge table
       only (compared, never dereferenced; the item holds no reference). */
    struct epoll_instance *child;
};

struct epoll_instance {
    pthread_mutex_t lock;
    struct epoll_item *items;
    size_t count;
    size_t capacity;
    unsigned generation;    /* bumped by every change to items */
    size_t next_start;      /* rotation: first item a wait examines */
    int wake_write;         /* hidden write end of the wake channel */
    int wake_read;          /* its hidden read end, or -1: the guest fd's
                               own native read end (a fork child or a
                               restored image has a private channel) */
    int nonblock;           /* O_NONBLOCK as the guest set it */
    unsigned waiters;       /* threads polling this instance's set */
    unsigned stale_waiters; /* of those, polling a set older than generation */
    int restoring;          /* restored at exec, items not counted yet */
    struct epoll_instance *previous, *next;     /* registry links */
};

/* Every instance of the process, for linux_epoll_note_close.  Lock order:
   the fd-replace lock, registry_lock, an instance lock, then the guest_fd
   table lock. */
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static struct epoll_instance *registry;
/* Items registered on each guest fd number, over all instances: the close
   hook returns at once for a number with none.  Changed atomically, under
   the owning instance's lock. */
static volatile unsigned watched[GUEST_FD_SLOTS];
/* Items with quiet bits on each number, over all instances (guest_fd.h):
   linux_epoll_note_io's one-load fast path.  Changed atomically, under the
   owning instance's lock, only through set_quiet. */
volatile unsigned linux_epoll_et_quiet[GUEST_FD_SLOTS];

/* Nesting: one edge per item that watches an epoll (parent's item names
   child).  epoll_ctl checks a new edge against these for cycles and depth
   without holding two instance locks.  nest_lock is a leaf: taken under an
   instance lock (remove_item) or alone, and nothing is taken under it.
   Edges change only under the owning instance's lock plus nest_lock; ADD
   runs under the fd-replace lock, so no other ADD and no close can change
   the graph while a check runs. */
#define EPOLL_MAX_DEPTH 5
#define NEST_EDGES_MAX 256
struct nest_edge { struct epoll_instance *parent, *child; };
static pthread_mutex_t nest_lock = PTHREAD_MUTEX_INITIALIZER;
static struct nest_edge nest_edges[NEST_EDGES_MAX];
static int nest_count;

/* Probes (poll, select or another epoll's wait looking into an epoll):
   they poll the expanded items' native fds, so a change to any instance's
   list must wake them, as the instance's own wake channel wakes its own
   waiters.  One process-wide channel: changed() bumps probe_generation
   and, while a probe runs, writes a byte; the byte is drained once no
   probe still polls a set older than the change (as for an instance).
   probe_lock is a leaf (taken under an instance lock in changed()). */
static pthread_mutex_t probe_lock = PTHREAD_MUTEX_INITIALIZER;
static int probe_read = -1, probe_write = -1;
static volatile unsigned probers, stale_probers, probe_generation;

/* Per-wait state for one pollfd entry that is an item. */
struct wait_slot {
    short ready;            /* host bits ready_now reported */
    uint32_t reported;      /* Linux events to report, 0 for none */
    int stale;              /* item to drop: closed or another file */
    int nested_fd;          /* item watching an epoll with EPOLLIN: its fd */
    int span_start, span_count;     /* that epoll's expanded entries */
};

static size_t round_to_pages(size_t size)
{
    static size_t page;
    if (page == 0) {
        long value = sysconf(_SC_PAGESIZE);
        page = value > 0 ? (size_t)value : 4096;
    }
    return (size + page - 1) / page * page;
}

/* Interest lists and per-wait poll arrays.  Zeroed; 0 on failure. */
void *epoll_storage_alloc(size_t size)
{
    void *memory;
    if (size == 0 || size > SIZE_MAX / 2) return 0;
    memory = mmap(0, round_to_pages(size), PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    return memory == MAP_FAILED ? 0 : memory;
}

void epoll_storage_free(void *memory, size_t size)
{
    if (memory != 0 && size != 0) munmap(memory, round_to_pages(size));
}

static int32_t linux_error(int host_errno)
{
    return -(int32_t)linux_errno_number(host_errno);
}

static int32_t epoll_read(struct guest_fd_object *object, int fd,
    void *buffer, size_t length)
{
    (void)object; (void)fd; (void)buffer; (void)length;
    return linux_error(EINVAL);
}

static int32_t epoll_write(struct guest_fd_object *object, int fd,
    const void *buffer, size_t length)
{
    (void)object; (void)fd; (void)buffer; (void)length;
    return linux_error(EINVAL);
}

static int epoll_poll_fd(struct guest_fd_object *object, int fd)
{
    (void)object;
    return fd;
}

/* The native read end stays nonblocking (the wait drains it); the guest's
   O_NONBLOCK is kept here.  An epoll file is O_RDWR on Linux. */
static int epoll_get_status(struct guest_fd_object *object, int fd)
{
    struct epoll_instance *instance = guest_fd_object_data(object);
    (void)fd;
    return O_RDWR | (instance->nonblock ? O_NONBLOCK : 0);
}

static int epoll_set_status(struct guest_fd_object *object, int fd,
    int host_flags)
{
    struct epoll_instance *instance = guest_fd_object_data(object);
    (void)fd;
    instance->nonblock = (host_flags & O_NONBLOCK) != 0;
    return 0;
}

static void epoll_release(struct guest_fd_object *object)
{
    struct epoll_instance *instance = guest_fd_object_data(object);
    size_t i;
    pthread_mutex_lock(&registry_lock);
    if (instance->previous != 0) instance->previous->next = instance->next;
    else if (registry == instance) registry = instance->next;
    if (instance->next != 0) instance->next->previous = instance->previous;
    pthread_mutex_unlock(&registry_lock);
    for (i = 0; i != instance->count; ++i) {
        __sync_sub_and_fetch(&watched[instance->items[i].fd], 1u);
        if (instance->items[i].quiet != 0)
            __sync_sub_and_fetch(&linux_epoll_et_quiet[instance->items[i].fd],
                1u);
    }
    /* Its own edges, and any edge still naming it as a child (its parents'
       items go when the number closes; this covers the gap). */
    pthread_mutex_lock(&nest_lock);
    for (i = (size_t)nest_count; i != 0; --i)
        if (nest_edges[i - 1].parent == instance ||
                nest_edges[i - 1].child == instance)
            nest_edges[i - 1] = nest_edges[--nest_count];
    pthread_mutex_unlock(&nest_lock);
    epoll_storage_free(instance->items,
        instance->capacity * sizeof(*instance->items));
    if (instance->wake_write >= 0) close(instance->wake_write);
    if (instance->wake_read >= 0) close(instance->wake_read);
    pthread_mutex_destroy(&instance->lock);
}

static char *append_hex(char *cursor, uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[16];
    int count = 0;
    do { text[count++] = digits[value & 15u]; value >>= 4; }
    while (value != 0);
    while (count != 0) *cursor++ = text[--count];
    return cursor;
}

/* Most text the exec record carries for one instance (it travels hex
   encoded in the record's path field, which holds PATH_MAX - 1 bytes). */
#define EXEC_TEXT_MAX (PATH_MAX - 1)

static int epoll_serialize(struct guest_fd_object *object, int fd,
    char *buffer, size_t size);

static const struct guest_fd_ops epoll_ops = {
    epoll_read, epoll_write, epoll_poll_fd, epoll_get_status,
    epoll_set_status, epoll_release, epoll_serialize, 0, 0
};

/* Exec record body "-1:hex(text)": no hidden fd travels (the new image
   makes its own wake channel); text is "n<nonblock>;" then per item
   "fd,events,data,disabled,quiet;" (hex numbers).  Items on numbers that
   close at exec are left out, as Linux drops them when their file closes.
   When the text doesn't fit (about 30 items) the list is left out and the
   new image's instance is empty: a documented limit. */
static int epoll_serialize(struct guest_fd_object *object, int fd,
    char *buffer, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    struct epoll_instance *instance = guest_fd_object_data(object);
    char text[EXEC_TEXT_MAX + 64], *cursor = text, *out;
    size_t i, length;
    int fits = 1;
    (void)fd;
    *cursor++ = 'n';
    *cursor++ = instance->nonblock ? '1' : '0';
    *cursor++ = ';';
    pthread_mutex_lock(&instance->lock);
    for (i = 0; i != instance->count && fits; ++i) {
        const struct epoll_item *item = &instance->items[i];
        int flags = fcntl(item->fd, F_GETFD);
        if (flags < 0 || (flags & FD_CLOEXEC) != 0 ||
                guest_fd_epoch(item->fd) != item->epoch)
            continue;
        if ((size_t)(cursor - text) + 64 > EXEC_TEXT_MAX) { fits = 0; break; }
        cursor = append_hex(cursor, (uint64_t)item->fd);
        *cursor++ = ',';
        cursor = append_hex(cursor, item->events);
        *cursor++ = ',';
        cursor = append_hex(cursor, item->data);
        *cursor++ = ',';
        *cursor++ = item->disabled ? '1' : '0';
        *cursor++ = ',';
        cursor = append_hex(cursor, item->quiet);
        *cursor++ = ';';
    }
    pthread_mutex_unlock(&instance->lock);
    if (!fits) {
        uint32_t args[6] = { 0, 0, 0, 0, 0, 0 };
        args[0] = (uint32_t)fd;
        args[1] = (uint32_t)instance->count;
        cursor = text + 3;
        /* The documented limit: the new image's instance starts empty. */
        (void)linux_report_unsupported(LINUX_NR_EXECVE,
            linux_errno_number(E2BIG), "epoll-exec-list-too-long", args);
    }
    length = (size_t)(cursor - text);
    if (3 + 2 * length + 1 > size) return -1;
    memcpy(buffer, "-1:", 3);
    out = buffer + 3;
    for (i = 0; i != length; ++i) {
        *out++ = digits[(unsigned char)text[i] >> 4];
        *out++ = digits[(unsigned char)text[i] & 15u];
    }
    *out = '\0';
    return (int)(out - buffer);
}

static int set_status_flag(int fd, int flag)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | flag);
}

int32_t linux_epoll_create(uint32_t linux_flags)
{
    int channel[2], saved_errno;
    struct guest_fd_object *object;
    struct epoll_instance *instance;
    if ((linux_flags & ~LINUX_EPOLL_CLOEXEC) != 0) return linux_error(EINVAL);
    if (pipe(channel) != 0) return linux_error(errno);
    if (guest_fd_admit(channel[0]) < 0) {
        saved_errno = errno;
        close(channel[1]);
        return linux_error(saved_errno);
    }
    if (set_status_flag(channel[0], O_NONBLOCK) != 0 ||
            set_status_flag(channel[1], O_NONBLOCK) != 0 ||
            ((linux_flags & LINUX_EPOLL_CLOEXEC) != 0 &&
            fcntl(channel[0], F_SETFD, FD_CLOEXEC) != 0))
        goto fail;
    {
        int hidden = guest_fd_hide(channel[1]);
        if (hidden < 0) {
            if (errno == EINVAL) errno = EMFILE;
            goto fail;
        }
        channel[1] = hidden;
    }
    object = guest_fd_object_new(GUEST_FD_EPOLL, &epoll_ops, channel[0],
        sizeof(struct epoll_instance));
    if (object == 0) goto fail;
    instance = guest_fd_object_data(object);
    {
        pthread_mutex_t fresh_lock = PTHREAD_MUTEX_INITIALIZER;
        instance->lock = fresh_lock;
    }
    instance->wake_write = channel[1];
    instance->wake_read = -1;
    pthread_mutex_lock(&registry_lock);
    instance->next = registry;
    if (registry != 0) registry->previous = instance;
    registry = instance;
    pthread_mutex_unlock(&registry_lock);
    if (guest_fd_attach(channel[0], object) != 0) {
        saved_errno = errno;
        instance->wake_write = -1;
        guest_fd_put(object);
        errno = saved_errno;
        goto fail;
    }
    guest_fd_put(object);
    return channel[0];
fail:
    saved_errno = errno;
    close(channel[0]);
    close(channel[1]);
    return linux_error(saved_errno);
}

/* The referenced epoll object for epfd, or 0 with *error set: EBADF for a
   number that isn't the guest's open fd, EINVAL for one that isn't an
   epoll. */
static struct guest_fd_object *acquire_epoll(int epfd, int32_t *error)
{
    struct guest_fd_object *object;
    if (epfd < 0 || guest_fd_is_hidden(epfd) || fcntl(epfd, F_GETFD) < 0) {
        *error = linux_error(EBADF);
        return 0;
    }
    object = guest_fd_acquire(epfd);
    if (object == 0 || guest_fd_object_type(object) != GUEST_FD_EPOLL) {
        guest_fd_put(object);
        *error = linux_error(EINVAL);
        return 0;
    }
    return object;
}

/* Guest memory the wait may write: the loader's and the guest's own
   mappings are known; anything else is probed (msync fails ENOMEM on an
   unmapped page). */
static int guest_range_mapped(const void *address, size_t length)
{
    uintptr_t start = (uintptr_t)address, first, last;
    if (address == 0 || length == 0 || length > UINTPTR_MAX - start)
        return 0;
    if (guest_memory_range_owned(start, length)) return 1;
    first = start / round_to_pages(1) * round_to_pages(1);
    last = start + length;
    return msync((void *)first, round_to_pages(last - first), MS_ASYNC) == 0;
}

/* Index of fd's item, or -1.  Caller holds the lock. */
static long find_item(const struct epoll_instance *instance, int fd)
{
    size_t i;
    for (i = 0; i != instance->count; ++i)
        if (instance->items[i].fd == fd) return (long)i;
    return -1;
}

static int grow_items(struct epoll_instance *instance)
{
    size_t capacity = instance->capacity == 0 ? 64 : instance->capacity * 2;
    struct epoll_item *items;
    if (capacity > SIZE_MAX / 2 / sizeof(*items)) { errno = ENOMEM; return -1; }
    items = epoll_storage_alloc(capacity * sizeof(*items));
    if (items == 0) { errno = ENOMEM; return -1; }
    if (instance->count != 0)
        memcpy(items, instance->items, instance->count * sizeof(*items));
    epoll_storage_free(instance->items,
        instance->capacity * sizeof(*instance->items));
    instance->items = items;
    instance->capacity = capacity;
    return 0;
}

static int make_channel(int *read_end, int *write_end)
{
    int channel[2], hidden;
    if (pipe(channel) != 0) return -1;
    if (set_status_flag(channel[0], O_NONBLOCK) != 0 ||
            set_status_flag(channel[1], O_NONBLOCK) != 0 ||
            (hidden = guest_fd_hide(channel[0])) < 0) {
        close(channel[0]);
        close(channel[1]);
        return -1;
    }
    channel[0] = hidden;
    if ((hidden = guest_fd_hide(channel[1])) < 0) {
        close(channel[0]);
        close(channel[1]);
        return -1;
    }
    *read_end = channel[0];
    *write_end = hidden;
    return 0;
}

/* Any instance changed.  Fast path (no probe running): the lock-free
   bump and the read of probers pair with probe_begin's increment and
   snapshot (all full barriers): either this sees the new prober, or the
   prober's snapshot already includes this change (and its expansion, which
   locks the instance after the snapshot, sees the change itself).

   With probers, the generation is bumped again under probe_lock together
   with stale_probers = probers, as an instance does under its lock.  Every
   prober counted then took its snapshot (under probe_lock) before that
   bump, so each one sees a change at probe_end and gives its count back;
   a prober that begins between the two bumps is counted and also sees the
   second one.  (Counting it against only the first bump left
   stale_probers at 1 for good: the byte was never drained and every later
   probe woke at once.) */
static void probe_note_change(void)
{
    __sync_add_and_fetch(&probe_generation, 1u);
    if (__sync_add_and_fetch(&probers, 0u) == 0) return;
    pthread_mutex_lock(&probe_lock);
    if (probers != 0) {
        __sync_add_and_fetch(&probe_generation, 1u);
        stale_probers = probers;
        if (probe_write >= 0) {
            char byte = 1;
            (void)write(probe_write, &byte, 1);
        }
    }
    pthread_mutex_unlock(&probe_lock);
}

/* A probe starts: returns the channel to poll beside the expanded fds
   (-1 when it can't be made: the probe then sees list changes only when
   its poll ends) and the generation to pass to linux_epoll_probe_end. */
int linux_epoll_probe_begin(unsigned *generation)
{
    int channel;
    pthread_mutex_lock(&probe_lock);
    if (probe_read < 0 && make_channel(&probe_read, &probe_write) != 0)
        probe_read = probe_write = -1;
    __sync_add_and_fetch(&probers, 1u);
    *generation = __sync_add_and_fetch(&probe_generation, 0u);
    channel = probe_read;
    pthread_mutex_unlock(&probe_lock);
    return channel;
}

/* A probe ends (woke: its poll reported the channel).  Returns 1 when an
   instance changed since probe_begin: an expansion may be out of date. */
int linux_epoll_probe_end(unsigned generation, int woke)
{
    int stale;
    pthread_mutex_lock(&probe_lock);
    __sync_sub_and_fetch(&probers, 1u);
    stale = __sync_add_and_fetch(&probe_generation, 0u) != generation;
    if (stale && stale_probers != 0) --stale_probers;
    if (woke && stale_probers == 0 && probe_read >= 0) {
        char sink[64];
        while (read(probe_read, sink, sizeof(sink)) > 0) {}
    }
    pthread_mutex_unlock(&probe_lock);
    return stale;
}

/* The list changed: waiters must rebuild.  Caller holds the lock. */
static void changed(struct epoll_instance *instance)
{
    ++instance->generation;
    if (instance->waiters != 0) {
        char byte = 1;
        instance->stale_waiters = instance->waiters;
        /* nonblocking: a full channel is already readable */
        (void)write(instance->wake_write, &byte, 1);
    }
    probe_note_change();
}

/* Caller holds nest_lock. */
static void nest_remove_locked(const struct epoll_instance *parent,
    const struct epoll_instance *child)
{
    int i;
    for (i = 0; i != nest_count; ++i)
        if (nest_edges[i].parent == parent && nest_edges[i].child == child) {
            nest_edges[i] = nest_edges[--nest_count];
            return;
        }
}

static void nest_remove(const struct epoll_instance *parent,
    const struct epoll_instance *child)
{
    pthread_mutex_lock(&nest_lock);
    nest_remove_locked(parent, child);
    pthread_mutex_unlock(&nest_lock);
}

/* Longest chain of edges from start, downwards (down) or upwards, level by
   level; *reached is set when target is on it (start itself included).
   The graph is acyclic (ADD checks), and the walk stops past the limit
   anyway.  Caller holds nest_lock. */
static int nest_longest(const struct epoll_instance *start, int down,
    const struct epoll_instance *target, int *reached)
{
    static const struct epoll_instance *frontier[NEST_EDGES_MAX + 1],
        *next[NEST_EDGES_MAX + 1];
    int frontier_count = 1, level = 0;
    frontier[0] = start;
    if (start == target) *reached = 1;
    while (frontier_count != 0 && level <= EPOLL_MAX_DEPTH) {
        int next_count = 0, e, f, k;
        for (e = 0; e != nest_count; ++e) {
            const struct epoll_instance *from = down ?
                nest_edges[e].parent : nest_edges[e].child;
            const struct epoll_instance *to = down ?
                nest_edges[e].child : nest_edges[e].parent;
            for (f = 0; f != frontier_count && frontier[f] != from; ++f) {}
            if (f == frontier_count) continue;
            for (k = 0; k != next_count && next[k] != to; ++k) {}
            if (k == next_count) next[next_count++] = to;
            if (to == target) *reached = 1;
        }
        if (next_count == 0) break;
        ++level;
        memcpy(frontier, next, (size_t)next_count * sizeof(*next));
        frontier_count = next_count;
    }
    return level;
}

/* Linux's ADD check for an epoll target: ELOOP when parent is reachable
   from child (a cycle, child == parent included) or when the longest
   chain through the new edge would nest more than EPOLL_MAX_DEPTH
   epolls.  0 or a Linux errno. */
static int32_t nest_check(const struct epoll_instance *parent,
    const struct epoll_instance *child)
{
    int cycle = 0, unused = 0, above, below;
    pthread_mutex_lock(&nest_lock);
    below = nest_longest(child, 1, parent, &cycle);
    above = nest_longest(parent, 0, 0, &unused);
    pthread_mutex_unlock(&nest_lock);
    if (cycle || above + 1 + below > EPOLL_MAX_DEPTH)
        return linux_error(ELOOP);
    return 0;
}

/* 0, or -1 (ENOMEM) when the edge table is full.  Caller holds the
   parent's lock. */
static int nest_add(struct epoll_instance *parent,
    struct epoll_instance *child)
{
    int result = 0;
    pthread_mutex_lock(&nest_lock);
    if (nest_count == NEST_EDGES_MAX) { errno = ENOMEM; result = -1; }
    else {
        nest_edges[nest_count].parent = parent;
        nest_edges[nest_count++].child = child;
    }
    pthread_mutex_unlock(&nest_lock);
    return result;
}

/* Keeps linux_epoll_et_quiet in step.  Caller holds the lock. */
static void set_quiet(struct epoll_item *item, uint32_t quiet)
{
    if (item->quiet == 0 && quiet != 0)
        __sync_add_and_fetch(&linux_epoll_et_quiet[item->fd], 1u);
    else if (item->quiet != 0 && quiet == 0)
        __sync_sub_and_fetch(&linux_epoll_et_quiet[item->fd], 1u);
    item->quiet = quiet;
}

static void remove_item(struct epoll_instance *instance, size_t index)
{
    set_quiet(&instance->items[index], 0);
    if (instance->items[index].child != 0)
        nest_remove(instance, instance->items[index].child);
    __sync_sub_and_fetch(&watched[instance->items[index].fd], 1u);
    memmove(instance->items + index, instance->items + index + 1,
        (instance->count - index - 1) * sizeof(*instance->items));
    --instance->count;
    if (instance->next_start >= instance->count) instance->next_start = 0;
    changed(instance);
}

/* The item still names what its number named at ADD.  Identity alone
   can't tell two sockets apart on QNX; the epoch can. */
static int item_current(const struct epoll_item *item,
    const struct stat64 *status)
{
    return guest_fd_epoch(item->fd) == item->epoch &&
        status->st_dev == item->device && status->st_ino == item->inode;
}

void linux_epoll_note_close(int fd)
{
    struct epoll_instance *instance;
    if (fd < 0 || fd >= GUEST_FD_SLOTS ||
            __sync_add_and_fetch(&watched[fd], 0u) == 0)
        return;
    pthread_mutex_lock(&registry_lock);
    for (instance = registry; instance != 0; instance = instance->next) {
        size_t i;
        pthread_mutex_lock(&instance->lock);
        for (i = instance->count; i != 0; --i)
            if (instance->items[i - 1].fd == fd) remove_item(instance, i - 1);
        pthread_mutex_unlock(&instance->lock);
    }
    pthread_mutex_unlock(&registry_lock);
}

/* The guest did I/O on fd: its edge-triggered items report again what is
   ready.  The waiters rebuild their sets with the bits back in. */
void linux_epoll_note_io_slow(int fd)
{
    struct epoll_instance *instance;
    if (fd < 0 || fd >= GUEST_FD_SLOTS) return;
    pthread_mutex_lock(&registry_lock);
    for (instance = registry; instance != 0; instance = instance->next) {
        size_t i;
        int rearmed = 0;
        pthread_mutex_lock(&instance->lock);
        for (i = 0; i != instance->count; ++i)
            if (instance->items[i].fd == fd && instance->items[i].quiet != 0) {
                set_quiet(&instance->items[i], 0);
                rearmed = 1;
            }
        if (rearmed) changed(instance);
        pthread_mutex_unlock(&instance->lock);
    }
    pthread_mutex_unlock(&registry_lock);
}

/* Only the forking thread exists in the child: nothing waits, and a lock
   another thread held is free again.  The interest lists (mmap, private),
   the per-number counts and the nesting edges are the child's own copies,
   so each instance is already independent of the parent's; only the wake
   channel is shared (an inherited pipe).  The child gets a private one:
   otherwise the parent's waiters would take the child's wake bytes and
   the child's the parent's.  If no hidden number is free the inherited
   channel stays (spurious wakes on both sides; still correct, as a waiter
   rechecks the generation).  The probe channel is made again on demand. */
void linux_epoll_after_fork(void)
{
    pthread_mutex_t fresh = PTHREAD_MUTEX_INITIALIZER;
    struct epoll_instance *instance;
    registry_lock = fresh;
    nest_lock = fresh;
    probe_lock = fresh;
    if (probe_read >= 0) close(probe_read);
    if (probe_write >= 0) close(probe_write);
    probe_read = probe_write = -1;
    probers = stale_probers = 0;
    for (instance = registry; instance != 0; instance = instance->next) {
        int read_end, write_end;
        instance->lock = fresh;
        instance->waiters = 0;
        instance->stale_waiters = 0;
        if (make_channel(&read_end, &write_end) == 0) {
            if (instance->wake_read >= 0) close(instance->wake_read);
            if (instance->wake_write >= 0) close(instance->wake_write);
            instance->wake_read = read_end;
            instance->wake_write = write_end;
        }
    }
}

/* Runs under the fd-replace lock, which close, dup2 and dup3 hold while
   they bump fd's epoch and replace the native fd: the epoch read here and
   the fstat that follows name the same file, and a close of fd is wholly
   before this call (fd is EBADF or another file, with a new epoch) or
   after it (and its note_close drops the new item). */
static int32_t epoll_ctl_locked(int epfd, int op, int fd,
    const struct linux_epoll_event *requested_in, const uint32_t *args)
{
    struct linux_epoll_event requested = *requested_in;
    struct stat64 status;
    struct guest_fd_object *object, *target;
    struct epoll_instance *instance, *child = 0;
    int32_t result = 0;
    long index;
    unsigned epoch;
    int has_event = op == LINUX_EPOLL_CTL_ADD || op == LINUX_EPOLL_CTL_MOD;
    epoch = guest_fd_epoch(fd);
    if (epfd < 0 || guest_fd_is_hidden(epfd) || fcntl(epfd, F_GETFD) < 0 ||
            fd < 0 || guest_fd_is_hidden(fd) || fstat64(fd, &status) != 0)
        return linux_error(EBADF);
    /* Regular files and directories have no poll on Linux. */
    if (S_ISREG(status.st_mode) || S_ISDIR(status.st_mode))
        return linux_error(EPERM);
    object = acquire_epoll(epfd, &result);
    if (object == 0) return result;
    if (fd == epfd || (op != LINUX_EPOLL_CTL_ADD &&
            op != LINUX_EPOLL_CTL_DEL && op != LINUX_EPOLL_CTL_MOD)) {
        guest_fd_put(object);
        return linux_error(EINVAL);
    }
    if (has_event) {
        int exclusive, nested;
        if ((requested.events & ~(LINUX_EPOLL_POLL_BITS | LINUX_EPOLLRDHUP |
                LINUX_EPOLLEXCLUSIVE | LINUX_EPOLLWAKEUP |
                LINUX_EPOLLONESHOT | LINUX_EPOLLET)) != 0) {
            guest_fd_put(object);
            return linux_report_unsupported(LINUX_NR_EPOLL_CTL,
                linux_errno_number(EINVAL), "epoll-events-unknown", args);
        }
        /* Linux drops EPOLLWAKEUP without CAP_BLOCK_SUSPEND. */
        requested.events &= ~LINUX_EPOLLWAKEUP;
        target = guest_fd_is_typed(fd) ? guest_fd_acquire(fd) : 0;
        nested = target != 0 &&
            guest_fd_object_type(target) == GUEST_FD_EPOLL;
        /* Only compared (the edge table): the table keeps the target
           alive, and no close of fd can run under the fd-replace lock. */
        if (nested) child = guest_fd_object_data(target);
        guest_fd_put(target);
        /* Linux: EPOLLEXCLUSIVE only on ADD, only beside the OK bits, and
           never on an epoll file. */
        exclusive = (requested.events & LINUX_EPOLLEXCLUSIVE) != 0;
        if (exclusive && (op == LINUX_EPOLL_CTL_MOD || nested ||
                (requested.events & ~LINUX_EPOLLEXCLUSIVE_OK_BITS) != 0)) {
            guest_fd_put(object);
            return linux_error(EINVAL);
        }
    }
    instance = guest_fd_object_data(object);
    /* Linux checks loops and depth on ADD of an epoll, before EEXIST. */
    if (op == LINUX_EPOLL_CTL_ADD && child != 0 &&
            (result = nest_check(instance, child)) != 0) {
        guest_fd_put(object);
        return result;
    }
    pthread_mutex_lock(&instance->lock);
    index = find_item(instance, fd);
    /* An item whose number now names another file is stale: gone, as
       Linux dropped it when its file closed. */
    if (index >= 0 && (instance->items[index].epoch != epoch ||
            instance->items[index].device != status.st_dev ||
            instance->items[index].inode != status.st_ino)) {
        remove_item(instance, (size_t)index);
        index = -1;
    }
    if (op == LINUX_EPOLL_CTL_ADD) {
        if (index >= 0) result = linux_error(EEXIST);
        else if (instance->count == instance->capacity &&
                grow_items(instance) != 0)
            result = linux_error(errno);
        else if (child != 0 && nest_add(instance, child) != 0)
            /* An emulator limit (Linux has none this low): ENOMEM, the
               closest Linux error, reported in the diagnostics. */
            result = linux_report_unsupported(LINUX_NR_EPOLL_CTL,
                linux_errno_number(ENOMEM), "epoll-nest-table-full", args);
        else {
            struct epoll_item *item = &instance->items[instance->count++];
            item->fd = fd;
            item->epoch = epoch;
            item->device = status.st_dev;
            item->inode = status.st_ino;
            item->events = requested.events;
            item->data = requested.data;
            item->quiet = 0;
            item->disabled = 0;
            item->child = child;
            __sync_add_and_fetch(&watched[fd], 1u);
            changed(instance);
        }
    } else if (index < 0) result = linux_error(ENOENT);
    else if (op == LINUX_EPOLL_CTL_MOD) {
        struct epoll_item *item = &instance->items[index];
        /* Linux refuses to modify an exclusive item. */
        if ((item->events & LINUX_EPOLLEXCLUSIVE) != 0)
            result = linux_error(EINVAL);
        else {
            /* MOD re-arms: a disabled one-shot item and the quiet bits of
               an edge-triggered one (Linux re-checks readiness). */
            item->events = requested.events;
            item->data = requested.data;
            set_quiet(item, 0);
            item->disabled = 0;
            changed(instance);
        }
    } else remove_item(instance, (size_t)index);
    pthread_mutex_unlock(&instance->lock);
    guest_fd_put(object);
    return result;
}

int32_t linux_epoll_ctl(int epfd, int op, int fd, const void *event)
{
    struct linux_epoll_event requested;
    int32_t result;
    uint32_t args[6] = { 0, 0, 0, 0, 0, 0 };   /* the diagnostics copy 6 */
    args[0] = (uint32_t)epfd; args[1] = (uint32_t)op;
    args[2] = (uint32_t)fd; args[3] = (uint32_t)(uintptr_t)event;
    /* Linux's order: the event copy, both fds, a file without poll, then
       the epoll checks and the op. */
    memset(&requested, 0, sizeof(requested));
    if (op == LINUX_EPOLL_CTL_ADD || op == LINUX_EPOLL_CTL_MOD) {
        if (!guest_range_mapped(event, sizeof(requested)))
            return linux_error(EFAULT);
        memcpy(&requested, event, sizeof(requested));
    }
    linux_fd_replace_lock();
    result = epoll_ctl_locked(epfd, op, fd, &requested, args);
    linux_fd_replace_unlock();
    return result;
}

/* Nested epoll and epoll in poll/select.  An epoll is readable (Linux
   EPOLLIN) when one of its items would be reported.  linux_epoll_expand
   lays out its items' native poll entries, linux_epoll_expanded_ready
   reads the result after the caller's poll():

     [entry per item, in list order][span of each nested item, in order]

   An item that can't report (a disabled one-shot, quiet HUP/ERR, a stale
   number) has fd -1.  An item watching another epoll for EPOLLIN has
   fd -2 - its guest fd (poll() skips negative fds), and that epoll's own
   span follows the instance's entries.  Only one instance lock is held at
   a time: an instance's entries are written under its lock, and the
   nested epolls are acquired, expanded and released after it is dropped,
   so expansion never orders two instance locks (and a release of the
   last reference never runs under one).  Nothing is consumed: no ET quiet
   bit, no one-shot, no stale item is touched.  The list can change
   between the two calls; the readiness walk then finds entries that don't
   match and reports nothing, and every change bumps probe_generation
   (linux_epoll_probe_end tells the caller to expand again). */
#define NESTED_ENTRY(fd) (-2 - (fd))

static int item_expected_fd(const struct epoll_item *item, short *events)
{
    uint32_t wanted = item->events & ~item->quiet;
    *events = 0;
    if (item->disabled ||
            (item->quiet & (LINUX_EPOLLERR | LINUX_EPOLLHUP)) != 0 ||
            guest_fd_epoch(item->fd) != item->epoch)
        return -1;
    if (item->child != 0)
        return (wanted & 0x001u) != 0 ? NESTED_ENTRY(item->fd) : -1;
    *events = linux_poll_events_to_host(
        (short)(wanted & LINUX_EPOLL_POLL_BITS));
    return item->fd;
}

int linux_epoll_expand(struct guest_fd_object *ep, struct pollfd *out,
    int capacity, int depth)
{
    struct epoll_instance *instance = guest_fd_object_data(ep);
    int count, used, i;
    if (depth > EPOLL_MAX_DEPTH) { errno = ELOOP; return -1; }
    pthread_mutex_lock(&instance->lock);
    count = (int)instance->count;
    if (count > capacity) {
        pthread_mutex_unlock(&instance->lock);
        errno = ENOMEM;
        return -1;
    }
    for (i = 0; i != count; ++i) {
        short events;
        int fd = item_expected_fd(&instance->items[i], &events);
        if (fd >= 0) (void)linux_poll_target_entry(fd, events, &out[i]);
        else {
            out[i].fd = fd;
            out[i].events = 0;
            out[i].revents = 0;
        }
    }
    pthread_mutex_unlock(&instance->lock);
    used = count;
    for (i = 0; i != count; ++i) {
        struct guest_fd_object *child;
        int expanded;
        if (out[i].fd > -2) continue;
        child = guest_fd_acquire(-2 - out[i].fd);
        if (child == 0 || guest_fd_object_type(child) != GUEST_FD_EPOLL) {
            /* Closed or replaced since: the list changed, and the walk
               reports nothing for it. */
            guest_fd_put(child);
            out[i].fd = -1;
            continue;
        }
        expanded = linux_epoll_expand(child, out + used, capacity - used,
            depth + 1);
        guest_fd_put(child);
        if (expanded < 0) return -1;
        used += expanded;
    }
    return used;
}

/* Walks ep's span from in; returns the entries it covers, or -1 when they
   don't match the current list.  *ready is set when an item would be
   reported. */
static int walk_expanded(struct guest_fd_object *ep, const struct pollfd *in,
    int count, int depth, int *ready)
{
    struct epoll_instance *instance = guest_fd_object_data(ep);
    int items, used, i;
    if (depth > EPOLL_MAX_DEPTH) return -1;
    pthread_mutex_lock(&instance->lock);
    items = (int)instance->count;
    if (items > count) {
        pthread_mutex_unlock(&instance->lock);
        return -1;
    }
    for (i = 0; i != items; ++i) {
        const struct epoll_item *item = &instance->items[i];
        struct stat64 status;
        short events, revents, now_bits = 0;
        int expected = item_expected_fd(item, &events), now;
        if (expected >= 0) expected = guest_fd_poll_target(item->fd);
        if (in[i].fd != expected && !(expected == NESTED_ENTRY(item->fd) &&
                in[i].fd == -1)) {
            pthread_mutex_unlock(&instance->lock);
            return -1;
        }
        if (in[i].fd < 0 || *ready) continue;
        now = guest_fd_ready_now(item->fd);
        if (now > 0) now_bits = (short)(now & events);
        revents = (short)(in[i].revents | now_bits);
        if (revents == 0 || (revents & POLLNVAL) != 0 ||
                fstat64(item->fd, &status) != 0 ||
                !item_current(item, &status))
            continue;
        if (now_bits == 0)
            revents = linux_poll_pipe_hangup(item->fd, revents, &status);
        if (((uint16_t)linux_poll_events_to_linux(revents) &
                (item->events | LINUX_EPOLLERR | LINUX_EPOLLHUP) &
                ~item->quiet) != 0)
            *ready = 1;
    }
    pthread_mutex_unlock(&instance->lock);
    used = items;
    for (i = 0; i != items; ++i) {
        struct guest_fd_object *child;
        int walked, child_ready = 0;
        if (in[i].fd > -2) continue;
        child = guest_fd_acquire(-2 - in[i].fd);
        if (child == 0 || guest_fd_object_type(child) != GUEST_FD_EPOLL) {
            guest_fd_put(child);
            return -1;
        }
        walked = walk_expanded(child, in + used, count - used, depth + 1,
            &child_ready);
        guest_fd_put(child);
        if (walked < 0) return -1;
        if (child_ready) *ready = 1;
        used += walked;
    }
    return used;
}

int linux_epoll_expanded_ready(struct guest_fd_object *ep,
    const struct pollfd *in, int count)
{
    int ready = 0;
    return walk_expanded(ep, in, count, 0, &ready) < 0 ? 0 : ready;
}

/* The referenced epoll object on guest fd, or 0 (also for one that is not
   an epoll): poll and select expand those. */
struct guest_fd_object *linux_epoll_acquire(int fd)
{
    struct guest_fd_object *object;
    if (!guest_fd_is_typed(fd)) return 0;
    object = guest_fd_acquire(fd);
    if (object != 0 && guest_fd_object_type(object) != GUEST_FD_EPOLL) {
        guest_fd_put(object);
        object = 0;
    }
    return object;
}

static long long monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Fixed stack buffers cover small interest lists; larger ones map. */
#define WAIT_STACK_ENTRIES 64

struct wait_arrays {
    struct pollfd *host;
    struct wait_slot *slots;
    size_t capacity;        /* entries in host: items, the 3 extra entries
                               (WAIT_EXTRA), then nested epolls' spans */
    size_t mapped_size;     /* 0 when the stack buffers are in use */
};

/* After the items: the wake channel, the signal-wait fd, the probe
   channel (fd -1 when unused). */
#define WAIT_EXTRA 3

/* Room for entries; what the arrays held is kept. */
static int wait_arrays_fit(struct wait_arrays *arrays, size_t entries,
    struct pollfd *stack_host, struct wait_slot *stack_slots)
{
    size_t size;
    void *memory;
    struct pollfd *host;
    struct wait_slot *slots;
    if (entries <= arrays->capacity) return 0;
    if (entries <= WAIT_STACK_ENTRIES) {
        arrays->host = stack_host;
        arrays->slots = stack_slots;
        arrays->capacity = WAIT_STACK_ENTRIES;
        return 0;
    }
    entries *= 2;
    if (entries > SIZE_MAX / (sizeof(struct pollfd) +
            sizeof(struct wait_slot)) / 2) {
        errno = ENOMEM;
        return -1;
    }
    size = entries * (sizeof(struct pollfd) + sizeof(struct wait_slot));
    memory = epoll_storage_alloc(size);
    if (memory == 0) { errno = ENOMEM; return -1; }
    host = memory;
    slots = (struct wait_slot *)(void *)
        ((char *)memory + entries * sizeof(struct pollfd));
    if (arrays->capacity != 0) {
        memcpy(host, arrays->host, arrays->capacity * sizeof(*host));
        memcpy(slots, arrays->slots, arrays->capacity * sizeof(*slots));
    }
    epoll_storage_free(arrays->host, arrays->mapped_size);
    arrays->host = host;
    arrays->slots = slots;
    arrays->capacity = entries;
    arrays->mapped_size = size;
    return 0;
}

/* Drops the items whose number was closed or replaced since ADD, then
   fills host[0..count) from the rest and returns the item count, or -1
   (errno) when the arrays can't grow.  *any_ready is set when ready_now
   already reports something.  Caller holds the lock. */
static long build_set(struct epoll_instance *instance,
    struct wait_arrays *arrays, struct pollfd *stack_host,
    struct wait_slot *stack_slots, int *any_ready)
{
    size_t i;
    *any_ready = 0;
    for (i = instance->count; i != 0; --i)
        if (guest_fd_epoch(instance->items[i - 1].fd) !=
                instance->items[i - 1].epoch)
            remove_item(instance, i - 1);
    if (wait_arrays_fit(arrays, instance->count + WAIT_EXTRA, stack_host,
            stack_slots) != 0)
        return -1;
    for (i = 0; i != instance->count; ++i) {
        const struct epoll_item *item = &instance->items[i];
        short events = linux_poll_events_to_host(
            (short)(item->events & ~item->quiet & LINUX_EPOLL_POLL_BITS));
        arrays->slots[i].reported = 0;
        arrays->slots[i].stale = 0;
        arrays->slots[i].nested_fd = -1;
        arrays->slots[i].span_count = 0;
        /* A disabled one-shot item, or an edge-triggered one whose HUP or
           ERR is quiet (poll() reports those whatever is asked): QNX
           skips a negative fd. */
        if (item->disabled ||
                (item->quiet & (LINUX_EPOLLERR | LINUX_EPOLLHUP)) != 0) {
            arrays->host[i].fd = -1;
            arrays->host[i].events = 0;
            arrays->host[i].revents = 0;
            arrays->slots[i].ready = 0;
            continue;
        }
        if (item->child != 0) {
            /* An epoll: expanded after the lock is dropped (expand_nested),
               reported readable (EPOLLIN) when one of its items is. */
            arrays->host[i].fd = -1;
            arrays->host[i].events = 0;
            arrays->host[i].revents = 0;
            arrays->slots[i].ready = 0;
            if (((item->events & ~item->quiet) & 0x001u) != 0)
                arrays->slots[i].nested_fd = item->fd;
            continue;
        }
        arrays->slots[i].ready = linux_poll_target_entry(item->fd, events,
            &arrays->host[i]);
        if (arrays->slots[i].ready != 0) *any_ready = 1;
    }
    return (long)instance->count;
}

/* A readable stream socket whose peer shut down writing: QNX poll() shows
   only POLLRDNORM (measured, Task 1), and a peek returns 0 with nothing
   left to read.  A zero-length datagram also peeks as 0, hence the type. */
static int peer_shut_down(int fd)
{
    char byte;
    int type = 0, saved_errno = errno;
    socklen_t length = sizeof(type);
    int shut = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT) == 0 &&
        getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) == 0 &&
        type == SOCK_STREAM;
    errno = saved_errno;
    return shut;
}

/* Marks what each item reports, up to maxevents, from the rotation start;
   items closed or now naming another file are marked stale and report
   nothing.  The fd layer bumps a number's epoch before the native close or
   dup2, so revents that came from a new file on the number always meet a
   changed epoch here.  Returns the reported count; *last is the last
   reported index.  Caller holds the lock. */
static int collect(struct epoll_instance *instance, struct wait_arrays *arrays,
    size_t count, int maxevents, size_t *last, int *stale_any)
{
    size_t k, start = instance->next_start < count ? instance->next_start : 0;
    int reported = 0;
    *stale_any = 0;
    for (k = 0; k != count && reported != maxevents; ++k) {
        size_t i = (start + k) % count;
        const struct epoll_item *item = &instance->items[i];
        struct stat64 status;
        short revents = (short)(arrays->host[i].revents |
            arrays->slots[i].ready);
        uint32_t linux_events;
        if (revents == 0) continue;
        if ((revents & POLLNVAL) != 0 || fstat64(item->fd, &status) != 0 ||
                !item_current(item, &status)) {
            arrays->slots[i].stale = 1;
            *stale_any = 1;
            continue;
        }
        if (arrays->slots[i].ready == 0)
            revents = linux_poll_pipe_hangup(item->fd, revents, &status);
        /* EPOLLERR and EPOLLHUP are reported whether requested or not. */
        linux_events = (uint16_t)linux_poll_events_to_linux(revents) &
            (item->events | LINUX_EPOLLERR | LINUX_EPOLLHUP);
        if ((item->events & LINUX_EPOLLRDHUP) != 0 &&
                S_ISSOCK(status.st_mode) &&
                ((revents & POLLHUP) != 0 ||
                ((revents & POLLRDNORM) != 0 && peer_shut_down(item->fd))))
            linux_events |= LINUX_EPOLLRDHUP;
        /* EPOLLET: what was reported stays quiet until the next I/O. */
        linux_events &= ~item->quiet;
        if (linux_events == 0) continue;
        arrays->slots[i].reported = linux_events;
        *last = i;
        ++reported;
    }
    return reported;
}

/* Expands the epolls the nested items watch into the entries from *used,
   with no instance lock held (one lock at a time; see linux_epoll_expand),
   and marks those already ready.  0, or -1 with errno. */
static int expand_nested(struct wait_arrays *arrays, size_t count,
    size_t *used, struct pollfd *stack_host, struct wait_slot *stack_slots,
    int *any_ready)
{
    size_t i;
    for (i = 0; i != count; ++i) {
        struct guest_fd_object *child;
        int expanded;
        if (arrays->slots[i].nested_fd < 0) continue;
        child = linux_epoll_acquire(arrays->slots[i].nested_fd);
        if (child == 0) {
            /* closed or replaced: the list changed, the item goes */
            arrays->slots[i].nested_fd = -1;
            continue;
        }
        for (;;) {
            expanded = linux_epoll_expand(child, arrays->host + *used,
                (int)(arrays->capacity - *used), 1);
            if (expanded >= 0) break;
            if (errno != ENOMEM || arrays->capacity > (size_t)INT_MAX / 4 ||
                    wait_arrays_fit(arrays, arrays->capacity + 1,
                        stack_host, stack_slots) != 0) {
                guest_fd_put(child);
                return -1;
            }
        }
        arrays->slots[i].span_start = (int)*used;
        arrays->slots[i].span_count = expanded;
        *used += (size_t)expanded;
        if (linux_epoll_expanded_ready(child, arrays->host +
                arrays->slots[i].span_start, expanded)) {
            arrays->slots[i].ready = POLLRDNORM;
            *any_ready = 1;
        }
        guest_fd_put(child);
    }
    return 0;
}

/* After the poll: which nested epolls are readable now.  Returns 1 when
   one is. */
static int nested_ready(struct wait_arrays *arrays, size_t count)
{
    size_t i;
    int any = 0;
    for (i = 0; i != count; ++i) {
        struct guest_fd_object *child;
        if (arrays->slots[i].nested_fd < 0) continue;
        child = linux_epoll_acquire(arrays->slots[i].nested_fd);
        arrays->slots[i].ready = child != 0 &&
            linux_epoll_expanded_ready(child, arrays->host +
                arrays->slots[i].span_start, arrays->slots[i].span_count) ?
            POLLRDNORM : 0;
        if (arrays->slots[i].ready != 0) any = 1;
        guest_fd_put(child);
    }
    return any;
}

static void drop_stale(struct epoll_instance *instance,
    const struct wait_arrays *arrays, size_t count)
{
    size_t i = count;
    while (i != 0) {
        --i;
        if (arrays->slots[i].stale) remove_item(instance, i);
    }
}

int32_t linux_epoll_wait(int epfd, void *events, int maxevents,
    int timeout_ms, const void *sigmask, uint32_t sigsetsize)
{
    struct pollfd stack_host[WAIT_STACK_ENTRIES];
    struct wait_slot stack_slots[WAIT_STACK_ENTRIES];
    struct wait_arrays arrays;
    struct linux_poll_mask mask;
    struct guest_fd_object *object;
    struct epoll_instance *instance;
    long long deadline = 0;
    int32_t result = 0;
    int interrupted = 0, wake_fd;
    if (maxevents <= 0 ||
            maxevents > (int)(INT_MAX / sizeof(struct linux_epoll_event)))
        return linux_error(EINVAL);
    object = acquire_epoll(epfd, &result);
    if (object == 0) return result;
    instance = guest_fd_object_data(object);
    /* Set at create, restore or fork, never while the guest runs another
       thread on it. */
    wake_fd = instance->wake_read >= 0 ? instance->wake_read : epfd;
    memset(&arrays, 0, sizeof(arrays));
    mask.signal_wait_fd = -1;
    if (sigmask != 0) {
        result = linux_poll_mask_begin(sigmask, sigsetsize, &mask);
        if (result != 0) {
            guest_fd_put(object);
            return result;
        }
    }
    if (timeout_ms > 0) deadline = monotonic_ms() + timeout_ms;
    for (;;) {
        long count;
        unsigned generation, probe_generation_seen = 0;
        int any_ready, polled, stale_any = 0, reported = 0, nested = 0;
        int timeout = timeout_ms < 0 ? -1 : timeout_ms;
        size_t last = 0, i, used;
        if (timeout_ms > 0) {
            long long left = deadline - monotonic_ms();
            timeout = left < 0 ? 0 : (int)left;
        }
        pthread_mutex_lock(&instance->lock);
        count = build_set(instance, &arrays, stack_host, stack_slots,
            &any_ready);
        generation = instance->generation;
        if (count >= 0) ++instance->waiters;
        pthread_mutex_unlock(&instance->lock);
        if (count < 0) { result = linux_error(errno); break; }
        arrays.host[count].fd = wake_fd;
        arrays.host[count].events = POLLIN;
        arrays.host[count].revents = 0;
        arrays.host[count + 1].fd = mask.signal_wait_fd;
        arrays.host[count + 1].events = POLLIN;
        arrays.host[count + 1].revents = 0;
        arrays.host[count + 2].fd = -1;
        arrays.host[count + 2].events = POLLIN;
        arrays.host[count + 2].revents = 0;
        used = (size_t)count + WAIT_EXTRA;
        for (i = 0; i != (size_t)count; ++i)
            if (arrays.slots[i].nested_fd >= 0) nested = 1;
        if (nested) {
            arrays.host[count + 2].fd =
                linux_epoll_probe_begin(&probe_generation_seen);
            if (expand_nested(&arrays, (size_t)count, &used, stack_host,
                    stack_slots, &any_ready) != 0) {
                result = linux_error(errno);
                (void)linux_epoll_probe_end(probe_generation_seen, 0);
                pthread_mutex_lock(&instance->lock);
                --instance->waiters;
                if (instance->generation != generation &&
                        instance->stale_waiters != 0)
                    --instance->stale_waiters;
                pthread_mutex_unlock(&instance->lock);
                break;
            }
        }
        polled = poll(arrays.host, (nfds_t)used, any_ready ? 0 : timeout);
        if (nested) {
            int saved_errno = errno;
            if (nested_ready(&arrays, (size_t)count)) any_ready = 1;
            (void)linux_epoll_probe_end(probe_generation_seen,
                polled > 0 && arrays.host[count + 2].revents != 0);
            errno = saved_errno;
        }
        if (polled < 0 && errno != EINTR) {
            result = linux_error(errno);
            pthread_mutex_lock(&instance->lock);
            --instance->waiters;
            if (instance->generation != generation &&
                    instance->stale_waiters != 0)
                --instance->stale_waiters;
            pthread_mutex_unlock(&instance->lock);
            break;
        }
        if (polled < 0 || (mask.signal_wait_fd >= 0 &&
                arrays.host[count + 1].revents != 0))
            interrupted = 1;
        pthread_mutex_lock(&instance->lock);
        --instance->waiters;
        if (instance->generation != generation &&
                instance->stale_waiters != 0)
            --instance->stale_waiters;
        /* The wake bytes stay while another waiter still polls an old set:
           it must see them too. */
        if (polled > 0 && arrays.host[count].revents != 0 &&
                instance->stale_waiters == 0) {
            char sink[64];
            while (read(wake_fd, sink, sizeof(sink)) > 0) {}
        }
        if (instance->generation != generation) {
            /* The list changed while polling: results may name other
               items.  Poll the new list (an interrupt still ends it). */
            pthread_mutex_unlock(&instance->lock);
            if (interrupted) { result = linux_error(EINTR); break; }
            continue;
        }
        if (polled > 0 || any_ready)
            reported = collect(instance, &arrays, (size_t)count, maxevents,
                &last, &stale_any);
        if (reported > 0) {
            size_t bytes = (size_t)reported * sizeof(struct linux_epoll_event);
            if (!guest_range_mapped(events, bytes)) {
                pthread_mutex_unlock(&instance->lock);
                result = linux_error(EFAULT);
                interrupted = 0;
                break;
            }
            {
                struct linux_epoll_event *out = events;
                size_t k, start = instance->next_start < (size_t)count ?
                    instance->next_start : 0;
                int written = 0, marked = 0;
                for (k = 0; k != (size_t)count && written != reported; ++k) {
                    struct linux_epoll_event event;
                    struct epoll_item *item;
                    i = (start + k) % (size_t)count;
                    if (arrays.slots[i].reported == 0) continue;
                    item = &instance->items[i];
                    event.events = arrays.slots[i].reported;
                    event.padding = 0;
                    event.data = item->data;
                    memcpy(out + written, &event, sizeof(event));
                    ++written;
                    if ((item->events & LINUX_EPOLLONESHOT) != 0)
                        item->disabled = 1;
                    else if ((item->events & LINUX_EPOLLET) != 0)
                        set_quiet(item, item->quiet | event.events);
                    else continue;
                    marked = 1;
                }
                /* Another thread polling the old set must not report the
                   same one-shot or edge event: it rebuilds. */
                if (marked) changed(instance);
            }
            instance->next_start = (last + 1) % (size_t)count;
        }
        if (stale_any) drop_stale(instance, &arrays, (size_t)count);
        pthread_mutex_unlock(&instance->lock);
        if (reported > 0) {
            /* Events win over a signal, as on Linux; a pending signal is
               delivered when the call returns. */
            interrupted = 0;
            result = reported;
            break;
        }
        if (interrupted) { result = linux_error(EINTR); break; }
        if (stale_any) continue;
        if (timeout_ms == 0 ||
                (timeout_ms > 0 && monotonic_ms() >= deadline))
            break;
        /* Woken by the wake channel or by readiness that reports nothing
           the guest asked for: wait again for the rest of the time. */
    }
    linux_poll_mask_end(&mask, interrupted);
    epoll_storage_free(arrays.host, arrays.mapped_size);
    guest_fd_put(object);
    /* An epoll watching this one with EPOLLET reports it again (the wait
       is the guest's I/O on it). */
    linux_epoll_note_io(epfd);
    return result;
}

static int parse_hex(const char **cursor, char end, uint64_t *value)
{
    const char *text = *cursor;
    uint64_t result = 0;
    int digits = 0;
    for (; *text != end; ++text, ++digits) {
        int digit;
        if (*text >= '0' && *text <= '9') digit = *text - '0';
        else if (*text >= 'a' && *text <= 'f') digit = *text - 'a' + 10;
        else return -1;
        if (digits == 16) return -1;
        result = result << 4 | (uint64_t)digit;
    }
    if (digits == 0) return -1;
    *value = result;
    *cursor = text + 1;
    return 0;
}

/* Restored instances' items still need their epoch, identity and counts
   (linux_epoll_restore_finish): a record restored later may still attach
   an object to an item's number, which bumps its epoch. */
static int restore_pending;

/* An epoll that survived exec: fd is still the guest's native read end;
   text is epoll_serialize's.  A malformed item is skipped. */
static int epoll_restore(int fd, int hidden_fd, int second_hidden_fd,
    const char *text)
{
    struct guest_fd_object *object;
    struct epoll_instance *instance;
    int read_end, write_end;
    if (hidden_fd >= 0 || second_hidden_fd >= 0 || text[0] != 'n' ||
            (text[1] != '0' && text[1] != '1') || text[2] != ';')
        return -1;
    if (make_channel(&read_end, &write_end) != 0) return -1;
    object = guest_fd_object_new(GUEST_FD_EPOLL, &epoll_ops, fd,
        sizeof(struct epoll_instance));
    if (object == 0) {
        close(read_end);
        close(write_end);
        return -1;
    }
    instance = guest_fd_object_data(object);
    {
        pthread_mutex_t fresh_lock = PTHREAD_MUTEX_INITIALIZER;
        instance->lock = fresh_lock;
    }
    instance->wake_read = read_end;
    instance->wake_write = write_end;
    instance->nonblock = text[1] == '1';
    instance->restoring = 1;
    text += 3;
    while (*text != '\0') {
        uint64_t number, events, data, quiet;
        int disabled;
        const char *start = text;
        if (parse_hex(&text, ',', &number) != 0 ||
                parse_hex(&text, ',', &events) != 0 ||
                parse_hex(&text, ',', &data) != 0 ||
                (text[0] != '0' && text[0] != '1') || text[1] != ',') {
            const char *end = strchr(start, ';');
            if (end == 0) break;
            text = end + 1;
            continue;
        }
        disabled = text[0] == '1';
        text += 2;
        if (parse_hex(&text, ';', &quiet) != 0) {
            const char *end = strchr(text, ';');
            if (end == 0) break;
            text = end + 1;
            continue;
        }
        if (number >= GUEST_FD_SLOTS || find_item(instance, (int)number) >= 0
                || (instance->count == instance->capacity &&
                grow_items(instance) != 0))
            continue;
        {
            struct epoll_item *item = &instance->items[instance->count++];
            memset(item, 0, sizeof(*item));
            item->fd = (int)number;
            item->events = (uint32_t)events;
            item->data = data;
            item->disabled = disabled;
            /* the quiet count is taken in restore_finish */
            item->quiet = (uint32_t)quiet;
        }
    }
    pthread_mutex_lock(&registry_lock);
    instance->next = registry;
    if (registry != 0) registry->previous = instance;
    registry = instance;
    pthread_mutex_unlock(&registry_lock);
    if (guest_fd_attach(fd, object) != 0) {
        /* release runs: it must not count items that were never counted */
        instance->count = 0;
        guest_fd_put(object);
        return -1;
    }
    guest_fd_put(object);
    restore_pending = 1;
    return 0;
}

void linux_epoll_register(void)
{
    guest_fd_register_restore(GUEST_FD_EPOLL, epoll_restore);
}

/* After every exec record is restored (single-threaded, at startup): each
   restored item takes its number's epoch and identity now, and is dropped
   if its number isn't open, is hidden, or names a file without poll. */
void linux_epoll_restore_finish(void)
{
    struct epoll_instance *instance;
    if (!restore_pending) return;
    restore_pending = 0;
    for (instance = registry; instance != 0; instance = instance->next) {
        size_t i = 0;
        if (!instance->restoring) continue;
        instance->restoring = 0;
        while (i != instance->count) {
            struct epoll_item *item = &instance->items[i];
            struct stat64 status;
            struct guest_fd_object *target;
            uint32_t quiet = item->quiet;
            if (guest_fd_is_hidden(item->fd) ||
                    fstat64(item->fd, &status) != 0 ||
                    S_ISREG(status.st_mode) || S_ISDIR(status.st_mode)) {
                memmove(item, item + 1,
                    (instance->count - i - 1) * sizeof(*item));
                --instance->count;
                continue;
            }
            target = linux_epoll_acquire(item->fd);
            item->child = 0;
            if (target != 0) {
                item->child = guest_fd_object_data(target);
                if (nest_add(instance, item->child) != 0) {
                    /* Kept as a plain fd it would poll the epoll's native
                       pipe, readable at EOF after exec: drop it. */
                    uint32_t args[6] = { 0, 0, 0, 0, 0, 0 };
                    args[0] = (uint32_t)item->fd;
                    guest_fd_put(target);
                    (void)linux_report_unsupported(LINUX_NR_EXECVE,
                        linux_errno_number(ENOMEM),
                        "epoll-exec-nest-table-full", args);
                    memmove(item, item + 1,
                        (instance->count - i - 1) * sizeof(*item));
                    --instance->count;
                    continue;
                }
            }
            guest_fd_put(target);
            item->epoch = guest_fd_epoch(item->fd);
            item->device = status.st_dev;
            item->inode = status.st_ino;
            item->quiet = 0;
            set_quiet(item, quiet);
            __sync_add_and_fetch(&watched[item->fd], 1u);
            ++i;
        }
    }
}
