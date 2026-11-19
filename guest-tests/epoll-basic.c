/*
 * epoll create, ctl and level-triggered wait (guest fixture, musl).
 *
 * Checks (the exit code is the check number, also printed on stderr with
 * errno; a check that hangs is ended by the watchdog thread with exit code
 * 100 + the check number):
 *   10 setup failed
 *   11 epoll_create1(0) and epoll_create(1) succeed; EPOLL_CLOEXEC sets
 *      FD_CLOEXEC; epoll_create1(1) and epoll_create(0) fail EINVAL
 *   12 epoll_ctl errors: bad epfd EBADF; epfd not an epoll EINVAL;
 *      fd == epfd EINVAL; bad op EINVAL; regular file and directory EPERM;
 *      second ADD EEXIST; MOD/DEL of an unregistered fd ENOENT; an fd at or
 *      above the soft RLIMIT_NOFILE (Linuxemu's hidden range) EBADF
 *   13 pipe: no data -> wait(0) returns 0; after a write, 1 with EPOLLIN
 *      and the data 0x1122334455667788; reported again (level)
 *   14 MOD of the write end to EPOLLOUT reports EPOLLOUT; after DEL, 0
 *   15 eventfd: no event, then EPOLLIN after a write; EPOLLOUT requested
 *      is reported at once
 *   16 a socketpair whose peer is closed reports EPOLLHUP and EPOLLIN with
 *      only EPOLLIN requested
 *   17 epoll_wait(..., 50) with nothing ready returns 0 after >= 50 ms
 *   18 maxevents 0 and -1 fail EINVAL; with 3 ready fds and maxevents 2,
 *      two waits together report all three (rotation)
 *   19 epoll_pwait2 with {0, 30000000} returns 0 after >= 30 ms
 *   20 epoll_pwait in thread B with SIGUSR1 blocked by its mask: a SIGUSR1
 *      sent to B during the wait doesn't end it (returns 0 at its
 *      timeout); the handler runs once afterwards
 *   21 a closed fd's registration is gone: wait(0) returns 0
 *   22 soft RLIMIT_NOFILE - 20 pipe ends registered (at least 700): data
 *      in the middle pipe gives exactly that pipe's data from one wait
 *   23 events NULL or an unmapped address fails EFAULT when an event is
 *      ready; the event is still reported afterwards
 *   24 while thread B waits in epoll_pwait with a mask, pipe() in thread A
 *      returns the same numbers as before B started waiting (the emulator's
 *      signal-wait pipe takes no guest fd numbers)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define NR_EPOLL_CREATE 250
#define NR_EPOLL_WAIT 252
#define NR_EPOLL_PWAIT2 441

typedef char epoll_event_is_16_bytes[sizeof(struct epoll_event) == 16 ? 1 : -1];

static volatile int current_check;
static volatile int watchdog_ticks;     /* 100 ms ticks since arm() */

