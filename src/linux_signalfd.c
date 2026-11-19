/*
 * The Linux signalfd (roadmap Phase 3, part 2) as a guest fd object, with
 * a per-process collector thread and the process-wide pending queue
 * (guest_thread.c).
 *
 * Why a collector.  The guest's signal mask is applied to the host thread,
 * so a process-directed signal that every guest thread blocks never runs
 * Linuxemu's handler: it stays pending in the QNX kernel, which is exactly
 * the signal a signalfd program wants to read.  One emulator thread per
 * process blocks every signal and waits in sigwaitinfo on the union of the
 * live signalfds' masks; QNX hands it those signals (FINDINGS 2026-11-19,
 * sigwait_collect=ok).  Each one goes into the process queue with its
 * siginfo, and every signalfd whose mask holds it gets a readiness byte.
 * No io-pkt message and no signal handler is involved.
 *
 * Never stealing.  If a guest thread has the collected signal unblocked
 * (Linux would have delivered it to that thread), the collector routes it
 * there instead (guest_signal_collected, guest_thread_route_signal): a
 * signal with a guest handler is queued "routed" (delivery takes it,
 * signalfd doesn't see it) and the thread is poked with QNX's SIGEMT,
 * whose handler delivers it; a default or ignored action is sent again to
 * that thread, so the kernel applies it.
 *
 * Re-arming.  When the union changes (a signalfd made, its mask changed,
 * its last reference closed) or a full real-time ring gets room,
 * linux_signalfd_collector_update sends the collector COLLECTOR_WAKE, one
 * of QNX's top eight signals, which QNX keeps masked in every thread
 * (signal.h: "handy for doing synchronous I/O via sigwaitinfo()"), so it
 * can never run a handler or reach another thread.  The collector always
 * has it in its wait set and recomputes the union when it arrives; one
 * sent before the collector re-enters sigwaitinfo stays pending on its
 * thread, so no update is lost.  Cost: one kernel call per update, and no
 * wake-up at all while nothing changes.
 *
 * Real-time back-pressure.  The collector leaves out of its wait set every
 * real-time signal whose process ring is full (guest_process_full_mask),
 * so the rest stay queued in the QNX kernel, in order.  A read that frees
 * a slot wakes the collector when it finishes.  Nothing is dropped; the
 * kernel's own queue limit makes sigqueue fail EAGAIN, as on Linux.
 *
 * Object.  The guest's fd is the read end of a pipe (the readiness
 * channel); the state lives in one page of unnamed shm whose start is a
 * struct eventfd_shared, so the readiness byte follows part 1's protocol
 * through the exported helpers (the counter is a "maybe pending" hint
 * here).  After it: the signalfd's Linux mask (shared by every fd of the
 * open-file description, as on Linux).  SIGKILL and SIGSTOP are dropped
 * from it silently, as on Linux.
 *
 * read.  Fills as many 128-byte struct signalfd_siginfo records as fit
 * (EINVAL below 128 bytes), taking signals in the mask from the calling
 * thread's queue, then the process queue (lowest standard signal first,
 * real-time ones in order), then signals still pending in the QNX kernel
 * that the caller blocks (a zero-timeout sigtimedwait: this is how a
 * signal sent to the calling thread alone is read).  That last step skips
 * the real-time signals the collector is waiting for, so one consumer
 * takes each real-time signal from the kernel and the order is kept; a
 * standard signal taken from a queue also takes away a second instance
 * left in the kernel (one pending instance, as on Linux).  Empty: EAGAIN
 * when nonblocking, else sleep on the channel and the thread's signal-wait
 * channel (part 1's interruptible eventfd wait), re-checking every second.
 *
 * Limits (documented): a signal sent to one thread (tgkill) that it blocks
 * stays in the QNX kernel until that thread reads; read returns it, but
 * poll/epoll don't show it first, and a real-time one is read that way
 * only while the collector isn't taking that signal (its ring is full).
 * A collected signal is visible microseconds after kill/sigqueue returns,
 * once the collector has run.
 *
 * Locking.  signalfd_lock guards the slot table and the collector start.
 * It is a leaf: under it only lock-free counter helpers, pipe writes and
 * pthread_create run.  The collector takes it to compute the union and to
 * raise readiness, never together with thread_registry_lock or the
 * process queue's lock.
 *
 * Not yet (later tasks): applying default and ignore actions of collected
 * signals on delivery, the masked waits' process-queue checks and the
 * collector's wake-ups of waiting threads (Task 5); fork restart, exec
 * records and SCM_RIGHTS (Task 6; until then a signalfd crosses exec and
 * SCM_RIGHTS as a plain pipe).
 */
