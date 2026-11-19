/*
 * The Linux timerfd (roadmap Phase 3, part 2) as a guest fd object, with a
 * per-process timer manager.
 *
 * Object.  Like eventfd: the guest's fd is the read end of a pipe (the
 * readiness channel) and the state lives in one page of unnamed shm.  The
 * page starts with a struct eventfd_shared, so the expiration count and the
 * channel byte follow eventfd's lock-free counter protocol through its
 * exported helpers (eventfd_counter_add/take/set, eventfd_channel_settle).
 * After it: the clock and the armed spec (first deadline and interval in
 * host-clock nanoseconds), how many expirations of this arming are
 * already in the count, and the TFD_TIMER_CANCEL_ON_SET state.
 *
 * Manager.  One emulator thread per process, started by the first arming,
 * blocks every signal and waits in MsgReceivePulse on a private QNX
 * channel.  Each timerfd that has been armed owns one QNX timer
 * (timer_create with SIGEV_PULSE to that channel).  Pulses are only a
 * trigger: QNX delivers at most about one per 1 ms tick and late by
 * milliseconds (FINDINGS 2026-11-19, Phase 3 part 2 probes), so the count
 * is always computed from the clock,
 *     total = floor((now - first_deadline) / interval) + 1,
 * and total minus what was already counted is added.  read, poll and
 * gettime compute the same way, so a late or merged pulse never loses or
 * double-counts an expiration; the pulse only raises readiness for a
 * waiter that sleeps on the channel.  No io-pkt and no signal handler.
 *
 * Counting across processes.  After fork, exec or SCM_RIGHTS several
 * processes map one page, each under its own process-local lock.  The
 * counted field is therefore only advanced by a 64-bit compare-and-swap
 * (eventfd_atomic_cas64): the process whose swap moves counted from seen
 * to total adds total - seen, so each expiration is added exactly once.
 *
 * Who fires.  Only the process that armed a timerfd (or an exec image that
 * inherited it armed) has its QNX timer, so poll/epoll readiness in a fork
 * child or an SCM_RIGHTS receiver comes from the arming process's pulses
 * and stops when that process exits (documented limit).  A blocking read
 * does not depend on it: it sleeps at most until the next expiration and
 * counts from the clock itself.
 *
 * TFD_TIMER_CANCEL_ON_SET.  For an absolute CLOCK_REALTIME(_ALARM) arming
 * the page keeps the realtime - monotonic offset.  It is compared again
 * on every manager wake-up (including the PROCMGR_EVENT_TOD pulse the
 * manager registers for: QNX's "time of day changed" notification), every
 * read and every readiness check; a change above TIMERFD_CLOCK_SET_NS
 * marks the timerfd cancelled and makes it readable, and the next read
 * fails ECANCELED and clears the count (Linux timerfd_canceled).  A step
 * is seen at the next of those points; a blocking read checks at least
 * once a second.  Gradual drift below the threshold moves the baseline
 * along without cancelling.  The flag on any other arming is accepted and
 * has no effect, as on Linux.
 *
 * Locking.  timer_lock (process-local) guards the slot table, the manager
 * start and the spec fields.  It is a leaf: nothing else is taken under
 * it (the counter helpers and the atomics are lock-free and only touch
 * the pipe).
 *
 * Clocks.  CLOCK_REALTIME and CLOCK_MONOTONIC map directly;
 * CLOCK_BOOTTIME and CLOCK_BOOTTIME_ALARM map to CLOCK_MONOTONIC (whether
 * QNX's monotonic clock stops in device sleep is unmeasured:
 * monotonic_in_sleep=unknown), CLOCK_REALTIME_ALARM to CLOCK_REALTIME
 * (linux_time.c reads these clocks the same way).  The *_ALARM clocks fire
 * while the device is awake and cannot wake it.
 *
 * Exec record "shm:channel:" (two hidden fds): the page carries the spec
 * (absolute host-clock first deadline, hence the remaining time) and the
 * count; the new image re-arms its own QNX timer from it.  SCM_RIGHTS
 * companions: the channel write end, then the shm page.
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
#include <sys/neutrino.h>
#include <sys/procmgr.h>
#include <sys/siginfo.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LINUX_CLOCK_REALTIME 0
#define LINUX_CLOCK_MONOTONIC 1
#define LINUX_CLOCK_BOOTTIME 7
#define LINUX_CLOCK_REALTIME_ALARM 8
#define LINUX_CLOCK_BOOTTIME_ALARM 9
#define LINUX_TFD_NONBLOCK 0x00000800u
#define LINUX_TFD_CLOEXEC 0x00080000u
#define LINUX_TFD_TIMER_ABSTIME 1u
#define LINUX_TFD_TIMER_CANCEL_ON_SET 2u
#define LINUX_NR_EXECVE 11

#define NS_PER_SECOND 1000000000LL
#define NS_PER_MS 1000000LL
/* Requested times saturate here (about 146 years), so now + value and
   the expiration arithmetic never overflow. */