static int fail(int check, const char *what)
{
    fprintf(stderr, "epoll-basic check %d failed: %s (errno %d %s)\n",
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

static long long now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* BerryCore timeout never fires on the device, so each check arms a 10 s
   bound for itself: a hang ends the run with exit 100 + check. */
static void *watchdog(void *argument)
{
    (void)argument;
    for (;;) {
        sleep_ms(100);
        if (__sync_add_and_fetch(&watchdog_ticks, 1) >= 100) {
            fprintf(stderr, "epoll-basic check %d timed out\n",
                current_check);
            _exit(100 + current_check);
        }
    }
    return 0;
}

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

static int add(int ep, int fd, uint32_t events, uint64_t data)
{
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = events;
    event.data.u64 = data;
    return epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event);
}

static int ctl_error(int ep, int op, int fd, uint32_t events)
{
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = events;
    errno = 0;
    return epoll_ctl(ep, op, fd, &event) == -1 ? errno : 0;
}

static int check_create(void)
{
    int a, b, c;
    errno = 0;
    a = epoll_create1(0);
    if (a < 0) return fail(11, "epoll_create1(0)");
    b = (int)syscall(NR_EPOLL_CREATE, 1);
    if (b < 0) return fail(11, "epoll_create(1)");
    c = epoll_create1(EPOLL_CLOEXEC);
    if (c < 0) return fail(11, "epoll_create1(EPOLL_CLOEXEC)");
    if ((fcntl(c, F_GETFD) & FD_CLOEXEC) == 0)
        return fail(11, "EPOLL_CLOEXEC did not set FD_CLOEXEC");
    if ((fcntl(a, F_GETFD) & FD_CLOEXEC) != 0)
        return fail(11, "epoll_create1(0) set FD_CLOEXEC");
    errno = 0;
    if (epoll_create1(1) != -1 || errno != EINVAL)
        return fail(11, "epoll_create1(1) did not fail EINVAL");
    errno = 0;
    if (syscall(NR_EPOLL_CREATE, 0) != -1 || errno != EINVAL)
        return fail(11, "epoll_create(0) did not fail EINVAL");
    close(a); close(b); close(c);
    return 0;
}

static int check_ctl_errors(void)
{
    int ep = epoll_create1(0), p[2], q[2], closed[2], file, directory, e;
    struct rlimit limit;
    if (ep < 0 || pipe(p) != 0 || pipe(q) != 0 || pipe(closed) != 0)
        return fail(10, "epoll_create1/pipe");
    close(closed[0]); close(closed[1]);
    if ((e = ctl_error(closed[0], EPOLL_CTL_ADD, p[0], EPOLLIN)) != EBADF)
        { errno = e; return fail(12, "bad epfd not EBADF"); }
    if ((e = ctl_error(ep, EPOLL_CTL_ADD, closed[0], EPOLLIN)) != EBADF)
        { errno = e; return fail(12, "bad fd not EBADF"); }
    if ((e = ctl_error(q[0], EPOLL_CTL_ADD, p[0], EPOLLIN)) != EINVAL)
        { errno = e; return fail(12, "epfd not an epoll not EINVAL"); }
    if ((e = ctl_error(ep, EPOLL_CTL_ADD, ep, EPOLLIN)) != EINVAL)
        { errno = e; return fail(12, "fd == epfd not EINVAL"); }
    if ((e = ctl_error(ep, 99, p[0], EPOLLIN)) != EINVAL)
        { errno = e; return fail(12, "bad op not EINVAL"); }
    file = open("/tmp/epoll-basic-regular", O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (file < 0) return fail(10, "open regular file");
    unlink("/tmp/epoll-basic-regular");
    if ((e = ctl_error(ep, EPOLL_CTL_ADD, file, EPOLLIN)) != EPERM)
        { errno = e; return fail(12, "regular file not EPERM"); }
    directory = open("/", O_RDONLY | O_DIRECTORY);
    if (directory < 0) return fail(10, "open /");
    if ((e = ctl_error(ep, EPOLL_CTL_ADD, directory, EPOLLIN)) != EPERM)
        { errno = e; return fail(12, "directory not EPERM"); }
    if (add(ep, p[0], EPOLLIN, 1) != 0) return fail(12, "first ADD");
    if ((e = ctl_error(ep, EPOLL_CTL_ADD, p[0], EPOLLIN)) != EEXIST)
        { errno = e; return fail(12, "second ADD not EEXIST"); }
    if ((e = ctl_error(ep, EPOLL_CTL_MOD, q[0], EPOLLIN)) != ENOENT)
        { errno = e; return fail(12, "MOD unregistered not ENOENT"); }
    if ((e = ctl_error(ep, EPOLL_CTL_DEL, q[0], EPOLLIN)) != ENOENT)
        { errno = e; return fail(12, "DEL unregistered not ENOENT"); }
    if (epoll_ctl(ep, EPOLL_CTL_DEL, p[0], 0) != 0)
        return fail(12, "DEL registered");
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) return fail(10, "getrlimit");
    if (limit.rlim_cur < 4096) {
        int hidden = (int)limit.rlim_cur, i;
        for (i = 0; i != 4; ++i)
            if ((e = ctl_error(ep, EPOLL_CTL_ADD, hidden + i, EPOLLIN)) !=
                    EBADF) {
                errno = e;
                return fail(12, "fd at or above RLIMIT_NOFILE not EBADF");
            }
    }
    close(file); close(directory);
    close(p[0]); close(p[1]); close(q[0]); close(q[1]); close(ep);
    return 0;
}

static int check_pipe_level(void)
{
    int ep = epoll_create1(0), p[2];
    struct epoll_event events[4];
    if (ep < 0 || pipe(p) != 0) return fail(10, "epoll_create1/pipe");
    if (add(ep, p[0], EPOLLIN, 0x1122334455667788ULL) != 0)
        return fail(13, "ADD");
    if (epoll_wait(ep, events, 4, 0) != 0) return fail(13, "empty wait not 0");
    if (write(p[1], "x", 1) != 1) return fail(10, "write");
    memset(events, 0, sizeof(events));
    if (syscall(NR_EPOLL_WAIT, ep, events, 4, 0) != 1)
        return fail(13, "epoll_wait after a write not 1");
    if (events[0].events != EPOLLIN)
        { errno = 0; return fail(13, "events not exactly EPOLLIN"); }
    if (events[0].data.u64 != 0x1122334455667788ULL)
        { errno = 0; return fail(13, "data not intact"); }
    memset(events, 0, sizeof(events));
    if (epoll_wait(ep, events, 4, 0) != 1 || events[0].events != EPOLLIN ||
            events[0].data.u64 != 0x1122334455667788ULL)
        return fail(13, "not reported again (level)");
    close(p[0]); close(p[1]); close(ep);
    return 0;
}

static int check_mod_del(void)
{
    int ep = epoll_create1(0), p[2];
    struct epoll_event events[4], change;
    if (ep < 0 || pipe(p) != 0) return fail(10, "epoll_create1/pipe");
    if (add(ep, p[1], EPOLLIN, 7) != 0) return fail(14, "ADD write end");
    if (epoll_wait(ep, events, 4, 0) != 0)
        return fail(14, "write end reported for EPOLLIN");
    memset(&change, 0, sizeof(change));
    change.events = EPOLLOUT;
    change.data.u64 = 8;
    if (epoll_ctl(ep, EPOLL_CTL_MOD, p[1], &change) != 0)
        return fail(14, "MOD to EPOLLOUT");
    memset(events, 0, sizeof(events));
    if (epoll_wait(ep, events, 4, 0) != 1 || events[0].events != EPOLLOUT ||
            events[0].data.u64 != 8)
        return fail(14, "EPOLLOUT not reported after MOD");
    if (epoll_ctl(ep, EPOLL_CTL_DEL, p[1], 0) != 0) return fail(14, "DEL");
    if (epoll_wait(ep, events, 4, 0) != 0)
        return fail(14, "reported after DEL");
    close(p[0]); close(p[1]); close(ep);
    return 0;
}

static int check_eventfd(void)
{
    int ep = epoll_create1(0), ep_out = epoll_create1(0);
    int e = eventfd(0, EFD_NONBLOCK);
    uint64_t value = 1;
    struct epoll_event events[4];
    if (ep < 0 || ep_out < 0 || e < 0) return fail(10, "epoll/eventfd");
    if (add(ep, e, EPOLLIN, 5) != 0) return fail(15, "ADD eventfd");
    if (epoll_wait(ep, events, 4, 0) != 0)
        return fail(15, "empty eventfd reported");
    if (write(e, &value, sizeof(value)) != sizeof(value))
        return fail(10, "eventfd write");
    memset(events, 0, sizeof(events));
    if (epoll_wait(ep, events, 4, 0) != 1 || events[0].events != EPOLLIN ||
            events[0].data.u64 != 5)
        return fail(15, "EPOLLIN not reported after a write");
    if (read(e, &value, sizeof(value)) != sizeof(value) || value != 1)
        return fail(10, "eventfd read");
    if (epoll_wait(ep, events, 4, 0) != 0)
        return fail(15, "reported after the read");
    if (add(ep_out, e, EPOLLOUT, 6) != 0) return fail(15, "ADD EPOLLOUT");
    memset(events, 0, sizeof(events));
    if (epoll_wait(ep_out, events, 4, -1) != 1 ||
            events[0].events != EPOLLOUT || events[0].data.u64 != 6)
        return fail(15, "EPOLLOUT not reported at once");
    close(e); close(ep); close(ep_out);
    return 0;
}

static int check_hangup(void)
{
    int ep = epoll_create1(0), pair[2];
    struct epoll_event events[4];
    if (ep < 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        return fail(10, "epoll_create1/socketpair");
    if (add(ep, pair[0], EPOLLIN, 9) != 0) return fail(16, "ADD");
    if (epoll_wait(ep, events, 4, 0) != 0)
        return fail(16, "open socketpair reported");
    close(pair[1]);
    memset(events, 0, sizeof(events));
    if (epoll_wait(ep, events, 4, 1000) != 1 || events[0].data.u64 != 9)
        return fail(16, "closed peer not reported");
    if ((events[0].events & EPOLLHUP) == 0) {
        fprintf(stderr, "epoll-basic check 16: events 0x%x\n",
            events[0].events);
        errno = 0;
        return fail(16, "no EPOLLHUP for a closed peer");
    }
    if ((events[0].events & EPOLLIN) == 0) {
        fprintf(stderr, "epoll-basic check 16: events 0x%x\n",
            events[0].events);
        errno = 0;
        return fail(16, "no EPOLLIN for a closed peer");
    }
    close(pair[0]); close(ep);
    return 0;
}

static int check_timeout(void)
{
    int ep = epoll_create1(0), p[2], result;
    struct epoll_event events[4];
    long long start, elapsed;
    if (ep < 0 || pipe(p) != 0) return fail(10, "epoll_create1/pipe");
    if (add(ep, p[0], EPOLLIN, 1) != 0) return fail(17, "ADD");
    start = now_ms();
    result = epoll_wait(ep, events, 4, 50);
    elapsed = now_ms() - start;
    if (result != 0) return fail(17, "timed wait not 0");
    if (elapsed < 50) {
        fprintf(stderr, "epoll-basic check 17: %lld ms\n", elapsed);
        errno = 0;
        return fail(17, "returned before 50 ms");
    }
    close(p[0]); close(p[1]); close(ep);
    return 0;
}

static int check_rotation(void)
{
    int ep = epoll_create1(0), p[3][2], i, seen = 0, n;
    struct epoll_event events[4];
    if (ep < 0) return fail(10, "epoll_create1");
    errno = 0;
    if (epoll_wait(ep, events, 0, 0) != -1 || errno != EINVAL)
        return fail(18, "maxevents 0 not EINVAL");
    errno = 0;
    if (epoll_wait(ep, events, -1, 0) != -1 || errno != EINVAL)
        return fail(18, "maxevents -1 not EINVAL");
    for (i = 0; i != 3; ++i) {
        if (pipe(p[i]) != 0) return fail(10, "pipe");
        if (add(ep, p[i][0], EPOLLIN, 1u << i) != 0) return fail(18, "ADD");
        if (write(p[i][1], "x", 1) != 1) return fail(10, "write");
    }
    for (n = 0; n != 2; ++n) {
        int count = epoll_wait(ep, events, 2, 0);
        if (count != 2) return fail(18, "wait with maxevents 2 not 2");
        for (i = 0; i != count; ++i) seen |= (int)events[i].data.u64;
    }
    if (seen != 7) {
        fprintf(stderr, "epoll-basic check 18: seen mask %d\n", seen);
        errno = 0;
        return fail(18, "two waits did not report all three");
    }
    for (i = 0; i != 3; ++i) { close(p[i][0]); close(p[i][1]); }
    close(ep);
    return 0;
}

static int check_pwait2(void)
{
    int ep = epoll_create1(0), p[2];
    struct epoll_event events[4];
    struct { int64_t seconds; int64_t nanoseconds; } timeout = { 0, 30000000 };
    long long start, elapsed;
    long result;
    if (ep < 0 || pipe(p) != 0) return fail(10, "epoll_create1/pipe");
    if (add(ep, p[0], EPOLLIN, 1) != 0) return fail(19, "ADD");
    start = now_ms();
    result = syscall(NR_EPOLL_PWAIT2, ep, events, 4, &timeout, 0, 8);
    elapsed = now_ms() - start;
    if (result != 0) return fail(19, "epoll_pwait2 not 0");
    if (elapsed < 30) {
        fprintf(stderr, "epoll-basic check 19: %lld ms\n", elapsed);
        errno = 0;
        return fail(19, "returned before 30 ms");
    }
    close(p[0]); close(p[1]); close(ep);
    return 0;
}

static volatile int handler_runs;

static void on_usr1(int signal_number)
{
    (void)signal_number;
    __sync_fetch_and_add(&handler_runs, 1);
}

struct masked_wait {
    int ep;
    int timeout;
    volatile pid_t tid;
    volatile int started;
    int result;
    int error;
    long long elapsed;
};

static void *masked_waiter(void *argument)
{
    struct masked_wait *wait = argument;
    struct epoll_event events[4];
    sigset_t mask;
    long long start;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    wait->tid = (pid_t)syscall(SYS_gettid);
    __sync_synchronize();
    wait->started = 1;
    start = now_ms();
    errno = 0;
    wait->result = epoll_pwait(wait->ep, events, 4, wait->timeout, &mask);
    wait->error = errno;
    wait->elapsed = now_ms() - start;
    return 0;
}

static int start_masked_wait(struct masked_wait *wait, int ep, int timeout,
    pthread_t *thread)
{
    memset(wait, 0, sizeof(*wait));
    wait->ep = ep;
    wait->timeout = timeout;
    if (pthread_create(thread, 0, masked_waiter, wait) != 0) return -1;
    while (!wait->started) sleep_ms(1);
    sleep_ms(100);
    return 0;
}

static int check_pwait_mask(void)
{
    int ep = epoll_create1(0), p[2];
    struct sigaction action;
    struct masked_wait wait;
    pthread_t thread;
    if (ep < 0 || pipe(p) != 0) return fail(10, "epoll_create1/pipe");
    if (add(ep, p[0], EPOLLIN, 1) != 0) return fail(20, "ADD");
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_usr1;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, 0) != 0) return fail(10, "sigaction");
    handler_runs = 0;
    if (start_masked_wait(&wait, ep, 400, &thread) != 0)
        return fail(10, "pthread_create");
    if (syscall(SYS_tgkill, getpid(), wait.tid, SIGUSR1) != 0)
        return fail(10, "tgkill");
    pthread_join(thread, 0);
    errno = wait.error;
    if (wait.result != 0) return fail(20, "masked SIGUSR1 ended the wait");
    if (wait.elapsed < 400) {
        fprintf(stderr, "epoll-basic check 20: %lld ms\n", wait.elapsed);
        return fail(20, "wait returned before its timeout");
    }
    if (handler_runs != 1) {
        fprintf(stderr, "epoll-basic check 20: handler ran %d times\n",
            handler_runs);
        errno = 0;
        return fail(20, "SIGUSR1 not delivered once after the wait");
    }
    close(p[0]); close(p[1]); close(ep);
    return 0;
}