#include "linuxemu.h"
#include "guest_fd.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define LINUX_SFD_NONBLOCK 0x00000800u
#define LINUX_SFD_CLOEXEC 0x00080000u
#define SIGNALFD_RECORD 128u
#define SIGNALFD_SLOTS 256
#define SIGNALFD_READ_SLICE_MS 1000
/* SIGKILL (9) and SIGSTOP (19). */
#define UNBLOCKABLE_WORD0 ((1u << 8) | (1u << 18))
/* Linux 32..47 (the QNX real-time range): bit 31 of word 0, bits 0-14 of
   word 1. */
#define REALTIME_WORD0 0x80000000u
#define REALTIME_WORD1 0x00007fffu
/* QNX's last signal: one of the top eight, always masked. */
#define COLLECTOR_WAKE (SIGRTMAX + 8)

struct signalfd_shared {
    struct eventfd_shared event;    /* first: the 8-byte aligned counter */
    uint32_t mask[2];               /* Linux signal set */
};

/* Per process.  The guest's fd (the channel read end) is passed to every
   op. */
struct signalfd_data {
    struct signalfd_shared *shared;
    int write_fd;
    int shm_fd;
    int slot;
};

struct signalfd_slot {
    struct signalfd_shared *shared;
    int write_fd;
    int in_use;
};

static pthread_mutex_t signalfd_lock = PTHREAD_MUTEX_INITIALIZER;
static struct signalfd_slot slots[SIGNALFD_SLOTS];
static volatile int collector_started;
static pthread_t collector_thread;
/* The Linux signals the collector is waiting for now. */
static volatile uint32_t collector_armed[2];

static void load_mask(struct signalfd_shared *shared, uint32_t mask[2])
{
    volatile uint32_t *words = shared->mask;
    mask[0] = words[0];
    mask[1] = words[1];
}

/* Raises the channel (the counter is only a hint: 1 = maybe pending). */
static void raise_ready(struct signalfd_shared *shared, int write_fd)
{
    if (eventfd_counter_load(&shared->event) == 0)
        (void)eventfd_counter_add(&shared->event, write_fd, 1);
}

/* A signal in mask waits in the calling thread's queue or (unrouted) in
   the process queue. */
static int has_pending(const uint32_t mask[2])
{
    uint32_t thread[2], process[2];
    guest_thread_pending_mask(thread);
    guest_process_pending_mask(process, 0);
    return ((thread[0] | process[0]) & mask[0]) != 0 ||
        ((thread[1] | process[1]) & mask[1]) != 0;
}

static void collector_union(uint32_t wanted[2])
{
    int index;
    wanted[0] = wanted[1] = 0;
    pthread_mutex_lock(&signalfd_lock);
    for (index = 0; index < SIGNALFD_SLOTS; ++index) {
        uint32_t mask[2];
        if (!slots[index].in_use) continue;
        load_mask(slots[index].shared, mask);
        wanted[0] |= mask[0];
        wanted[1] |= mask[1];
    }
    pthread_mutex_unlock(&signalfd_lock);
}

static void raise_matching(int linux_signal)
{
    uint32_t word = (uint32_t)(linux_signal - 1) / 32u;
    uint32_t bit = 1u << ((uint32_t)(linux_signal - 1) % 32u);
    int index;
    pthread_mutex_lock(&signalfd_lock);
    for (index = 0; index < SIGNALFD_SLOTS; ++index) {
        uint32_t mask[2];
        if (!slots[index].in_use) continue;
        load_mask(slots[index].shared, mask);
        if ((mask[word] & bit) != 0)
            raise_ready(slots[index].shared, slots[index].write_fd);
    }
    pthread_mutex_unlock(&signalfd_lock);
}

static void *collector_main(void *argument)
{
    (void)argument;
    for (;;) {
        uint32_t wanted[2], full[2];
        sigset_t set;
        siginfo_t information;
        int host_signal, linux_signal;
        collector_union(wanted);
        guest_process_full_mask(full);
        wanted[0] &= ~full[0];
        wanted[1] &= ~full[1];
        collector_armed[0] = wanted[0];
        collector_armed[1] = wanted[1];
        __sync_synchronize();
        (void)guest_signal_host_mask(wanted, sizeof(wanted), &set);
        sigaddset(&set, COLLECTOR_WAKE);
        host_signal = sigwaitinfo(&set, &information);
        if (host_signal <= 0 || host_signal == COLLECTOR_WAKE) continue;
        linux_signal = guest_signal_collected(host_signal, &information);
        if (linux_signal != 0) raise_matching(linux_signal);
    }
    return 0;
}