#define TIMERFD_NS_MAX ((int64_t)1 << 62)
/* QNX delivers at most one pulse per 1 ms tick anyway; a shorter host
   period only costs CPU.  The count comes from the clock regardless. */
#define TIMERFD_HOST_INTERVAL_MIN 1000000LL
/* Slot index in the low 16 bits of a pulse value, generation above. */
#define TIMERFD_SLOTS 1024
#define TIMERFD_PULSE_CODE (_PULSE_CODE_MINAVAIL + 3)
#define TIMERFD_TOD_CODE (_PULSE_CODE_MINAVAIL + 4)
/* A blocking read re-checks at least this often (a close by another
   thread, a clock change, a lost channel byte). */
#define TIMERFD_READ_SLICE_MS 1000
/* A realtime - monotonic offset change above this is a clock set.  The
   two clocks are sampled with the realtime one on both sides, so the
   sampling error is far below it. */
#define TIMERFD_CLOCK_SET_NS (10 * NS_PER_MS)

struct timerfd_shared {
    struct eventfd_shared event;    /* first: the 8-byte aligned counter */
    int32_t linux_clock;
    int32_t host_clock;
    uint32_t armed;                 /* it_value was non-zero */
    uint32_t reserved;
    int64_t first_deadline;         /* host-clock ns of expiration 1 */
    int64_t interval;               /* ns; 0 for a one-shot timer */
    uint64_t counted;               /* expirations added; CAS only */
    uint32_t cancel_on_set;         /* absolute REALTIME + CANCEL_ON_SET */
    uint32_t cancelled;             /* a clock set seen, not yet read */
    uint64_t cancel_offset;         /* realtime - monotonic ns; CAS only */
};

/* Per process.  The guest's fd (the channel read end) is passed to every
   op. */
struct timerfd_data {
    struct timerfd_shared *shared;
    int write_fd;
    int shm_fd;
    int slot;
};

/* What the manager may touch, under timer_lock, while in_use. */
struct timer_slot {
    struct timerfd_shared *shared;
    int write_fd;
    uint16_t generation;
    uint8_t in_use;
    uint8_t has_timer;
    timer_t timer;
};

static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timer_slot slots[TIMERFD_SLOTS];
static int manager_channel = -1;
static int manager_connection = -1;

static int64_t clock_now(int host_clock)
{
    struct timespec now;
    if (clock_gettime((clockid_t)host_clock, &now) != 0) return 0;
    /* QNX time_t is an unsigned 32-bit count. */
    return (int64_t)now.tv_sec * NS_PER_SECOND + now.tv_nsec;
}

static void store64(volatile uint64_t *pointer, uint64_t value)
{
    uint64_t seen = eventfd_atomic_load64(pointer), swapped;
    while ((swapped = eventfd_atomic_cas64(pointer, seen, value)) != seen)
        seen = swapped;
}

static uint64_t counted_now(struct timerfd_shared *shared)
{
    return eventfd_atomic_load64(&shared->counted);
}

/* Expirations of the current arming due at now. */
static uint64_t due_total(const struct timerfd_shared *shared, int64_t now)
{
    if (!shared->armed || now < shared->first_deadline) return 0;
    if (shared->interval == 0) return 1;
    return (uint64_t)((now - shared->first_deadline) / shared->interval) + 1;
}

/* Adds the expirations due now that no process has counted yet.  The
   swap of counted decides which caller adds them, so two processes
   catching up at once never add the same expiration twice. */
static void catch_up(struct timerfd_shared *shared, int write_fd)
{
    uint64_t total, seen;
    if (!shared->armed) return;
    total = due_total(shared, clock_now(shared->host_clock));
    seen = counted_now(shared);
    while (total > seen) {
        uint64_t swapped = eventfd_atomic_cas64(&shared->counted, seen,
            total);
        if (swapped == seen) {
            /* 0xfffffffffffffffe expirations can't be reached (584000
               years at 1 ns), so a refused add is not handled. */
            (void)eventfd_counter_add(&shared->event, write_fd,
                total - seen);
            return;
        }
        seen = swapped;
    }
}

/* realtime - monotonic, sampled with realtime on both sides of the
   monotonic read (retried when something slept in between). */
