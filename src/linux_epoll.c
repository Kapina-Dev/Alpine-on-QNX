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
 * Not yet: EPOLLET, EPOLLONESHOT and EPOLLEXCLUSIVE (EINVAL, reported to
 * LINUXEMU_DIAGNOSTICS), EPOLLRDHUP reporting (accepted), nested epoll and
 * epoll in poll/select, and carrying the interest list across fork and
 * exec.
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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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
};

struct epoll_instance {
    pthread_mutex_t lock;
    struct epoll_item *items;
    size_t count;
    size_t capacity;
    unsigned generation;    /* bumped by every change to items */
    size_t next_start;      /* rotation: first item a wait examines */
    int wake_write;         /* hidden write end of the wake channel */
    int nonblock;           /* O_NONBLOCK as the guest set it */
    unsigned waiters;       /* threads polling this instance's set */
    unsigned stale_waiters; /* of those, polling a set older than generation */
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

/* Per-wait state for one pollfd entry that is an item. */
struct wait_slot {
    short ready;            /* host bits ready_now reported */
    uint32_t reported;      /* Linux events to report, 0 for none */
    int stale;              /* item to drop: closed or another file */
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
    for (i = 0; i != instance->count; ++i)
        __sync_sub_and_fetch(&watched[instance->items[i].fd], 1u);
    epoll_storage_free(instance->items,
        instance->capacity * sizeof(*instance->items));
    if (instance->wake_write >= 0) close(instance->wake_write);
    pthread_mutex_destroy(&instance->lock);
}

static const struct guest_fd_ops epoll_ops = {
    epoll_read, epoll_write, epoll_poll_fd, epoll_get_status,
    epoll_set_status, epoll_release, 0, 0, 0
};

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
}

static void remove_item(struct epoll_instance *instance, size_t index)
{
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

/* Only the forking thread exists in the child: nothing waits, and a lock
   another thread held is free again. */
void linux_epoll_after_fork(void)
{
    pthread_mutex_t fresh = PTHREAD_MUTEX_INITIALIZER;
    struct epoll_instance *instance;
    registry_lock = fresh;
    for (instance = registry; instance != 0; instance = instance->next) {
        instance->lock = fresh;
        instance->waiters = 0;
        instance->stale_waiters = 0;
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
    struct epoll_instance *instance;
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
        const char *detail = 0;
        if ((requested.events & ~(LINUX_EPOLL_POLL_BITS | LINUX_EPOLLRDHUP |
                LINUX_EPOLLEXCLUSIVE | LINUX_EPOLLWAKEUP |
                LINUX_EPOLLONESHOT | LINUX_EPOLLET)) != 0)
            detail = "epoll-events-unknown";
        else if ((requested.events & LINUX_EPOLLET) != 0)
            detail = "epoll-et";
        else if ((requested.events & LINUX_EPOLLONESHOT) != 0)
            detail = "epoll-oneshot";
        else if ((requested.events & LINUX_EPOLLEXCLUSIVE) != 0)
            detail = "epoll-exclusive";
        if (detail != 0) {
            guest_fd_put(object);
            return linux_report_unsupported(LINUX_NR_EPOLL_CTL,
                linux_errno_number(EINVAL), detail, args);
        }
        /* Linux drops EPOLLWAKEUP without CAP_BLOCK_SUSPEND. */
        requested.events &= ~LINUX_EPOLLWAKEUP;
        target = guest_fd_is_typed(fd) ? guest_fd_acquire(fd) : 0;
        if (target != 0 && guest_fd_object_type(target) == GUEST_FD_EPOLL) {
            guest_fd_put(target);
            guest_fd_put(object);
            return linux_report_unsupported(LINUX_NR_EPOLL_CTL,
                linux_errno_number(EINVAL), "epoll-nested", args);
        }
        guest_fd_put(target);
    }
    instance = guest_fd_object_data(object);
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
        else {
            struct epoll_item *item = &instance->items[instance->count++];
            item->fd = fd;
            item->epoch = epoch;
            item->device = status.st_dev;
            item->inode = status.st_ino;
            item->events = requested.events;
            item->data = requested.data;
            __sync_add_and_fetch(&watched[fd], 1u);
            changed(instance);
        }
    } else if (index < 0) result = linux_error(ENOENT);
    else if (op == LINUX_EPOLL_CTL_MOD) {
        instance->items[index].events = requested.events;
        instance->items[index].data = requested.data;
        changed(instance);
    } else remove_item(instance, (size_t)index);
    pthread_mutex_unlock(&instance->lock);
    guest_fd_put(object);
    return result;
}