/* Starts the collector once per process (signalfd_lock held).  It blocks
   every signal; sigwaitinfo takes those in its set. */
static int collector_start(void)
{
    pthread_attr_t attributes;
    sigset_t all, old;
    int error;
    if (collector_started) return 0;
    if (guest_signal_poke_install() != 0) return -1;
    sigfillset(&all);
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    error = pthread_create(&collector_thread, &attributes, collector_main, 0);
    pthread_sigmask(SIG_SETMASK, &old, 0);
    pthread_attr_destroy(&attributes);
    if (error != 0) {
        errno = error;
        return -1;
    }
    __sync_synchronize();
    collector_started = 1;
    return 0;
}

static int ensure_collector(void)
{
    int result;
    if (collector_started) return 0;
    pthread_mutex_lock(&signalfd_lock);
    result = collector_start();
    pthread_mutex_unlock(&signalfd_lock);
    return result;
}

void linux_signalfd_collector_update(void)
{
    if (collector_started) (void)pthread_kill(collector_thread, COLLECTOR_WAKE);
}

/* The parent's collector doesn't exist in a fork child, and the lock may
   have been held by a parent thread.  The child's signalfds keep their
   slots; the next signalfd call or read starts the child's collector
   (Task 6 completes fork). */
void linux_signalfd_after_fork(void)
{
    pthread_mutex_init(&signalfd_lock, 0);
    collector_started = 0;
    collector_armed[0] = collector_armed[1] = 0;
}

static int slot_take(struct signalfd_shared *shared, int write_fd)
{
    int index;
    pthread_mutex_lock(&signalfd_lock);
    for (index = 0; index < SIGNALFD_SLOTS; ++index) {
        if (slots[index].in_use) continue;
        slots[index].in_use = 1;
        slots[index].shared = shared;
        slots[index].write_fd = write_fd;
        pthread_mutex_unlock(&signalfd_lock);
        return index;
    }
    pthread_mutex_unlock(&signalfd_lock);
    errno = EMFILE;
    return -1;
}

static void slot_release(int index)
{
    if (index < 0) return;
    pthread_mutex_lock(&signalfd_lock);
    slots[index].in_use = 0;
    slots[index].shared = 0;
    slots[index].write_fd = -1;
    pthread_mutex_unlock(&signalfd_lock);
}

/* A signal in words still pending in the QNX kernel for the calling
   thread (thread-directed, or process-directed and not yet collected),
   without waiting; 0 when none. */
static int take_kernel(const uint32_t words[2], siginfo_t *information)
{
    static const struct timespec zero = { 0, 0 };
    sigset_t set;
    int host_signal;
    if ((words[0] | words[1]) == 0) return 0;
    (void)guest_signal_host_mask(words, 2 * sizeof(uint32_t), &set);
    host_signal = sigtimedwait(&set, information, &zero);
    if (host_signal <= 0) return 0;
    return guest_signal_from_host(host_signal);
}

/* Takes up to capacity signals in the signalfd's mask, writing a record
   for each.  *freed: a full real-time ring got room. */
static int collect_records(const uint32_t mask[2], unsigned char *buffer,
    size_t capacity, int *freed)
{
    uint32_t blocked[2], kernel[2];
    size_t count = 0;
    guest_thread_signal_mask_get(blocked);
    *freed = 0;
    while (count < capacity) {
        siginfo_t information;
        int linux_signal, from_kernel = 0, ring_freed = 0;
        linux_signal = guest_thread_take_pending_mask(mask, &information);
        if (linux_signal == 0) {
            linux_signal = guest_process_take_visible(mask, &information,
                &ring_freed);
            if (ring_freed) *freed = 1;
        }
        if (linux_signal == 0) {
            /* The real-time signals the collector takes are left to it,
               so they reach the reader in kernel order. */
            kernel[0] = mask[0] & blocked[0] &
                ~(collector_armed[0] & REALTIME_WORD0);
            kernel[1] = mask[1] & blocked[1] &
                ~(collector_armed[1] & REALTIME_WORD1);
            linux_signal = take_kernel(kernel, &information);
            from_kernel = 1;
        }
        if (linux_signal == 0) break;
        if (!from_kernel && linux_signal < 32 &&
                (blocked[0] & (1u << (linux_signal - 1))) != 0) {
            /* One pending instance of a standard signal: a copy that
               reached the kernel again after the queued one is the same
               Linux instance.  The collector works beside this without a
               shared lock, so two races remain, both bounded to one
               instance of one standard signal: a duplicate (the collector
               took a second copy from the kernel and queues it after
               this read emptied the queue: a later read returns it once
               more), and a loss (a kill that lands after process_take
               above but before this discarding take_kernel, while the
               collector is between sigwaitinfo and its re-arm, is
               discarded here although Linux would leave it pending).
               Both need a second kill of the same signal inside that
               window. */
            siginfo_t discarded;
            uint32_t single[2];
            single[0] = 1u << (linux_signal - 1);
            single[1] = 0;
            (void)take_kernel(single, &discarded);
        }
        guest_signal_signalfd_record(buffer + count * SIGNALFD_RECORD,
            linux_signal, &information);
        ++count;
    }
    return (int)count;
}