static int64_t realtime_offset(void)
{
    int64_t before = 0, monotonic = 0, after = 0;
    int attempt;
    for (attempt = 0; attempt < 3; ++attempt) {
        before = clock_now(CLOCK_REALTIME);
        monotonic = clock_now(CLOCK_MONOTONIC);
        after = clock_now(CLOCK_REALTIME);
        if (after - before < NS_PER_MS) break;
    }
    return before + (after - before) / 2 - monotonic;
}

/* TFD_TIMER_CANCEL_ON_SET: a realtime clock set since the baseline marks
   the timerfd cancelled and readable (Linux: ticks++ and a wake-up). */
static void check_clock_set(struct timerfd_shared *shared, int write_fd)
{
    int64_t offset, baseline, change;
    if (!shared->cancel_on_set) return;
    offset = realtime_offset();
    baseline = (int64_t)eventfd_atomic_load64(&shared->cancel_offset);
    change = offset > baseline ? offset - baseline : baseline - offset;
    store64(&shared->cancel_offset, (uint64_t)offset);
    if (change > TIMERFD_CLOCK_SET_NS &&
            __sync_lock_test_and_set(&shared->cancelled, 1) == 0)
        (void)eventfd_counter_add(&shared->event, write_fd, 1);
}

/* Time to the next expiration and the interval, as gettime reports them
   (lock held).  A one-shot timer that has fired reports 0. */
static void current_spec(const struct timerfd_shared *shared, int64_t *value,
    int64_t *interval)
{
    int64_t now;
    *interval = shared->interval;
    *value = 0;
    if (!shared->armed) return;
    now = clock_now(shared->host_clock);
    if (now < shared->first_deadline) *value = shared->first_deadline - now;
    else if (shared->interval != 0) {
        int64_t passed = (now - shared->first_deadline) / shared->interval + 1;
        *value = shared->first_deadline + passed * shared->interval - now;
    }
}

/* How long a blocking read may sleep before the next expiration is due
   (lock held), in ms: at least 1, at most the read slice. */
static int wait_slice(struct timerfd_shared *shared)
{
    int64_t next, left;
    uint64_t counted;
    if (!shared->armed) return TIMERFD_READ_SLICE_MS;
    counted = counted_now(shared);
    if (shared->interval == 0) {
        if (counted != 0) return TIMERFD_READ_SLICE_MS;
        next = shared->first_deadline;
    } else {
        if (counted > (uint64_t)(TIMERFD_NS_MAX / shared->interval))
            return TIMERFD_READ_SLICE_MS;
        next = shared->first_deadline + (int64_t)counted * shared->interval;
    }
    left = next - clock_now(shared->host_clock);
    if (left <= 0) return 1;
    if (left >= (int64_t)TIMERFD_READ_SLICE_MS * NS_PER_MS)
        return TIMERFD_READ_SLICE_MS;
    return (int)((left + NS_PER_MS - 1) / NS_PER_MS);
}

/* Arms (or with delay 0 disarms) the slot's QNX timer, relative to now. */
static int host_arm(struct timer_slot *slot, int64_t delay, int64_t interval)
{
    struct _itimer setting;
    if (delay <= 0 && slot->shared->armed) delay = 1;
    setting.nsec = (uint64_t)(delay > 0 ? delay : 0);
    setting.interval_nsec = interval == 0 ? 0 : (uint64_t)(
        interval < TIMERFD_HOST_INTERVAL_MIN ? TIMERFD_HOST_INTERVAL_MIN :
        interval);
    if (setting.nsec == 0) setting.interval_nsec = 0;
    return TimerSettime(slot->timer, 0, &setting, 0) == -1 ? -1 : 0;
}

static int slot_timer(int index);

/* Arms the slot's QNX timer for the page's next expiration (lock held):
   an exec image re-arming an inherited timerfd, or a realtime timer after
   the clock was set (QNX timers are armed relative to now). */
static int rearm_from_page(int index)
{
    struct timer_slot *slot = &slots[index];
    struct timerfd_shared *shared = slot->shared;
    int64_t now, delay;
    if (!shared->armed) return 0;
    if (slot_timer(index) != 0) return -1;
    now = clock_now(shared->host_clock);
    if (now < shared->first_deadline) delay = shared->first_deadline - now;
    else if (shared->interval != 0)
        delay = shared->first_deadline + ((now - shared->first_deadline) /
            shared->interval + 1) * shared->interval - now;
    else return 0;      /* a fired one-shot: catch_up counts it */
    return host_arm(slot, delay, shared->interval);
}

