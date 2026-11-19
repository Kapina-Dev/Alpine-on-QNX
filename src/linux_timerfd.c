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
 * host-clock nanoseconds) and how many expirations of this arming are
 * already in the count.
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
 * Locking.  timer_lock (process-local) guards the slot table, the manager
 * start and every read or write of a page's spec and counted fields.  It
 * is a leaf: nothing else is taken under it (the counter helpers are
 * lock-free and only touch the pipe).
 *
 * Clocks.  CLOCK_REALTIME and CLOCK_MONOTONIC map directly;
 * CLOCK_BOOTTIME and CLOCK_BOOTTIME_ALARM map to CLOCK_MONOTONIC (whether
 * QNX's monotonic clock stops in device sleep is unmeasured:
 * monotonic_in_sleep=unknown), CLOCK_REALTIME_ALARM to CLOCK_REALTIME.
 * The *_ALARM clocks fire while the device is awake and cannot wake it.
 *
 * Not yet (Phase 3 part 2, Task 3): blocking reads (an empty blocking read
 * returns EAGAIN with a diagnostic), TFD_TIMER_CANCEL_ON_SET (EINVAL with a
 * diagnostic), exec carry-over and SCM_RIGHTS rebuilding.  After fork the
 * child forgets the parent's QNX timers and manager (see
 * linux_timerfd_after_fork).
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
#include <sys/neutrino.h>
#include <sys/siginfo.h>
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
#define LINUX_NR_READ 3
#define LINUX_NR_TIMERFD_SETTIME 353

#define NS_PER_SECOND 1000000000LL
/* Requested times saturate here (about 146 years), so now + value and
   the expiration arithmetic never overflow. */
#define TIMERFD_NS_MAX ((int64_t)1 << 62)
/* QNX delivers at most one pulse per 1 ms tick anyway; a shorter host
   period only costs CPU.  The count comes from the clock regardless. */
#define TIMERFD_HOST_INTERVAL_MIN 1000000LL
/* Slot index in the low 16 bits of a pulse value, generation above. */
#define TIMERFD_SLOTS 1024
#define TIMERFD_PULSE_CODE (_PULSE_CODE_MINAVAIL + 3)