/* After a read: drop the readiness byte, then raise it again when a
   signal in the mask is still queued.  The take comes before the check,
   so a signal the collector queues meanwhile is either seen by the check
   or raises the channel after the take. */
static void settle(struct signalfd_data *data, int fd, const uint32_t mask[2])
{
    (void)eventfd_counter_take(&data->shared->event, data->write_fd, fd);
    if (has_pending(mask)) raise_ready(data->shared, data->write_fd);
}

static int32_t signalfd_read(struct guest_fd_object *object, int fd,
    void *buffer, size_t length)
{
    struct signalfd_data *data = guest_fd_object_data(object);
    size_t capacity = length / SIGNALFD_RECORD;
    if (capacity == 0) return -(int32_t)linux_errno_number(EINVAL);
    if (ensure_collector() != 0) return -(int32_t)linux_errno_number(errno);
    for (;;) {
        uint32_t mask[2];
        int32_t waited;
        int count, freed;
        load_mask(data->shared, mask);
        count = collect_records(mask, buffer, capacity, &freed);
        if (freed) linux_signalfd_collector_update();
        settle(data, fd, mask);
        if (count > 0) return (int32_t)(count * SIGNALFD_RECORD);
        if (has_pending(mask)) continue;
        if (data->shared->event.nonblock)
            return -(int32_t)linux_errno_number(EAGAIN);
        waited = eventfd_wait_readable(&data->shared->event, fd,
            SIGNALFD_READ_SLICE_MS);
        if (waited == 0 && !eventfd_still_attached(object, fd))
            waited = -(int32_t)linux_errno_number(EBADF);
        if (waited != 0) return waited;
    }
}

static int32_t signalfd_write(struct guest_fd_object *object, int fd,
    const void *buffer, size_t length)
{
    (void)object; (void)fd; (void)buffer; (void)length;
    return -(int32_t)linux_errno_number(EINVAL);
}

static int signalfd_poll_fd(struct guest_fd_object *object, int fd)
{
    (void)object;
    return fd;
}

static int signalfd_get_status(struct guest_fd_object *object, int fd)
{
    struct signalfd_data *data = guest_fd_object_data(object);
    (void)fd;
    return O_RDWR | (data->shared->event.nonblock ? O_NONBLOCK : 0);
}

static int signalfd_set_status(struct guest_fd_object *object, int fd,
    int host_flags)
{
    struct signalfd_data *data = guest_fd_object_data(object);
    (void)fd;
    data->shared->event.nonblock = (host_flags & O_NONBLOCK) != 0;
    __sync_synchronize();
    return 0;
}

/* The last reference: the signalfd leaves the union, so the collector
   stops taking its signals unless another signalfd wants them. */
static void signalfd_release(struct guest_fd_object *object)
{
    struct signalfd_data *data = guest_fd_object_data(object);
    slot_release(data->slot);
    linux_signalfd_collector_update();
    if (data->shared != 0) munmap(data->shared, EVENTFD_PAGE);
    if (data->write_fd >= 0) close(data->write_fd);
    if (data->shm_fd >= 0) close(data->shm_fd);
}

/* POLLRDNORM (Linux POLLIN) while a signal in the mask waits in the
   calling thread's queue or the process queue, so a lost channel byte
   can't hide one; never writable, as on Linux. */
static short signalfd_ready_now(struct guest_fd_object *object, int fd)
{
    struct signalfd_data *data = guest_fd_object_data(object);
    uint32_t mask[2];
    (void)fd;
    load_mask(data->shared, mask);
    return has_pending(mask) ? POLLRDNORM : 0;
}

static const struct guest_fd_ops signalfd_ops = {
    signalfd_read, signalfd_write, signalfd_poll_fd, signalfd_get_status,
    signalfd_set_status, signalfd_release, 0, signalfd_ready_now, 0
};