static void on_pulse(uint32_t value)
{
    uint32_t index = value & 0xffffu;
    struct timer_slot *slot;
    if (index >= TIMERFD_SLOTS) return;
    pthread_mutex_lock(&timer_lock);
    slot = &slots[index];
    if (slot->in_use && slot->generation == (uint16_t)(value >> 16)) {
        struct timerfd_shared *shared = slot->shared;
        check_clock_set(shared, slot->write_fd);
        catch_up(shared, slot->write_fd);
        /* A one-shot pulse that came before the deadline (QNX rounds to
           ticks, so it shouldn't) would leave a waiter asleep: re-arm. */
        if (shared->armed && shared->interval == 0 &&
                counted_now(shared) == 0 && slot->has_timer) {
            int64_t left = shared->first_deadline -
                clock_now(shared->host_clock);
            if (left > 0) (void)host_arm(slot, left, 0);
        }
    }
    pthread_mutex_unlock(&timer_lock);
}

/* PROCMGR_EVENT_TOD: the realtime clock was set.  Cancel what asked for
   it, and re-arm this process's realtime timers for their absolute
   deadlines. */
static void on_clock_set(void)
{
    int index;
    pthread_mutex_lock(&timer_lock);
    for (index = 0; index < TIMERFD_SLOTS; ++index) {
        struct timer_slot *slot = &slots[index];
        if (!slot->in_use) continue;
        check_clock_set(slot->shared, slot->write_fd);
        if (slot->has_timer && slot->shared->host_clock == CLOCK_REALTIME)
            (void)rearm_from_page(index);
        catch_up(slot->shared, slot->write_fd);
    }
    pthread_mutex_unlock(&timer_lock);
}

static void *manager_main(void *argument)
{
    int channel = (int)(intptr_t)argument;
    for (;;) {
        struct _pulse pulse;
        if (MsgReceivePulse(channel, &pulse, sizeof(pulse), 0) == -1) {
            if (errno == EINTR) continue;
            break;
        }
        if (pulse.code == TIMERFD_PULSE_CODE)
            on_pulse((uint32_t)pulse.value.sival_int);
        else if (pulse.code == TIMERFD_TOD_CODE)
            on_clock_set();
    }
    return 0;
}

/* Starts the manager once per process (lock held).  The thread blocks
   every signal, so no guest or emulator signal is ever routed to it. */
static int manager_start(void)
{
    pthread_attr_t attributes;
    pthread_t thread;
    sigset_t all, old;
    struct sigevent event;
    int channel, connection, error;
    if (manager_connection != -1) return 0;
    channel = ChannelCreate(0);
    if (channel == -1) return -1;
    connection = ConnectAttach(0, 0, channel, _NTO_SIDE_CHANNEL, 0);
    if (connection == -1) {
        error = errno;
        ChannelDestroy(channel);
        errno = error;
        return -1;
    }
    sigfillset(&all);
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    error = pthread_create(&thread, &attributes, manager_main,
        (void *)(intptr_t)channel);
    pthread_sigmask(SIG_SETMASK, &old, 0);
    pthread_attr_destroy(&attributes);
    if (error != 0) {
        ConnectDetach(connection);
        ChannelDestroy(channel);
        errno = error;
        return -1;
    }
    manager_channel = channel;
    manager_connection = connection;
    /* Clock-set notification as a pulse to this channel.  A failure only
       leaves the offset checks at reads and timer pulses. */
    SIGEV_PULSE_INIT(&event, connection, SIGEV_PULSE_PRIO_INHERIT,
        TIMERFD_TOD_CODE, 0);
    (void)procmgr_event_notify_add(PROCMGR_EVENT_TOD, &event);
    return 0;
}

/* The slot's QNX timer, created on first arming (lock held). */
static int slot_timer(int index)
{
    struct timer_slot *slot = &slots[index];
    struct sigevent event;
    if (slot->has_timer) return 0;
    if (manager_start() != 0) return -1;
    SIGEV_PULSE_INIT(&event, manager_connection, SIGEV_PULSE_PRIO_INHERIT,
        TIMERFD_PULSE_CODE,
        (int)((uint32_t)index | ((uint32_t)slot->generation << 16)));
    if (timer_create((clockid_t)slot->shared->host_clock, &event,
            &slot->timer) != 0) return -1;
    slot->has_timer = 1;
    return 0;
}

static int slot_take(struct timerfd_shared *shared, int write_fd)
{
    int index;
    pthread_mutex_lock(&timer_lock);
    for (index = 0; index < TIMERFD_SLOTS; ++index) {
        if (slots[index].in_use) continue;
        slots[index].in_use = 1;
        slots[index].has_timer = 0;
        slots[index].shared = shared;
        slots[index].write_fd = write_fd;
        pthread_mutex_unlock(&timer_lock);
        return index;
    }
    pthread_mutex_unlock(&timer_lock);
    errno = EMFILE;
    return -1;
}

