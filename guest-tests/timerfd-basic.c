/*
 * The Linux timerfd: create, settime, gettime, read, readiness and
 * TFD_IOC_SET_TICKS (guest fixture, musl).
 *
 * The calls go through syscall() with explicit layouts, so 353/354 (the
 * 32-bit itimerspec) and 410/411 (the time64 one) are each tested; musl's
 * own wrappers try the time64 calls first.  Every timerfd here is
 * TFD_NONBLOCK: blocking reads are tested separately.
 *
 * Checks (the exit code is the check number, also printed on stderr with
 * errno; a check that hangs is ended by the watchdog thread with exit code
 * 100 + the check number):
 *   10 setup failed
 *   11 timerfd_create succeeds for clocks 0, 1, 7, 8, 9; clocks 2 and 99
 *      fail EINVAL; flag bit 0x4 fails EINVAL; TFD_CLOEXEC sets FD_CLOEXEC
 *      (and its absence leaves it clear)
 *   12 one-shot relative 50 ms: a read before fails EAGAIN; after 80 ms a
 *      read returns 8 bytes holding 1; the next read fails EAGAIN
 *   13 absolute CLOCK_MONOTONIC deadline now + 40 ms (TFD_TIMER_ABSTIME)
 *      fires once
 *   14 periodic 10 ms: after sleeping 105 ms one read returns 10 or 11
 *      (more only when the measured sleep overran: the count must match
 *      the measured time)
 *   15 gettime on a 1 s timer after 200 ms reports 0.75-0.81 s left and
 *      the interval; a disarmed timer reports zeros
 *   16 re-arming resets the count and old_value returns the previous
 *      setting; disarming with {0, 0} stops it firing
 *   17 a read with a 7-byte buffer fails EINVAL
 *   18 poll, select and epoll report readability only after expiry, and
 *      a poll and an epoll_wait sleeping on an armed timer wake on expiry
 *   19 ioctl(TFD_IOC_SET_TICKS, &5): the next read returns 5
 *   20 periodic 1000 ns: after 100 ms the count is >= 10000 and the
 *      process's user + system CPU grew < 20 ms over the wait
 *   21 tv_nsec 1000000000 fails EINVAL; negative seconds fail EINVAL;
 *      settime flag 0x4 fails EINVAL; settime on a pipe fails EINVAL; an
 *      absolute CLOCK_REALTIME deadline of 2040-01-01 (time64) is armed,
 *      gettime reports > 0, and nothing fires within 50 ms
 *   22 the time64 calls (410/411) behave as 353/354 for checks 12 and 15
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define NR_TIMERFD_CREATE 350
#define NR_TIMERFD_SETTIME 353
#define NR_TIMERFD_GETTIME 354
#define NR_TIMERFD_GETTIME64 410
#define NR_TIMERFD_SETTIME64 411
#define TFD_NONBLOCK_FLAG 0x800
#define TFD_CLOEXEC_FLAG 0x80000
#define TFD_ABSTIME 1
#define TFD_SET_TICKS 0x40085400u
#define MS 1000000LL

struct spec32 { int32_t value_sec, value_nsec, interval_sec, interval_nsec; };
struct spec64 { int64_t value_sec, value_nsec, interval_sec, interval_nsec; };

static volatile int current_check;
static volatile int watchdog_ticks;     /* 100 ms ticks since arm() */

static int fail(int check, const char *what)
{
    fprintf(stderr, "timerfd-basic check %d failed: %s (errno %d %s)\n",
        check, what, errno, strerror(errno));
    return check;
}