/* Wraps a channel read end, write end, shm fd and its mapping (all the
   caller's until success) into an object attached to read_fd, with a
   slot.  On failure the mapping is gone and the fds are still the
   caller's.  0, or -1 with errno. */
static int attach_object(int read_fd, int write_fd, int shm_fd, void *page)
{
    struct guest_fd_object *object;
    struct signalfd_data *data;
    int slot = slot_take(page, write_fd), saved_errno;
    if (slot < 0) {
        saved_errno = errno;
        munmap(page, EVENTFD_PAGE);
        errno = saved_errno;
        return -1;
    }
    object = guest_fd_object_new(GUEST_FD_SIGNALFD, &signalfd_ops, read_fd,
        sizeof(struct signalfd_data));
    if (object == 0) {
        saved_errno = errno;
        slot_release(slot);
        munmap(page, EVENTFD_PAGE);
        errno = saved_errno;
        return -1;
    }
    data = guest_fd_object_data(object);
    data->shared = page;
    data->write_fd = write_fd;
    data->shm_fd = shm_fd;
    data->slot = slot;
    if (guest_fd_attach(read_fd, object) != 0) {
        saved_errno = errno;
        /* The put releases the slot and the mapping; the fds go back to
           the caller. */
        data->write_fd = -1;
        data->shm_fd = -1;
        guest_fd_put(object);
        errno = saved_errno;
        return -1;
    }
    guest_fd_put(object);
    return 0;
}

/* signalfd(fd, mask, size) with fd an existing signalfd: replace its
   mask (every fd of that open-file description sees it). */
static int32_t update_mask(int fd, const uint32_t words[2])
{
    struct guest_fd_object *object;
    struct signalfd_data *data;
    if (guest_fd_is_hidden(fd) || fcntl(fd, F_GETFD) < 0)
        return -(int32_t)linux_errno_number(EBADF);
    object = guest_fd_acquire(fd);
    if (object == 0 || guest_fd_object_type(object) != GUEST_FD_SIGNALFD) {
        guest_fd_put(object);
        return -(int32_t)linux_errno_number(EINVAL);
    }
    data = guest_fd_object_data(object);
    pthread_mutex_lock(&signalfd_lock);
    data->shared->mask[0] = words[0];
    data->shared->mask[1] = words[1];
    __sync_synchronize();
    (void)collector_start();
    pthread_mutex_unlock(&signalfd_lock);
    linux_signalfd_collector_update();
    if (has_pending(words)) raise_ready(data->shared, data->write_fd);
    guest_fd_put(object);
    return fd;
}

int32_t linux_signalfd(int fd, const void *mask, uint32_t size,
    uint32_t flags)
{
    struct signalfd_shared *shared;
    uint32_t words[2];
    void *page;
    int fds[3], saved_errno;

    if ((flags & ~(LINUX_SFD_NONBLOCK | LINUX_SFD_CLOEXEC)) != 0 ||
            size != sizeof(words))
        return -(int32_t)linux_errno_number(EINVAL);
    if (mask == 0) return -(int32_t)linux_errno_number(EFAULT);
    memcpy(words, mask, sizeof(words));
    words[0] &= ~UNBLOCKABLE_WORD0;
    if (fd != -1) return update_mask(fd, words);
    if (eventfd_channel_open("/linuxemu-signalfd-",
            (flags & LINUX_SFD_CLOEXEC) != 0, fds, &page) != 0)
        return -(int32_t)linux_errno_number(errno);
    shared = page;
    /* Fresh zero page, seen by nobody else yet. */
    shared->event.nonblock = (flags & LINUX_SFD_NONBLOCK) != 0;
    shared->mask[0] = words[0];
    shared->mask[1] = words[1];
    __sync_synchronize();
    if (attach_object(fds[0], fds[1], fds[2], page) != 0) {
        saved_errno = errno;
        close(fds[0]);
        close(fds[1]);
        close(fds[2]);
        return -(int32_t)linux_errno_number(saved_errno);
    }
    if (ensure_collector() != 0) {
        uint32_t args[6] = { 0, 0, 0, 0, 0, 0 };
        args[0] = (uint32_t)fds[0];
        /* Reads still take what the kernel holds for the caller. */
        (void)linux_report_unsupported(355, linux_errno_number(errno),
            "signalfd-collector", args);
    }
    linux_signalfd_collector_update();
    if (has_pending(words)) raise_ready(shared, fds[1]);
    return fds[0];
}