/* After the release a queued pulse for this slot finds another
   generation and is ignored. */
static void slot_release(int index)
{
    struct timer_slot *slot;
    if (index < 0) return;
    pthread_mutex_lock(&timer_lock);
    slot = &slots[index];
    if (slot->has_timer) timer_delete(slot->timer);
    slot->has_timer = 0;
    slot->in_use = 0;
    slot->shared = 0;
    slot->write_fd = -1;
    ++slot->generation;
    pthread_mutex_unlock(&timer_lock);
}

/* Blocks interruptibly (part 1's eventfd wait) until an expiration is
   counted: the channel wakes it on a pulse from the arming process, and
   it never sleeps past the next deadline, so it also counts by itself
   where no process fires the timer any more. */
static int32_t timerfd_read(struct guest_fd_object *object, int fd,
    void *buffer, size_t length)
{
    struct timerfd_data *data = guest_fd_object_data(object);
    struct timerfd_shared *shared = data->shared;
    uint64_t value;
    if (length < sizeof(value)) return -(int32_t)linux_errno_number(EINVAL);
    for (;;) {
        int32_t waited;
        int slice;
        pthread_mutex_lock(&timer_lock);
        check_clock_set(shared, data->write_fd);
        catch_up(shared, data->write_fd);
        slice = wait_slice(shared);
        pthread_mutex_unlock(&timer_lock);
        if (shared->cancelled &&
                __sync_lock_test_and_set(&shared->cancelled, 0) != 0) {
            eventfd_counter_set(&shared->event, data->write_fd, fd, 0);
            return -(int32_t)linux_errno_number(ECANCELED);
        }
        value = eventfd_counter_take(&shared->event, data->write_fd, fd);
        if (value != 0) break;
        if (shared->event.nonblock)
            return -(int32_t)linux_errno_number(EAGAIN);
        waited = eventfd_wait_readable(&shared->event, fd, slice);
        if (waited == 0 && !eventfd_still_attached(object, fd))
            waited = -(int32_t)linux_errno_number(EBADF);
        if (waited != 0) return waited;
    }
    memcpy(buffer, &value, sizeof(value));
    return (int32_t)sizeof(value);
}

static int32_t timerfd_write(struct guest_fd_object *object, int fd,
    const void *buffer, size_t length)
{
    (void)object; (void)fd; (void)buffer; (void)length;
    return -(int32_t)linux_errno_number(EINVAL);
}

static int timerfd_poll_fd(struct guest_fd_object *object, int fd)
{
    (void)object;
    return fd;
}

static int timerfd_get_status(struct guest_fd_object *object, int fd)
{
    struct timerfd_data *data = guest_fd_object_data(object);
    (void)fd;
    return O_RDWR | (data->shared->event.nonblock ? O_NONBLOCK : 0);
}

static int timerfd_set_status(struct guest_fd_object *object, int fd,
    int host_flags)
{
    struct timerfd_data *data = guest_fd_object_data(object);
    (void)fd;
    data->shared->event.nonblock = (host_flags & O_NONBLOCK) != 0;
    __sync_synchronize();
    return 0;
}

static void timerfd_release(struct guest_fd_object *object)
{
    struct timerfd_data *data = guest_fd_object_data(object);
    slot_release(data->slot);
    if (data->shared != 0) munmap(data->shared, EVENTFD_PAGE);
    if (data->write_fd >= 0) close(data->write_fd);
    if (data->shm_fd >= 0) close(data->shm_fd);
}

/* Exec record body "shm:channel:" (two hidden fds), as eventfd's. */
static int timerfd_serialize(struct guest_fd_object *object, int fd,
    char *buffer, size_t size)
{
    struct timerfd_data *data = guest_fd_object_data(object);
    int length = snprintf(buffer, size, "%d:%d:", data->shm_fd,
        data->write_fd);
    (void)fd;
    return length < 0 || (size_t)length >= size ? -1 : length;
}

/* POLLRDNORM (Linux POLLIN) while the count is above 0, an expiration is
   due that no pulse has counted yet, or a clock set cancelled it; never
   writable, as on Linux. */