static int check_closed(void)
{
    int ep = epoll_create1(0), p[2];
    struct epoll_event events[4];
    if (ep < 0 || pipe(p) != 0) return fail(10, "epoll_create1/pipe");
    if (add(ep, p[0], EPOLLIN, 1) != 0) return fail(21, "ADD");
    if (write(p[1], "x", 1) != 1) return fail(10, "write");
    close(p[0]);
    if (epoll_wait(ep, events, 4, 0) != 0)
        return fail(21, "closed fd still reported");
    close(p[1]); close(ep);
    return 0;
}

static int check_many(void)
{
    static int ends[4096];
    struct rlimit limit;
    struct epoll_event events[8];
    int ep = epoll_create1(0), count, pipes, i, middle, result;
    if (ep < 0 || getrlimit(RLIMIT_NOFILE, &limit) != 0)
        return fail(10, "epoll_create1/getrlimit");
    count = limit.rlim_cur > 4000 ? 4000 : (int)limit.rlim_cur - 20;
    if (count < 700) {
        fprintf(stderr, "epoll-basic check 22: soft limit %lu\n",
            (unsigned long)limit.rlim_cur);
        errno = 0;
        return fail(22, "fewer than 700 fds available");
    }
    pipes = count / 2;
    for (i = 0; i != pipes; ++i) {
        if (pipe(ends + 2 * i) != 0) return fail(22, "pipe");
        /* The read end watches EPOLLIN; the write end only the events
           that are always reported (EPOLLERR, EPOLLHUP). */
        if (add(ep, ends[2 * i], EPOLLIN, 0xabcd00000000ULL | (unsigned)i)
                != 0 ||
            add(ep, ends[2 * i + 1], 0, 0xdcba00000000ULL | (unsigned)i)
                != 0)
            return fail(22, "ADD");
    }
    middle = pipes / 2;
    if (write(ends[2 * middle + 1], "x", 1) != 1) return fail(10, "write");
    memset(events, 0, sizeof(events));
    result = epoll_wait(ep, events, 8, 1000);
    if (result != 1) {
        fprintf(stderr, "epoll-basic check 22: %d events\n", result);
        return fail(22, "not exactly one event");
    }
    if (events[0].data.u64 != (0xabcd00000000ULL | (unsigned)middle) ||
            events[0].events != EPOLLIN) {
        errno = 0;
        return fail(22, "wrong event");
    }
    for (i = 0; i != 2 * pipes; ++i) close(ends[i]);
    close(ep);
    return 0;
}

