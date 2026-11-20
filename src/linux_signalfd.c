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
 * Object.  The open-file description is a page of unnamed shm (the
 * Linux mask, a mask generation and O_NONBLOCK; shared by every fd and
 * every process holding it, as on Linux) and a pipe whose read end is the
 * guest's fd; that pipe never carries data, it gives the description its
 * identity and travels through exec records and SCM_RIGHTS.  SIGKILL and
 * SIGSTOP are dropped from the mask silently, as on Linux.
 *
 * Readiness is per process, because what a signalfd reads (the reader's
 * own queues) is: each process holding the signalfd has its own hidden
 * pipe and a private struct eventfd_shared in its slot, kept by part 1's
 * protocol through the exported helpers (the counter is a "maybe
 * pending" hint).  poll, select and epoll wait on that pipe (poll_fd).
 * One shared channel would let one process's empty read drain the byte
 * another process's collector wrote for it, losing that wake-up.
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
 * It is a leaf: under it only lock-free counter helpers, pipe writes, the
 * making of a readiness pipe (pipe, fcntl, close) and pthread_create
 * run.  The collector takes it to compute the union and to
 * raise readiness, never together with thread_registry_lock or the
 * process queue's lock.
 *
 * Delivery of collected signals (guest_signal.c): once a thread lets a
 * queued signal in, delivery takes it whatever its action: a handler runs,
 * an ignoring action discards it, and a default that ends or stops the
 * process is raised again at that thread under the host default.  Every
 * masked wait (ppoll, pselect6, epoll_pwait, rt_sigsuspend, all on the
 * thread's signal-wait channel) checks the queues against its temporary
 * mask before sleeping, and after queueing a signal the collector pokes
 * every thread that has it unblocked, so none sleeps through a signal
 * Linux would deliver.
 *
 * fork.  The child keeps its signalfds (slots, pages) but has no
 * collector, no readiness channels (its copies of the parent's are
 * closed) and an empty process queue (guest_thread_after_fork), as Linux
 * passes no pending signal to a child.  Its channel is made and its
 * collector started lazily, on the first signalfd call, read or readiness
 * check (poll, select, epoll) of a signalfd.
 *
 * A mask change wakes the changing process's collector at once; another
 * process holding the description reloads at its next read or readiness
 * check (the generation moved).  One asleep in a wait meanwhile keeps
 * collecting the old set until then (documented limit).
 *
 * exec.  A signalfd without SFD_CLOEXEC travels in the fd records as
 * "shm:channel:" (eventfd's format); the mask is in the page, so the new
 * image rebuilds the object with its mask and gets a slot and a new
 * readiness channel; its collector starts lazily as after fork.  Signals
 * pending at execve (the process queue included) travel separately
 * (guest_signal_exec_pending) and are raised again process-directed at
 * start-up, so that collector takes them.
 *
 * SCM_RIGHTS.  Linux semantics: a received signalfd reads the receiving
 * process's signals.  The description's pipe write end and the page
 * travel as companions (as eventfd's); the receiver maps the page (mask
 * shared with the sender, as one open-file description) and takes a slot
 * with its own readiness channel, so its own collector serves it.
 */
#include "linuxemu.h"
#include "guest_fd.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
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

/* The open-file description, shared by every process holding it.  Only
   event.nonblock is used of the event part (readiness is per process,
   in the slot). */
struct signalfd_shared {
    struct eventfd_shared event;    /* first: the 8-byte aligned counter */
    uint32_t mask[2];               /* Linux signal set */
    uint32_t generation;            /* bumped by every mask change */
};

/* Per process.  The guest's fd (the read end of the description's pipe,
   which never carries data) is passed to every op. */
struct signalfd_data {
    struct signalfd_shared *shared;
    int write_fd;                   /* that pipe's write end (records) */
    int shm_fd;
    int slot;
};

/* Per process and signalfd: this process's readiness channel.  A fork
   child's inherited copies are closed (linux_signalfd_after_fork) and it
   makes its own on first use; an exec'd image makes one at restore. */
struct signalfd_slot {
    struct eventfd_shared ready;    /* first: the 8-byte aligned hint */
    struct signalfd_shared *shared;
    int channel[2];                 /* hidden pipe: read end, write end */
    int has_channel;
    uint32_t generation;            /* the mask generation last seen */
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

/* Raises this process's channel for the slot (the counter is only a
   hint: 1 = maybe pending). */
static void raise_ready(struct signalfd_slot *slot)
{
    if (!slot->has_channel) return;
    if (eventfd_counter_load(&slot->ready) == 0)
        (void)eventfd_counter_add(&slot->ready, slot->channel[1], 1);
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
        if ((mask[word] & bit) != 0) raise_ready(&slots[index]);
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
   every signal; sigwaitinfo takes those in its set.  The start is lazy,
   from signalfd, read or poll inside the SIGILL dispatch: like
   guest_fd_object_new, this pthread_create (which may allocate) is an
   accepted exception to the no-malloc rule for dispatch paths; it runs
   once per process, with every signal blocked around it. */
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

static void channel_close(struct signalfd_slot *slot)
{
    if (!slot->has_channel) return;
    close(slot->channel[0]);
    close(slot->channel[1]);
    slot->has_channel = 0;
}

/* The parent's collector doesn't exist in a fork child, and the lock may
   have been held by a parent thread.  The child's signalfds keep their
   slots but not the parent's readiness channels (those copies are
   closed: a read here must never drain the parent's wake-up); the next
   signalfd call, read or readiness check makes the child's channel and
   starts its collector. */
void linux_signalfd_after_fork(void)
{
    int index;
    pthread_mutex_init(&signalfd_lock, 0);
    collector_started = 0;
    collector_armed[0] = collector_armed[1] = 0;
    for (index = 0; index < SIGNALFD_SLOTS; ++index)
        channel_close(&slots[index]);
}

/* Makes this process's readiness channel for the slot (signalfd_lock
   held; only pipe, fcntl and close run).  0, or -1 with errno. */
static int channel_make(struct signalfd_slot *slot)
{
    int fds[2], index, saved_errno;
    if (slot->has_channel) return 0;
    if (pipe(fds) != 0) return -1;
    for (index = 0; index < 2; ++index) {
        int flags = fcntl(fds[index], F_GETFL), hidden;
        if (flags < 0 ||
                fcntl(fds[index], F_SETFL, flags | O_NONBLOCK) != 0)
            goto fail;
        hidden = guest_fd_hide(fds[index]);
        if (hidden < 0) {
            if (errno == EINVAL) errno = EMFILE;
            goto fail;
        }
        fds[index] = hidden;
    }
    memset(&slot->ready, 0, sizeof(slot->ready));
    slot->channel[0] = fds[0];
    slot->channel[1] = fds[1];
    __sync_synchronize();
    slot->has_channel = 1;
    return 0;
fail:
    saved_errno = errno;
    close(fds[0]);
    close(fds[1]);
    errno = saved_errno;
    return -1;
}

static int slot_take(struct signalfd_shared *shared)
{
    int index;
    pthread_mutex_lock(&signalfd_lock);
    for (index = 0; index < SIGNALFD_SLOTS; ++index) {
        if (slots[index].in_use) continue;
        slots[index].has_channel = 0;
        if (channel_make(&slots[index]) != 0) {
            pthread_mutex_unlock(&signalfd_lock);
            return -1;
        }
        slots[index].in_use = 1;
        slots[index].shared = shared;
        slots[index].generation = shared->generation;
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
    channel_close(&slots[index]);
    slots[index].in_use = 0;
    slots[index].shared = 0;
    pthread_mutex_unlock(&signalfd_lock);
}

/* Reports a failure of the lazy start-up once per process and kind
   (reads still take what the kernel holds for the caller).  Each kind
   has its own flag, so one failure doesn't hide the other. */
static volatile int reported_channel;
static volatile int reported_collector;

static void report_once(volatile int *reported, const char *detail, int fd)
{
    uint32_t args[6] = { 0, 0, 0, 0, 0, 0 };
    if (__sync_lock_test_and_set(reported, 1) != 0) return;
    args[0] = (uint32_t)fd;
    (void)linux_report_unsupported(355, linux_errno_number(errno), detail,
        args);
}

/* Before any use in this process: the readiness channel (a fork child
   makes its own), a collector wake-up when another process changed the
   mask since this one last looked, and the collector itself.  The slot,
   or 0 when the channel can't be made (errno). */
static struct signalfd_slot *slot_ready(struct signalfd_data *data, int fd)
{
    struct signalfd_slot *slot = &slots[data->slot];
    uint32_t generation;
    if (!slot->has_channel) {
        int result;
        pthread_mutex_lock(&signalfd_lock);
        result = channel_make(slot);
        pthread_mutex_unlock(&signalfd_lock);
        if (result != 0) {
            report_once(&reported_channel, "signalfd-channel", fd);
            return 0;
        }
    }
    generation = *(volatile uint32_t *)&data->shared->generation;
    if (slot->generation != generation) {
        slot->generation = generation;
        linux_signalfd_collector_update();
    }
    if (ensure_collector() != 0)
        report_once(&reported_collector, "signalfd-collector", fd);
    return slot;
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

/* After a read: drop this process's readiness byte, then raise it again
   when a signal in the mask is still queued.  The take comes before the
   check, so a signal the collector queues meanwhile is either seen by
   the check or raises the channel after the take.  The channel is this
   process's alone, so nothing here can take another process's
   wake-up. */
static void settle(struct signalfd_slot *slot, const uint32_t mask[2])
{
    (void)eventfd_counter_take(&slot->ready, slot->channel[1],
        slot->channel[0]);
    if (has_pending(mask)) raise_ready(slot);
}

static int32_t signalfd_read(struct guest_fd_object *object, int fd,
    void *buffer, size_t length)
{
    struct signalfd_data *data = guest_fd_object_data(object);
    struct signalfd_slot *slot;
    size_t capacity = length / SIGNALFD_RECORD;
    if (capacity == 0) return -(int32_t)linux_errno_number(EINVAL);
    slot = slot_ready(data, fd);
    if (slot == 0) return -(int32_t)linux_errno_number(errno);
    for (;;) {
        uint32_t mask[2];
        int32_t waited;
        int count, freed;
        load_mask(data->shared, mask);
        count = collect_records(mask, buffer, capacity, &freed);
        if (freed) linux_signalfd_collector_update();
        settle(slot, mask);
        if (count > 0) return (int32_t)(count * SIGNALFD_RECORD);
        if (has_pending(mask)) continue;
        if (data->shared->event.nonblock)
            return -(int32_t)linux_errno_number(EAGAIN);
        waited = eventfd_wait_readable(&slot->ready, slot->channel[0],
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

/* poll, select and epoll wait on this process's channel, not on the
   guest's fd (whose pipe never carries data). */
static int signalfd_poll_fd(struct guest_fd_object *object, int fd)
{
    struct signalfd_slot *slot = slot_ready(guest_fd_object_data(object), fd);
    return slot == 0 ? fd : slot->channel[0];
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
   can't hide one; never writable, as on Linux.  Every guest wait checks
   here before sleeping, so this is where a fork child's or a new image's
   collector starts when the program only polls. */
static short signalfd_ready_now(struct guest_fd_object *object, int fd)
{
    struct signalfd_data *data = guest_fd_object_data(object);
    uint32_t mask[2];
    (void)slot_ready(data, fd);
    load_mask(data->shared, mask);
    return has_pending(mask) ? POLLRDNORM : 0;
}

/* Exec record body "shm:channel:" (two hidden fds), as eventfd's. */
static int signalfd_serialize(struct guest_fd_object *object, int fd,
    char *buffer, size_t size)
{
    struct signalfd_data *data = guest_fd_object_data(object);
    int length = snprintf(buffer, size, "%d:%d:", data->shm_fd,
        data->write_fd);
    (void)fd;
    return length < 0 || (size_t)length >= size ? -1 : length;
}

/* SCM_RIGHTS companions: the channel write end, then the shm page. */
static int signalfd_companions(struct guest_fd_object *object, int fd,
    int *out, int max)
{
    struct signalfd_data *data = guest_fd_object_data(object);
    (void)fd;
    if (max < 2 || data->write_fd < 0 || data->shm_fd < 0) return -1;
    out[0] = data->write_fd;
    out[1] = data->shm_fd;
    return 2;
}

static const struct guest_fd_ops signalfd_ops = {
    signalfd_read, signalfd_write, signalfd_poll_fd, signalfd_get_status,
    signalfd_set_status, signalfd_release, signalfd_serialize,
    signalfd_ready_now, signalfd_companions
};

/* Wraps a channel read end, write end, shm fd and its mapping (all the
   caller's until success) into an object attached to read_fd, with a
   slot (and this process's readiness channel).  On failure the mapping
   is gone and the fds are still the caller's.  The slot index, or -1
   with errno. */
static int attach_object(int read_fd, int write_fd, int shm_fd, void *page)
{
    struct guest_fd_object *object;
    struct signalfd_data *data;
    int slot = slot_take(page), saved_errno;
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
    return slot;
}

/* signalfd(fd, mask, size) with fd an existing signalfd: replace its
   mask (every fd of that open-file description sees it).  This process's
   collector is woken now; another process holding the same description
   reloads the union at its next read or readiness check of it
   (slot_ready sees the generation move).  Until then (it is asleep in a
   wait) its collector keeps the old set: a newly added signal reaches
   its read (from the kernel) but no wake-up; a removed one is still
   collected there (documented limit). */
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
    slots[data->slot].generation =
        __sync_add_and_fetch(&data->shared->generation, 1u);
    (void)collector_start();
    pthread_mutex_unlock(&signalfd_lock);
    linux_signalfd_collector_update();
    {
        struct signalfd_slot *slot = slot_ready(data, fd);
        if (slot != 0 && has_pending(words)) raise_ready(slot);
    }
    guest_fd_put(object);
    return fd;
}

int32_t linux_signalfd(int fd, const void *mask, uint32_t size,
    uint32_t flags)
{
    struct signalfd_shared *shared;
    uint32_t words[2];
    void *page;
    int fds[3], saved_errno, slot;

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
    slot = attach_object(fds[0], fds[1], fds[2], page);
    if (slot < 0) {
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
    if (has_pending(words)) raise_ready(&slots[slot]);
    return fds[0];
}

/* Exec: the record "shm:channel:" names the page (with the mask) and the
   channel write end; fd is the channel read end, which survived exec.
   Runs at start-up, before the carried signals are raised again; the
   collector starts lazily. */
static int signalfd_restore(int fd, int hidden_fd, int second_hidden_fd,
    const char *host_path)
{
    void *page;
    (void)host_path;
    if (hidden_fd < 0 || second_hidden_fd < 0) return -1;
    page = eventfd_map_page(hidden_fd);
    if (page == 0) return -1;
    return attach_object(fd, second_hidden_fd, hidden_fd, page) < 0 ? -1 : 0;
}

/* A signalfd received from another process: fd is the channel read end,
   the companions the channel write end and the shm page.  It reads this
   process's signals (Linux semantics): the slot puts its mask into this
   process's collector union. */
static int signalfd_receive(int fd, const int *companions, int count)
{
    struct stat64 status;
    int write_fd, shm_fd;
    void *page;
    if (count != 2 || fstat64(companions[1], &status) != 0 ||
            status.st_size < EVENTFD_PAGE) return -1;
    write_fd = eventfd_hidden_copy(companions[0]);
    if (write_fd < 0) return -1;
    shm_fd = eventfd_hidden_copy(companions[1]);
    if (shm_fd < 0) {
        close(write_fd);
        return -1;
    }
    page = eventfd_map_page(shm_fd);
    if (page == 0 || attach_object(fd, write_fd, shm_fd, page) < 0) {
        close(write_fd);
        close(shm_fd);
        return -1;
    }
    linux_signalfd_collector_update();
    return 0;
}

void linux_signalfd_register(void)
{
    guest_fd_register_restore(GUEST_FD_SIGNALFD, signalfd_restore);
    guest_fd_register_receive(GUEST_FD_SIGNALFD, signalfd_receive);
}