struct timerfd_shared {
    struct eventfd_shared event;    /* first: the 8-byte aligned counter */
    int32_t linux_clock;
    int32_t host_clock;
    uint32_t armed;                 /* it_value was non-zero */
    uint32_t reserved;
    int64_t first_deadline;         /* host-clock ns of expiration 1 */
    int64_t interval;               /* ns; 0 for a one-shot timer */
    uint64_t counted;               /* expirations of this arming added */
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

/* Expirations of the current arming due at now (lock held). */
static uint64_t due_total(const struct timerfd_shared *shared, int64_t now)
{
    if (!shared->armed || now < shared->first_deadline) return 0;
    if (shared->interval == 0) return 1;
    return (uint64_t)((now - shared->first_deadline) / shared->interval) + 1;
}

/* Adds the expirations due now that are not yet counted (lock held). */
static void catch_up(struct timerfd_shared *shared, int write_fd)
{
    uint64_t total;
    if (!shared->armed) return;
    total = due_total(shared, clock_now(shared->host_clock));
    if (total <= shared->counted) return;
    /* A count of 0xfffffffffffffffe expirations can't be reached (584000
       years at 1 ns), so a refused add is not handled. */
    (void)eventfd_counter_add(&shared->event, write_fd,
        total - shared->counted);
    shared->counted = total;
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

static void on_pulse(uint32_t value)
{
    uint32_t index = value & 0xffffu;
    struct timer_slot *slot;
    if (index >= TIMERFD_SLOTS) return;
    pthread_mutex_lock(&timer_lock);
    slot = &slots[index];
    if (slot->in_use && slot->generation == (uint16_t)(value >> 16)) {
        struct timerfd_shared *shared = slot->shared;
        catch_up(shared, slot->write_fd);
        /* A one-shot pulse that came before the deadline (QNX rounds to
           ticks, so it shouldn't) would leave a waiter asleep: re-arm. */
        if (shared->armed && shared->interval == 0 && shared->counted == 0
                && slot->has_timer) {
            int64_t left = shared->first_deadline -
                clock_now(shared->host_clock);
            if (left > 0) (void)host_arm(slot, left, 0);
        }
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

static int32_t timerfd_read(struct guest_fd_object *object, int fd,
    void *buffer, size_t length)
{
    struct timerfd_data *data = guest_fd_object_data(object);
    uint64_t value;
    if (length < sizeof(value)) return -(int32_t)linux_errno_number(EINVAL);
    pthread_mutex_lock(&timer_lock);
    catch_up(data->shared, data->write_fd);
    pthread_mutex_unlock(&timer_lock);
    value = eventfd_counter_take(&data->shared->event, data->write_fd, fd);
    if (value == 0) {
        uint32_t args[6] = { (uint32_t)fd, 0, (uint32_t)length, 0, 0, 0 };
        if (data->shared->event.nonblock)
            return -(int32_t)linux_errno_number(EAGAIN);
        /* Blocking reads come with Phase 3 part 2, Task 3. */
        return linux_report_unsupported(LINUX_NR_READ,
            linux_errno_number(EAGAIN), "timerfd-blocking-read", args);
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

/* POLLRDNORM (Linux POLLIN) while the count is above 0 or an expiration
   is due that no pulse has counted yet; never writable, as on Linux. */
static short timerfd_ready_now(struct guest_fd_object *object, int fd)
{
    struct timerfd_data *data = guest_fd_object_data(object);
    struct timerfd_shared *shared = data->shared;
    int due;
    (void)fd;
    if (eventfd_counter_load(&shared->event) != 0) return POLLRDNORM;
    pthread_mutex_lock(&timer_lock);
    due = shared->armed &&
        due_total(shared, clock_now(shared->host_clock)) > shared->counted;
    pthread_mutex_unlock(&timer_lock);
    return due ? POLLRDNORM : 0;
}

static const struct guest_fd_ops timerfd_ops = {
    timerfd_read, timerfd_write, timerfd_poll_fd, timerfd_get_status,
    timerfd_set_status, timerfd_release, 0, timerfd_ready_now, 0
};

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
    struct guest_fd_object *object;
    struct timerfd_data *data;
    struct timerfd_shared *shared;
    void *page;
    int fds[3], host_clock, slot, saved_errno;

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
    slot = slot_take(shared, fds[1]);
    if (slot < 0) goto fail_unmap;
    object = guest_fd_object_new(GUEST_FD_TIMERFD, &timerfd_ops, fds[0],
        sizeof(struct timerfd_data));
    if (object == 0) {
        saved_errno = errno;
        slot_release(slot);
        errno = saved_errno;
        goto fail_unmap;
    }
    data = guest_fd_object_data(object);
    data->shared = shared;
    data->write_fd = fds[1];
    data->shm_fd = fds[2];
    data->slot = slot;
    if (guest_fd_attach(fds[0], object) != 0) {
        saved_errno = errno;
        /* The put releases the slot, the mapping and the hidden fds. */
        guest_fd_put(object);
        close(fds[0]);
        return -(int32_t)linux_errno_number(saved_errno);
    }
    guest_fd_put(object);
    return fds[0];
fail_unmap:
    saved_errno = errno;
    munmap(page, EVENTFD_PAGE);
    close(fds[0]);
    close(fds[1]);
    close(fds[2]);
    return -(int32_t)linux_errno_number(saved_errno);
}

/* The guest itimerspec: two {sec, nsec} pairs, 32- or 64-bit fields. */
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
            nanoseconds = fields[1];
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

    if ((flags & ~(LINUX_TFD_TIMER_ABSTIME | LINUX_TFD_TIMER_CANCEL_ON_SET))
            != 0)
        return -(int32_t)linux_errno_number(EINVAL);
    result = load_spec(new_spec, time64, requested);
    if (result != 0) return result;
    object = acquire_timerfd(fd, &result);
    if (object == 0) return result;
    if ((flags & LINUX_TFD_TIMER_CANCEL_ON_SET) != 0) {
        uint32_t args[6] = { (uint32_t)fd, flags, 0, 0, 0, 0 };
        guest_fd_put(object);
        /* Phase 3 part 2, Task 3. */
        return linux_report_unsupported(LINUX_NR_TIMERFD_SETTIME,
            linux_errno_number(EINVAL), "timerfd-cancel-on-set", args);
    }
    data = guest_fd_object_data(object);
    shared = data->shared;
    slot = &slots[data->slot];
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
        shared->interval = requested[1];
        shared->counted = 0;
        shared->armed = requested[0] != 0;
        if (shared->armed) {
            shared->first_deadline = (flags & LINUX_TFD_TIMER_ABSTIME) != 0 ?
                requested[0] : now + requested[0];
            delay = shared->first_deadline - now;
        }
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
    if (spec == 0) return -EFAULT;
    object = acquire_timerfd(fd, &result);
    if (object == 0) return result;
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
    if (data->shared->armed)
        data->shared->counted = due_total(data->shared,
            clock_now(data->shared->host_clock));
    eventfd_counter_set(&data->shared->event, data->write_fd, fd, ticks);
    pthread_mutex_unlock(&timer_lock);
    guest_fd_put(object);
    return 0;
}

/* The fork child has neither the manager thread nor the QNX timers.  Its
   timerfds keep their pages (the count stays shared, as on Linux) and get
   a child-side timer and manager when the child arms them. */
void linux_timerfd_after_fork(void)
{
    int index;
    pthread_mutex_init(&timer_lock, 0);
    manager_channel = -1;
    manager_connection = -1;
    for (index = 0; index < TIMERFD_SLOTS; ++index)
        slots[index].has_timer = 0;
}