static short timerfd_ready_now(struct guest_fd_object *object, int fd)
{
    struct timerfd_data *data = guest_fd_object_data(object);
    struct timerfd_shared *shared = data->shared;
    int due;
    (void)fd;
    if (eventfd_counter_load(&shared->event) != 0) return POLLRDNORM;
    pthread_mutex_lock(&timer_lock);
    check_clock_set(shared, data->write_fd);
    due = shared->cancelled || (shared->armed &&
        due_total(shared, clock_now(shared->host_clock)) >
        counted_now(shared));
    pthread_mutex_unlock(&timer_lock);
    return due ? POLLRDNORM : 0;
}

/* SCM_RIGHTS companions: the channel write end, then the shm page. */
static int timerfd_companions(struct guest_fd_object *object, int fd,
    int *out, int max)
{
    struct timerfd_data *data = guest_fd_object_data(object);
    (void)fd;
    if (max < 2 || data->write_fd < 0 || data->shm_fd < 0) return -1;
    out[0] = data->write_fd;
    out[1] = data->shm_fd;
    return 2;
}

static const struct guest_fd_ops timerfd_ops = {
    timerfd_read, timerfd_write, timerfd_poll_fd, timerfd_get_status,
    timerfd_set_status, timerfd_release, timerfd_serialize,
    timerfd_ready_now, timerfd_companions
};

/* Wraps a channel read end, write end, shm fd and its mapping (all the
   caller's until success) into an object attached to read_fd, with a
   slot.  On failure the mapping is gone and the fds are still the
   caller's.  Returns the slot index, or -1 with errno. */