static int check_fault(void)
{
    int ep = epoll_create1(0), p[2];
    struct epoll_event events[4];
    void *unmapped;
    if (ep < 0 || pipe(p) != 0) return fail(10, "epoll_create1/pipe");
    if (add(ep, p[0], EPOLLIN, 3) != 0) return fail(23, "ADD");
    if (write(p[1], "x", 1) != 1) return fail(10, "write");
    unmapped = mmap(0, 4096, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (unmapped == MAP_FAILED || munmap(unmapped, 4096) != 0)
        return fail(10, "mmap/munmap");
    errno = 0;
    if (syscall(NR_EPOLL_WAIT, ep, 0, 4, 0) != -1 || errno != EFAULT)
        return fail(23, "events NULL not EFAULT");
    errno = 0;
    if (syscall(NR_EPOLL_WAIT, ep, unmapped, 4, 0) != -1 || errno != EFAULT)
        return fail(23, "unmapped events not EFAULT");
    memset(events, 0, sizeof(events));
    if (epoll_wait(ep, events, 4, 0) != 1 || events[0].data.u64 != 3)
        return fail(23, "event lost after EFAULT");
    close(p[0]); close(p[1]); close(ep);
    return 0;
}

static int check_fd_numbers(void)
{
    int ep = epoll_create1(0), idle[2], before[2], during[2];
    struct masked_wait wait;
    pthread_t thread;
    if (ep < 0 || pipe(idle) != 0) return fail(10, "epoll_create1/pipe");
    if (add(ep, idle[0], EPOLLIN, 1) != 0) return fail(24, "ADD");
    if (pipe(before) != 0) return fail(10, "pipe before");
    close(before[0]); close(before[1]);
    if (start_masked_wait(&wait, ep, 400, &thread) != 0)
        return fail(10, "pthread_create");
    if (pipe(during) != 0) return fail(10, "pipe during");
    if (during[0] != before[0] || during[1] != before[1]) {
        fprintf(stderr, "epoll-basic check 24: before %d,%d during %d,%d\n",
            before[0], before[1], during[0], during[1]);
        errno = 0;
        pthread_join(thread, 0);
        return fail(24, "the wait took guest fd numbers");
    }
    pthread_join(thread, 0);
    errno = wait.error;
    if (wait.result != 0) return fail(24, "masked wait failed");
    close(during[0]); close(during[1]);
    close(idle[0]); close(idle[1]); close(ep);
    return 0;
}

int main(void)
{
    int result;
    arm(11);
    if ((result = check_create()) != 0) return result;
    arm(12);
    if ((result = check_ctl_errors()) != 0) return result;
    arm(13);
    if ((result = check_pipe_level()) != 0) return result;
    arm(14);
    if ((result = check_mod_del()) != 0) return result;
    arm(15);
    if ((result = check_eventfd()) != 0) return result;
    arm(16);
    if ((result = check_hangup()) != 0) return result;
    arm(17);
    if ((result = check_timeout()) != 0) return result;
    arm(18);
    if ((result = check_rotation()) != 0) return result;
    arm(19);
    if ((result = check_pwait2()) != 0) return result;
    arm(20);
    if ((result = check_pwait_mask()) != 0) return result;
    arm(21);
    if ((result = check_closed()) != 0) return result;
    arm(22);
    if ((result = check_many()) != 0) return result;
    arm(23);
    if ((result = check_fault()) != 0) return result;
    arm(24);
    if ((result = check_fd_numbers()) != 0) return result;
    return 0;
}