static void sleep_ms(int milliseconds)
{
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

static int64_t now_ns(clockid_t clock)
{
    struct timespec now;
    clock_gettime(clock, &now);
    return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

/* Each check arms the watchdog for itself: BerryCore timeout never
   fires on the device. */
static void *watchdog(void *argument)
{
    (void)argument;
    for (;;) {
        sleep_ms(100);
        if (__sync_add_and_fetch(&watchdog_ticks, 1) >= 100) {
            fprintf(stderr, "timerfd-basic check %d timed out\n",
                current_check);
            _exit(100 + current_check);
        }
    }
    return 0;
}

/* Starts the check's 10 s bound. */
static void arm(int check)
{
    static int started;
    current_check = check;
    watchdog_ticks = 0;
    __sync_synchronize();
    if (!started) {
        pthread_t thread;
        if (pthread_create(&thread, 0, watchdog, 0) == 0) started = 1;
    }
}

static int tfd_create(int clock, int flags)
{
    return (int)syscall(NR_TIMERFD_CREATE, clock, flags);
}

static int settime32(int fd, int flags, int64_t value, int64_t interval,
    struct spec32 *old)
{
    struct spec32 spec;
    spec.value_sec = (int32_t)(value / 1000000000LL);
    spec.value_nsec = (int32_t)(value % 1000000000LL);
    spec.interval_sec = (int32_t)(interval / 1000000000LL);
    spec.interval_nsec = (int32_t)(interval % 1000000000LL);
    return (int)syscall(NR_TIMERFD_SETTIME, fd, flags, &spec, old);
}

static int settime64(int fd, int flags, int64_t value, int64_t interval,
    struct spec64 *old)
{
    struct spec64 spec;
    spec.value_sec = value / 1000000000LL;
    spec.value_nsec = value % 1000000000LL;
    spec.interval_sec = interval / 1000000000LL;
    spec.interval_nsec = interval % 1000000000LL;
    return (int)syscall(NR_TIMERFD_SETTIME64, fd, flags, &spec, old);
}

static int gettime32(int fd, int64_t *value, int64_t *interval)
{
    struct spec32 spec;
    memset(&spec, 0x55, sizeof(spec));
    if (syscall(NR_TIMERFD_GETTIME, fd, &spec) != 0) return -1;
    *value = spec.value_sec * 1000000000LL + spec.value_nsec;
    *interval = spec.interval_sec * 1000000000LL + spec.interval_nsec;
    return 0;
}

static int gettime64(int fd, int64_t *value, int64_t *interval)
{
    struct spec64 spec;
    memset(&spec, 0x55, sizeof(spec));
    if (syscall(NR_TIMERFD_GETTIME64, fd, &spec) != 0) return -1;
    *value = spec.value_sec * 1000000000LL + spec.value_nsec;
    *interval = spec.interval_sec * 1000000000LL + spec.interval_nsec;
    return 0;
}

static int read_count(int fd, uint64_t *count)
{
    return read(fd, count, sizeof(*count)) == (ssize_t)sizeof(*count);
}

static int read_eagain(int fd)
{
    uint64_t count;
    errno = 0;
    return read(fd, &count, sizeof(count)) == -1 && errno == EAGAIN;
}

static int check_create(void)
{
    static const int clocks[] = { 0, 1, 7, 8, 9 };
    unsigned i;
    int fd;
    for (i = 0; i < sizeof(clocks) / sizeof(clocks[0]); ++i) {
        fd = tfd_create(clocks[i], TFD_NONBLOCK_FLAG);
        if (fd < 0) {
            fprintf(stderr, "clock %d\n", clocks[i]);
            return fail(11, "timerfd_create of a supported clock");
        }
        if ((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0)
            return fail(11, "FD_CLOEXEC without TFD_CLOEXEC");
        close(fd);
    }
    errno = 0;
    if (tfd_create(2, 0) != -1 || errno != EINVAL)
        return fail(11, "clock 2 not EINVAL");
    errno = 0;
    if (tfd_create(99, 0) != -1 || errno != EINVAL)
        return fail(11, "clock 99 not EINVAL");
    errno = 0;
    if (tfd_create(1, 0x4) != -1 || errno != EINVAL)
        return fail(11, "flag 0x4 not EINVAL");
    fd = tfd_create(1, TFD_CLOEXEC_FLAG | TFD_NONBLOCK_FLAG);
    if (fd < 0) return fail(11, "TFD_CLOEXEC create");
    if ((fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0)
        return fail(11, "TFD_CLOEXEC did not set FD_CLOEXEC");
    if ((fcntl(fd, F_GETFL) & O_NONBLOCK) == 0)
        return fail(11, "TFD_NONBLOCK not in F_GETFL");
    close(fd);
    return 0;
}

/* Check 12, or 22 with the time64 calls. */
static int check_oneshot(int check, int time64)
{
    uint64_t count = 0;
    int fd = tfd_create(1, TFD_NONBLOCK_FLAG), result;
    if (fd < 0) return fail(10, "timerfd_create");
    result = time64 ? settime64(fd, 0, 50 * MS, 0, 0) :
        settime32(fd, 0, 50 * MS, 0, 0);
    if (result != 0) return fail(check, "settime one-shot 50 ms");
    if (!read_eagain(fd)) return fail(check, "read before expiry not EAGAIN");
    sleep_ms(80);
    if (!read_count(fd, &count) || count != 1) {
        fprintf(stderr, "count %llu\n", (unsigned long long)count);
        return fail(check, "read after 80 ms not 1");
    }
    if (!read_eagain(fd)) return fail(check, "second read not EAGAIN");
    sleep_ms(60);
    if (!read_eagain(fd)) return fail(check, "one-shot fired again");
    close(fd);
    return 0;
}

static int check_absolute(void)
{
    uint64_t count = 0;
    int fd = tfd_create(1, TFD_NONBLOCK_FLAG);
    if (fd < 0) return fail(10, "timerfd_create");
    if (settime32(fd, TFD_ABSTIME, now_ns(CLOCK_MONOTONIC) + 40 * MS, 0,
            0) != 0)
        return fail(13, "settime absolute");
    if (!read_eagain(fd)) return fail(13, "read before deadline not EAGAIN");
    sleep_ms(70);
    if (!read_count(fd, &count) || count != 1)
        return fail(13, "absolute deadline did not fire once");
    sleep_ms(50);
    if (!read_eagain(fd)) return fail(13, "absolute one-shot fired again");
    close(fd);
    return 0;
}

static int check_periodic(void)
{
    uint64_t count = 0, most;
    int64_t start, elapsed;
    int fd = tfd_create(1, TFD_NONBLOCK_FLAG);
    if (fd < 0) return fail(10, "timerfd_create");
    start = now_ns(CLOCK_MONOTONIC);
    if (settime32(fd, 0, 10 * MS, 10 * MS, 0) != 0)
        return fail(14, "settime periodic 10 ms");
    sleep_ms(105);
    if (!read_count(fd, &count)) return fail(14, "read of periodic count");
    elapsed = now_ns(CLOCK_MONOTONIC) - start;
    /* 10 or 11 for a sleep of 105-109 ms; a longer measured overrun allows
       the expirations it really covered. */
    most = (uint64_t)(elapsed / (10 * MS));
    if (most < 11) most = 11;
    if (count < 10 || count > most) {
        fprintf(stderr, "count %llu elapsed_us %lld\n",
            (unsigned long long)count, (long long)(elapsed / 1000));
        return fail(14, "periodic count not 10 or 11");
    }
    close(fd);
    return 0;
}

/* Check 15, or 22 with the time64 calls. */
static int check_gettime(int check, int time64)
{
    int64_t value = -1, interval = -1;
    int fd = tfd_create(1, TFD_NONBLOCK_FLAG), result;
    if (fd < 0) return fail(10, "timerfd_create");
    result = time64 ? settime64(fd, 0, 1000 * MS, 300 * MS, 0) :
        settime32(fd, 0, 1000 * MS, 300 * MS, 0);
    if (result != 0) return fail(check, "settime 1 s");
    sleep_ms(200);
    result = time64 ? gettime64(fd, &value, &interval) :
        gettime32(fd, &value, &interval);
    if (result != 0) return fail(check, "gettime");
    if (value < 750 * MS || value > 810 * MS || interval != 300 * MS) {
        fprintf(stderr, "value_us %lld interval_us %lld\n",
            (long long)(value / 1000), (long long)(interval / 1000));
        return fail(check, "gettime not 0.75-0.81 s left with the interval");
    }
    result = time64 ? settime64(fd, 0, 0, 0, 0) : settime32(fd, 0, 0, 0, 0);
    if (result != 0) return fail(check, "disarm");
    result = time64 ? gettime64(fd, &value, &interval) :
        gettime32(fd, &value, &interval);
    if (result != 0 || value != 0 || interval != 0)
        return fail(check, "disarmed timer not zeros");
    close(fd);
    return 0;
}

static int check_rearm(void)
{
    struct spec32 old;
    int64_t old_value, old_interval;
    int fd = tfd_create(1, TFD_NONBLOCK_FLAG);
    if (fd < 0) return fail(10, "timerfd_create");
    if (settime32(fd, 0, 10 * MS, 20 * MS, 0) != 0)
        return fail(16, "settime periodic");
    sleep_ms(45);
    memset(&old, 0x55, sizeof(old));
    if (settime32(fd, 0, 1000 * MS, 0, &old) != 0)
        return fail(16, "re-arm settime");
    old_value = old.value_sec * 1000000000LL + old.value_nsec;
    old_interval = old.interval_sec * 1000000000LL + old.interval_nsec;
    if (old_interval != 20 * MS || old_value <= 0 || old_value > 20 * MS) {
        fprintf(stderr, "old value_us %lld interval_us %lld\n",
            (long long)(old_value / 1000), (long long)(old_interval / 1000));
        return fail(16, "old_value not the previous setting");
    }
    if (!read_eagain(fd)) return fail(16, "re-arm did not reset the count");
    /* Disarm a timer about to fire. */
    if (settime32(fd, 0, 20 * MS, 0, 0) != 0)
        return fail(16, "settime 20 ms");
    memset(&old, 0x55, sizeof(old));
    if (settime32(fd, 0, 0, 0, &old) != 0) return fail(16, "disarm");
    old_value = old.value_sec * 1000000000LL + old.value_nsec;
    if (old_value <= 0 || old_value > 20 * MS || old.interval_sec != 0 ||
            old.interval_nsec != 0)
        return fail(16, "disarm old_value not the 20 ms setting");
    sleep_ms(60);
    if (!read_eagain(fd)) return fail(16, "disarmed timer fired");
    close(fd);
    return 0;
}

static int check_short_read(void)
{
    char buffer[7];
    int fd = tfd_create(1, TFD_NONBLOCK_FLAG);
    if (fd < 0) return fail(10, "timerfd_create");
    if (settime32(fd, 0, 1 * MS, 0, 0) != 0) return fail(17, "settime");
    sleep_ms(20);
    errno = 0;
    if (read(fd, buffer, sizeof(buffer)) != -1 || errno != EINVAL)
        return fail(17, "7-byte read not EINVAL");
    close(fd);
    return 0;
}

static int select_readable(int fd)
{
    fd_set set;
    struct timeval zero = { 0, 0 };
    FD_ZERO(&set);
    FD_SET(fd, &set);
    if (select(fd + 1, &set, 0, 0, &zero) < 0) return -1;
    return FD_ISSET(fd, &set) ? 1 : 0;
}

static int check_readiness(void)
{
    struct pollfd entry;
    struct epoll_event event, got;
    int64_t start;
    uint64_t count;
    int fd = tfd_create(1, TFD_NONBLOCK_FLAG), ep = epoll_create1(0);
    if (fd < 0 || ep < 0) return fail(10, "timerfd_create or epoll_create1");
    event.events = EPOLLIN;
    event.data.u64 = 77;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event) != 0)
        return fail(18, "epoll_ctl ADD");
    if (settime32(fd, 0, 40 * MS, 0, 0) != 0) return fail(18, "settime");
    entry.fd = fd;
    entry.events = POLLIN | POLLOUT;
    entry.revents = 0;
    if (poll(&entry, 1, 0) != 0) return fail(18, "poll ready before expiry");
    if (select_readable(fd) != 0) return fail(18, "select before expiry");
    if (epoll_wait(ep, &got, 1, 0) != 0)
        return fail(18, "epoll_wait ready before expiry");
    sleep_ms(70);
    entry.revents = 0;
    /* Linuxemu's poll also reports an unrequested POLLRDNORM for every
       readable fd (a separate, older quirk), so only POLLIN and the
       absence of POLLOUT are checked. */
    if (poll(&entry, 1, 0) != 1 ||
            (entry.revents & (POLLIN | POLLOUT)) != POLLIN) {
        fprintf(stderr, "revents 0x%x\n", entry.revents);
        return fail(18, "poll not POLLIN without POLLOUT after expiry");
    }
    if (select_readable(fd) != 1) return fail(18, "select after expiry");
    if (epoll_wait(ep, &got, 1, 0) != 1 || got.events != EPOLLIN ||
            got.data.u64 != 77)
        return fail(18, "epoll_wait after expiry");
    if (!read_count(fd, &count) || count != 1) return fail(18, "read");
    /* Sleepers woken by the expiry itself. */
    if (settime32(fd, 0, 30 * MS, 0, 0) != 0) return fail(18, "re-arm");
    start = now_ns(CLOCK_MONOTONIC);
    entry.events = POLLIN;
    entry.revents = 0;
    if (poll(&entry, 1, 3000) != 1 || (entry.revents & POLLIN) == 0)
        return fail(18, "blocking poll not woken by expiry");
    if (now_ns(CLOCK_MONOTONIC) - start > 1000 * MS)
        return fail(18, "blocking poll woke late");
    if (!read_count(fd, &count) || count != 1) return fail(18, "read 2");
    if (settime32(fd, 0, 30 * MS, 0, 0) != 0) return fail(18, "re-arm 2");
    start = now_ns(CLOCK_MONOTONIC);
    if (epoll_wait(ep, &got, 1, 3000) != 1 || got.events != EPOLLIN)
        return fail(18, "blocking epoll_wait not woken by expiry");
    if (now_ns(CLOCK_MONOTONIC) - start > 1000 * MS)
        return fail(18, "blocking epoll_wait woke late");
    if (!read_count(fd, &count) || count != 1) return fail(18, "read 3");
    close(ep);
    close(fd);
    return 0;
}

static int check_set_ticks(void)
{
    uint64_t ticks = 5, count = 0;
    struct pollfd entry;
    int fd = tfd_create(1, TFD_NONBLOCK_FLAG);
    if (fd < 0) return fail(10, "timerfd_create");
    if (ioctl(fd, TFD_SET_TICKS, &ticks) != 0)
        return fail(19, "ioctl TFD_IOC_SET_TICKS");
    entry.fd = fd;
    entry.events = POLLIN;
    entry.revents = 0;
    if (poll(&entry, 1, 0) != 1) return fail(19, "not readable after ticks");
    if (!read_count(fd, &count) || count != 5) return fail(19, "read not 5");
    if (!read_eagain(fd)) return fail(19, "second read not EAGAIN");
    ticks = 0;
    errno = 0;
    if (ioctl(fd, TFD_SET_TICKS, &ticks) != -1 || errno != EINVAL)
        return fail(19, "ticks 0 not EINVAL");
    close(fd);
    return 0;
}

static int64_t cpu_ns(void)
{
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) return -1;
    return ((int64_t)usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) *
        1000000000LL + ((int64_t)usage.ru_utime.tv_usec +
        usage.ru_stime.tv_usec) * 1000LL;
}

static int check_fast_period(void)
{
    uint64_t count = 0;
    int64_t before, after;
    int fd = tfd_create(1, TFD_NONBLOCK_FLAG);
    if (fd < 0) return fail(10, "timerfd_create");
    before = cpu_ns();
    if (before < 0) return fail(10, "getrusage");
    if (settime32(fd, 0, 1000, 1000, 0) != 0)
        return fail(20, "settime 1000 ns period");
    sleep_ms(100);
    after = cpu_ns();
    if (!read_count(fd, &count) || count < 10000) {
        fprintf(stderr, "count %llu\n", (unsigned long long)count);
        return fail(20, "count after 100 ms under 10000");
    }
    if (after - before >= 20 * MS) {
        fprintf(stderr, "cpu_us %lld\n", (long long)((after - before) / 1000));
        return fail(20, "the wait used 20 ms of CPU or more");
    }
    close(fd);
    return 0;
}

static int check_validation(void)
{
    struct spec32 bad;
    int64_t value = 0, interval = 0;
    int pipe_fds[2];
    int fd = tfd_create(0, TFD_NONBLOCK_FLAG);
    if (fd < 0) return fail(10, "timerfd_create");
    memset(&bad, 0, sizeof(bad));
    bad.value_sec = 1;
    bad.value_nsec = 1000000000;
    errno = 0;
    if (syscall(NR_TIMERFD_SETTIME, fd, 0, &bad, 0) != -1 || errno != EINVAL)
        return fail(21, "tv_nsec 1000000000 not EINVAL");
    memset(&bad, 0, sizeof(bad));
    bad.value_sec = 1;
    bad.interval_nsec = 1000000000;
    errno = 0;
    if (syscall(NR_TIMERFD_SETTIME, fd, 0, &bad, 0) != -1 || errno != EINVAL)
        return fail(21, "interval tv_nsec 1000000000 not EINVAL");
    memset(&bad, 0, sizeof(bad));
    bad.value_sec = -1;
    errno = 0;
    if (syscall(NR_TIMERFD_SETTIME, fd, 0, &bad, 0) != -1 || errno != EINVAL)
        return fail(21, "negative seconds not EINVAL");
    errno = 0;
    if (settime32(fd, 0x4, 10 * MS, 0, 0) != -1 || errno != EINVAL)
        return fail(21, "settime flag 0x4 not EINVAL");
    if (pipe(pipe_fds) != 0) return fail(10, "pipe");
    errno = 0;
    if (settime32(pipe_fds[0], 0, 10 * MS, 0, 0) != -1 || errno != EINVAL)
        return fail(21, "settime on a pipe not EINVAL");
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    /* 2040-01-01T00:00:00Z. */
    if (settime64(fd, TFD_ABSTIME, 2208988800LL * 1000000000LL, 0, 0) != 0)
        return fail(21, "settime64 absolute 2040-01-01");
    if (gettime64(fd, &value, &interval) != 0 || value <= 0)
        return fail(21, "gettime of the 2040 deadline not > 0");
    sleep_ms(50);
    if (!read_eagain(fd)) return fail(21, "2040 deadline fired");
    close(fd);
    return 0;
}

int main(void)
{
    int result;
    arm(11);
    if ((result = check_create()) != 0) return result;
    arm(12);
    if ((result = check_oneshot(12, 0)) != 0) return result;
    arm(13);
    if ((result = check_absolute()) != 0) return result;
    arm(14);
    if ((result = check_periodic()) != 0) return result;
    arm(15);
    if ((result = check_gettime(15, 0)) != 0) return result;
    arm(16);
    if ((result = check_rearm()) != 0) return result;
    arm(17);
    if ((result = check_short_read()) != 0) return result;
    arm(18);
    if ((result = check_readiness()) != 0) return result;
    arm(19);
    if ((result = check_set_ticks()) != 0) return result;
    arm(20);
    if ((result = check_fast_period()) != 0) return result;
    arm(21);
    if ((result = check_validation()) != 0) return result;
    arm(22);
    if ((result = check_oneshot(22, 1)) != 0) return result;
    if ((result = check_gettime(22, 1)) != 0) return result;
    return 0;
}