static int attach_object(int read_fd, int write_fd, int shm_fd, void *page)
{
    struct guest_fd_object *object;
    struct timerfd_data *data;
    int slot = slot_take(page, write_fd), saved_errno;
    if (slot < 0) {
        saved_errno = errno;
        munmap(page, EVENTFD_PAGE);
        errno = saved_errno;
        return -1;
    }
    object = guest_fd_object_new(GUEST_FD_TIMERFD, &timerfd_ops, read_fd,
        sizeof(struct timerfd_data));
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

static int map_clock(int32_t linux_clock, int *host_clock)
{
    switch (linux_clock) {
    case LINUX_CLOCK_REALTIME:
    case LINUX_CLOCK_REALTIME_ALARM:
        *host_clock = CLOCK_REALTIME;
        return 0;
    case LINUX_CLOCK_MONOTONIC:
    case LINUX_CLOCK_BOOTTIME:
    case LINUX_CLOCK_BOOTTIME_ALARM:
        *host_clock = CLOCK_MONOTONIC;
        return 0;
    default:
        return -1;
    }
}

int32_t linux_timerfd_create(int32_t clock, uint32_t flags)
{
    struct timerfd_shared *shared;
    void *page;
    int fds[3], host_clock, saved_errno;

    if (map_clock(clock, &host_clock) != 0 ||
            (flags & ~(LINUX_TFD_NONBLOCK | LINUX_TFD_CLOEXEC)) != 0)
        return -(int32_t)linux_errno_number(EINVAL);
    if (eventfd_channel_open("/linuxemu-timerfd-",
            (flags & LINUX_TFD_CLOEXEC) != 0, fds, &page) != 0)
        return -(int32_t)linux_errno_number(errno);
    shared = page;
    /* Fresh zero page, seen by nobody else yet. */
    shared->event.nonblock = (flags & LINUX_TFD_NONBLOCK) != 0;
    shared->linux_clock = clock;
    shared->host_clock = host_clock;
    __sync_synchronize();
    if (attach_object(fds[0], fds[1], fds[2], page) < 0) {
        saved_errno = errno;
        close(fds[0]);
        close(fds[1]);
        close(fds[2]);
        return -(int32_t)linux_errno_number(saved_errno);
    }
    return fds[0];
}

/* The guest itimerspec: two {sec, nsec} pairs, 32- or 64-bit fields.  A
   time64 tv_nsec keeps only its low 32 bits, as Linux's get_timespec64
   does for 32-bit tasks (the upper half is padding). */
static int load_spec(const void *address, int time64, int64_t ns[2])
{
    int i;
    if (address == 0) return -EFAULT;
    for (i = 0; i < 2; ++i) {
        int64_t seconds, nanoseconds;
        if (time64) {
            int64_t fields[2];
            memcpy(fields, (const char *)address + 16 * i, sizeof(fields));
            seconds = fields[0];
            nanoseconds = (int32_t)(uint32_t)(uint64_t)fields[1];
        } else {
            int32_t fields[2];
            memcpy(fields, (const char *)address + 8 * i, sizeof(fields));
            seconds = fields[0];
            nanoseconds = fields[1];
        }
        if (seconds < 0 || nanoseconds < 0 || nanoseconds >= NS_PER_SECOND)
            return -(int32_t)linux_errno_number(EINVAL);
        ns[i] = seconds >= TIMERFD_NS_MAX / NS_PER_SECOND ? TIMERFD_NS_MAX :
            seconds * NS_PER_SECOND + nanoseconds;
    }
    return 0;
}

static void store_spec(void *address, int time64, const int64_t ns[2])
{
    int i;
    for (i = 0; i < 2; ++i) {
        int64_t seconds = ns[i] / NS_PER_SECOND;
        int64_t nanoseconds = ns[i] % NS_PER_SECOND;
        if (time64) {
            int64_t fields[2];
            fields[0] = seconds;
            fields[1] = nanoseconds;
            memcpy((char *)address + 16 * i, fields, sizeof(fields));
        } else {
            int32_t fields[2];
            fields[0] = seconds > INT32_MAX ? INT32_MAX : (int32_t)seconds;
            fields[1] = (int32_t)nanoseconds;
            memcpy((char *)address + 8 * i, fields, sizeof(fields));
        }
    }
}

/* The referenced timerfd object on a guest fd, or 0 with *error set:
   EBADF for no open fd (or a hidden one), EINVAL for another kind. */
static struct guest_fd_object *acquire_timerfd(int fd, int32_t *error)
{
    struct guest_fd_object *object;
    if (guest_fd_is_hidden(fd) || fcntl(fd, F_GETFD) < 0) {
        *error = -(int32_t)linux_errno_number(EBADF);
        return 0;
    }
    object = guest_fd_acquire(fd);
    if (object == 0 || guest_fd_object_type(object) != GUEST_FD_TIMERFD) {
        guest_fd_put(object);
        *error = -(int32_t)linux_errno_number(EINVAL);
        return 0;
    }
    return object;
}

int32_t linux_timerfd_settime(int fd, uint32_t flags, const void *new_spec,
    void *old_spec, int time64)
{
    struct guest_fd_object *object;
    struct timerfd_data *data;
    struct timerfd_shared *shared;
    struct timer_slot *slot;
    int64_t requested[2], previous[2];
    int32_t result;
    int cancel;

    if ((flags & ~(LINUX_TFD_TIMER_ABSTIME | LINUX_TFD_TIMER_CANCEL_ON_SET))
            != 0)
        return -(int32_t)linux_errno_number(EINVAL);
    result = load_spec(new_spec, time64, requested);
    if (result != 0) return result;
    object = acquire_timerfd(fd, &result);
    if (object == 0) return result;
    data = guest_fd_object_data(object);
    shared = data->shared;
    slot = &slots[data->slot];
    /* Linux timerfd_setup_cancel: only an absolute realtime arming can
       be cancelled; elsewhere the flag is accepted and ignored. */
    cancel = (flags & LINUX_TFD_TIMER_CANCEL_ON_SET) != 0 &&
        (flags & LINUX_TFD_TIMER_ABSTIME) != 0 &&
        (shared->linux_clock == LINUX_CLOCK_REALTIME ||
        shared->linux_clock == LINUX_CLOCK_REALTIME_ALARM);
    pthread_mutex_lock(&timer_lock);
    current_spec(shared, &previous[0], &previous[1]);
    if (requested[0] != 0 && slot_timer(data->slot) != 0) {
        result = -(int32_t)linux_errno_number(errno);
        pthread_mutex_unlock(&timer_lock);
        guest_fd_put(object);
        return result;
    }
    {
        int64_t now = clock_now(shared->host_clock), delay = 0;
        shared->cancel_on_set = 0;
        shared->interval = requested[1];
        store64(&shared->counted, 0);
        shared->armed = requested[0] != 0;
        if (shared->armed) {
            shared->first_deadline = (flags & LINUX_TFD_TIMER_ABSTIME) != 0 ?
                requested[0] : now + requested[0];
            delay = shared->first_deadline - now;
        }
        if (cancel) store64(&shared->cancel_offset,
            (uint64_t)realtime_offset());
        (void)__sync_lock_test_and_set(&shared->cancelled, 0);
        __sync_synchronize();
        shared->cancel_on_set = (uint32_t)cancel;
        /* Linux resets the count on every settime. */
        eventfd_counter_set(&shared->event, data->write_fd, fd, 0);
        if (slot->has_timer &&
                host_arm(slot, delay, shared->armed ? shared->interval : 0)
                != 0) {
            result = -(int32_t)linux_errno_number(errno);
            shared->armed = 0;
        }
        /* A deadline already passed counts now, as Linux fires at once. */
        catch_up(shared, data->write_fd);
    }
    pthread_mutex_unlock(&timer_lock);
    guest_fd_put(object);
    if (result == 0 && old_spec != 0) store_spec(old_spec, time64, previous);
    return result;
}

int32_t linux_timerfd_gettime(int fd, void *spec, int time64)
{
    struct guest_fd_object *object;
    struct timerfd_data *data;
    int64_t current[2];
    int32_t result;
    /* The fd first (EBADF, EINVAL), then the pointer, as Linux. */
    object = acquire_timerfd(fd, &result);
    if (object == 0) return result;
    if (spec == 0) {
        guest_fd_put(object);
        return -EFAULT;
    }
    data = guest_fd_object_data(object);
    pthread_mutex_lock(&timer_lock);
    current_spec(data->shared, &current[0], &current[1]);
    pthread_mutex_unlock(&timer_lock);
    guest_fd_put(object);
    store_spec(spec, time64, current);
    return 0;
}

int32_t linux_timerfd_set_ticks(int fd, const void *guest_ticks)
{
    struct guest_fd_object *object;
    struct timerfd_data *data;
    uint64_t ticks;
    int32_t result;
    object = acquire_timerfd(fd, &result);
    if (object == 0)
        return result == -(int32_t)linux_errno_number(EINVAL) ? -ENOTTY :
            result;
    if (guest_ticks == 0) {
        guest_fd_put(object);
        return -EFAULT;
    }
    memcpy(&ticks, guest_ticks, sizeof(ticks));
    if (ticks == 0) {
        guest_fd_put(object);
        return -(int32_t)linux_errno_number(EINVAL);
    }
    data = guest_fd_object_data(object);
    pthread_mutex_lock(&timer_lock);
    /* Expirations due so far are folded into the replaced count. */
    if (data->shared->armed) {
        uint64_t total = due_total(data->shared,
            clock_now(data->shared->host_clock));
        if (total > counted_now(data->shared))
            store64(&data->shared->counted, total);
    }
    eventfd_counter_set(&data->shared->event, data->write_fd, fd, ticks);
    pthread_mutex_unlock(&timer_lock);
    guest_fd_put(object);
    return 0;
}

/* The fork child has neither the manager thread nor the QNX timers.  Its
   timerfds keep their pages (the count stays shared, as on Linux): the
   parent's timers keep raising them, the child's reads count from the
   clock, and a timerfd the child re-arms gets a child-side timer and
   manager. */
void linux_timerfd_after_fork(void)
{
    int index;
    pthread_mutex_init(&timer_lock, 0);
    manager_channel = -1;
    manager_connection = -1;
    for (index = 0; index < TIMERFD_SLOTS; ++index)
        slots[index].has_timer = 0;
}

/* Re-arms an inherited timerfd in this process and counts what is due
   (exec restore). */
static void adopt_armed(int slot)
{
    int failed = 0, error = 0;
    pthread_mutex_lock(&timer_lock);
    if (slots[slot].shared->armed) {
        if (rearm_from_page(slot) != 0) {
            failed = 1;
            error = errno;
        }
        catch_up(slots[slot].shared, slots[slot].write_fd);
    }
    pthread_mutex_unlock(&timer_lock);
    if (failed) {
        /* Reads still count from the clock; only poll/epoll readiness
           without a read lacks its wake-up. */
        uint32_t args[6] = { 0, 0, 0, 0, 0, 0 };
        args[0] = (uint32_t)slot;
        (void)linux_report_unsupported(LINUX_NR_EXECVE,
            linux_errno_number(error), "timerfd-exec-rearm", args);
    }
}

/* Exec: the record "shm:channel:" names the page and the channel write
   end; fd is the channel read end, which survived exec. */
static int timerfd_restore(int fd, int hidden_fd, int second_hidden_fd,
    const char *host_path)
{
    void *page;
    int slot;
    (void)host_path;
    if (hidden_fd < 0 || second_hidden_fd < 0) return -1;
    page = eventfd_map_page(hidden_fd);
    if (page == 0) return -1;
    slot = attach_object(fd, second_hidden_fd, hidden_fd, page);
    if (slot < 0) return -1;
    adopt_armed(slot);
    return 0;
}

/* A timerfd received from another process: fd is the channel read end,
   the companions the channel write end and the shm page.  The receiver
   doesn't fire it: the arming process does (documented limit), and the
   receiver's reads count from the clock. */
static int timerfd_receive(int fd, const int *companions, int count)
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
    return 0;
}

void linux_timerfd_register(void)
{
    guest_fd_register_restore(GUEST_FD_TIMERFD, timerfd_restore);
    guest_fd_register_receive(GUEST_FD_TIMERFD, timerfd_receive);
}