int32_t linux_epoll_ctl(int epfd, int op, int fd, const void *event)
{
    struct linux_epoll_event requested;
    int32_t result;
    uint32_t args[4];
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
    size_t capacity;        /* entries in host (items + 2) */
    size_t mapped_size;     /* 0 when the stack buffers are in use */
};

static int wait_arrays_fit(struct wait_arrays *arrays, size_t entries,
    struct pollfd *stack_host, struct wait_slot *stack_slots)
{
    size_t size;
    void *memory;
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
    epoll_storage_free(arrays->host, arrays->mapped_size);
    arrays->host = memory;
    arrays->slots = (struct wait_slot *)(void *)
        ((char *)memory + entries * sizeof(struct pollfd));
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
    if (wait_arrays_fit(arrays, instance->count + 2, stack_host,
            stack_slots) != 0)
        return -1;
    for (i = 0; i != instance->count; ++i) {
        const struct epoll_item *item = &instance->items[i];
        short events = linux_poll_events_to_host(
            (short)(item->events & LINUX_EPOLL_POLL_BITS));
        arrays->slots[i].reported = 0;
        arrays->slots[i].stale = 0;
        arrays->slots[i].ready = linux_poll_target_entry(item->fd, events,
            &arrays->host[i]);
        if (arrays->slots[i].ready != 0) *any_ready = 1;
    }
    return (long)instance->count;
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
        /* EPOLLERR and EPOLLHUP are reported whether requested or not. */
        linux_events = (uint16_t)linux_poll_events_to_linux(revents) &
            (item->events | LINUX_EPOLLERR | LINUX_EPOLLHUP);
        if (linux_events == 0) continue;
        arrays->slots[i].reported = linux_events;
        *last = i;
        ++reported;
    }
    return reported;
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
    int interrupted = 0;
    if (maxevents <= 0 ||
            maxevents > (int)(INT_MAX / sizeof(struct linux_epoll_event)))
        return linux_error(EINVAL);
    object = acquire_epoll(epfd, &result);
    if (object == 0) return result;
    instance = guest_fd_object_data(object);
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
        unsigned generation;
        int any_ready, polled, stale_any = 0, reported = 0;
        int timeout = timeout_ms < 0 ? -1 : timeout_ms;
        size_t last = 0, i;
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
        arrays.host[count].fd = epfd;
        arrays.host[count].events = POLLIN;
        arrays.host[count].revents = 0;
        if (mask.signal_wait_fd >= 0) {
            arrays.host[count + 1].fd = mask.signal_wait_fd;
            arrays.host[count + 1].events = POLLIN;
            arrays.host[count + 1].revents = 0;
        }
        polled = poll(arrays.host,
            (nfds_t)(count + 1 + (mask.signal_wait_fd >= 0)),
            any_ready ? 0 : timeout);
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
            while (read(epfd, sink, sizeof(sink)) > 0) {}
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
                int written = 0;
                for (k = 0; k != (size_t)count && written != reported; ++k) {
                    struct linux_epoll_event event;
                    i = (start + k) % (size_t)count;
                    if (arrays.slots[i].reported == 0) continue;
                    event.events = arrays.slots[i].reported;
                    event.padding = 0;
                    event.data = instance->items[i].data;
                    memcpy(out + written, &event, sizeof(event));
                    ++written;
                }
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
    return result;
}
