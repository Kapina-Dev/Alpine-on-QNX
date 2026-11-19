/*
 * epoll modes: EPOLLET, EPOLLONESHOT, EPOLLRDHUP, EPOLLEXCLUSIVE,
 * EPOLLWAKEUP, and EPOLLHUP at pipe EOF (guest fixture, musl).
 *
 * Checks (the exit code is the check number, also printed on stderr with
 * errno; a check that hangs is ended by the watchdog thread with exit code
 * 100 + the check number):
 *   10 setup failed
 *   11 EPOLLET pipe: a write gives one event; a second wait(0) gives none;
 *      reading part of the data re-arms it (reported again); after reading
 *      to EAGAIN nothing is reported until the next write, which is
 *   12 EPOLLONESHOT: one event, then none while data stays; the item is
 *      still registered (ADD fails EEXIST); MOD re-arms it and the next
 *      wait reports, once; DEL succeeds
 *   13 EPOLLRDHUP on a TCP loopback pair: nothing before; after the peer's
 *      shutdown(SHUT_WR), EPOLLIN|EPOLLRDHUP (no EPOLLHUP) where EPOLLRDHUP
 *      was requested, and EPOLLIN alone where it wasn't
 *   14 the same over an AF_UNIX stream socketpair
 *   15 EPOLLEXCLUSIVE: ADD with EPOLLIN succeeds and reports; MOD with it
 *      fails EINVAL, and so does a MOD without it of the exclusive item;
 *      ADD with EPOLLONESHOT or EPOLLRDHUP beside it fails EINVAL
 *   16 EPOLLWAKEUP is accepted on ADD and MOD and never reported
 *   17 EPOLLET eventfd (libuv's async pattern): write 1 -> event; read ->
 *      no event; write 1 -> event; with the item reported and unread, a
 *      write from this thread wakes another thread blocked in epoll_wait
 *   18 pipe EOF: after the writer closes and the data is read, epoll with
 *      EPOLLIN reports EPOLLHUP (EPOLLIN clear: the pipe is empty, as
 *      Linux), and so does poll() (POLLHUP); with EPOLLET once only
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile int current_check;
static volatile int watchdog_ticks;     /* 100 ms ticks since arm() */

static int fail(int check, const char *what)
{
    fprintf(stderr, "epoll-modes check %d failed: %s (errno %d %s)\n",
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
            fprintf(stderr, "epoll-modes check %d timed out\n",
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

static int ctl(int ep, int op, int fd, uint32_t events, uint64_t data)
{
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = events;
    event.data.u64 = data;
    errno = 0;
    return epoll_ctl(ep, op, fd, &event);
}

static int ctl_error(int ep, int op, int fd, uint32_t events)
{
    return ctl(ep, op, fd, events, 0) == -1 ? errno : 0;
}

/* One wait: the count, and the first event in *event. */
static int wait_one(int ep, int timeout, struct epoll_event *event)
{
    struct epoll_event events[4];
    int count;
    memset(events, 0, sizeof(events));
    do count = epoll_wait(ep, events, 4, timeout);
    while (count < 0 && errno == EINTR);
    if (count > 0) *event = events[0];
    return count;
}

/* The event mask of a single reported event, or -1 when the wait doesn't
   report exactly one event with the expected data. */
static long one_event(int ep, int timeout, uint64_t data)
{
    struct epoll_event event;
    int count = wait_one(ep, timeout, &event);
    if (count != 1 || event.data.u64 != data) return -1;
    return (long)event.events;
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int check_et_pipe(void)
{
    int ep = epoll_create1(0), p[2];
    char buffer[16];
    long events;
    ssize_t got;
    if (ep < 0 || pipe(p) != 0 || set_nonblock(p[0]) != 0)
        return fail(10, "epoll_create1/pipe");
    if (ctl(ep, EPOLL_CTL_ADD, p[0], EPOLLIN | EPOLLET, 11) != 0)
        return fail(11, "ADD EPOLLIN|EPOLLET");
    if (wait_one(ep, 0, &(struct epoll_event){0}) != 0)
        return fail(11, "event before any write");
    if (write(p[1], "abcdefgh", 8) != 8) return fail(10, "write");
    events = one_event(ep, 0, 11);
    if (events != EPOLLIN) return fail(11, "write did not give one EPOLLIN");
    if (wait_one(ep, 0, &(struct epoll_event){0}) != 0)
        return fail(11, "second wait reported again (not edge-triggered)");
    if (wait_one(ep, 50, &(struct epoll_event){0}) != 0)
        return fail(11, "wait(50) reported again (not edge-triggered)");
    if (read(p[0], buffer, 3) != 3) return fail(10, "partial read");
    if (one_event(ep, 0, 11) != EPOLLIN)
        return fail(11, "partial read did not re-arm");
    do got = read(p[0], buffer, sizeof(buffer));
    while (got > 0);
    if (got != -1 || errno != EAGAIN) return fail(10, "read to EAGAIN");
    if (wait_one(ep, 0, &(struct epoll_event){0}) != 0)
        return fail(11, "event with the pipe empty");
    if (write(p[1], "z", 1) != 1) return fail(10, "second write");
    if (one_event(ep, 0, 11) != EPOLLIN)
        return fail(11, "write after EAGAIN not reported");
    if (wait_one(ep, 0, &(struct epoll_event){0}) != 0)
        return fail(11, "write after EAGAIN reported twice");
    close(ep); close(p[0]); close(p[1]);
    return 0;
}

static int check_oneshot(void)
{
    int ep = epoll_create1(0), p[2], e;
    if (ep < 0 || pipe(p) != 0) return fail(10, "epoll_create1/pipe");
    if (ctl(ep, EPOLL_CTL_ADD, p[0], EPOLLIN | EPOLLONESHOT, 12) != 0)
        return fail(12, "ADD EPOLLIN|EPOLLONESHOT");
    if (write(p[1], "ab", 2) != 2) return fail(10, "write");
    if (one_event(ep, 0, 12) != EPOLLIN) return fail(12, "first event");
    if (wait_one(ep, 0, &(struct epoll_event){0}) != 0)
        return fail(12, "reported again after one shot");
    if (wait_one(ep, 50, &(struct epoll_event){0}) != 0)
        return fail(12, "wait(50) reported after one shot");
    if ((e = ctl_error(ep, EPOLL_CTL_ADD, p[0], EPOLLIN)) != EEXIST)
        { errno = e; return fail(12, "disabled item gone (ADD not EEXIST)"); }
    if (ctl(ep, EPOLL_CTL_MOD, p[0], EPOLLIN | EPOLLONESHOT, 120) != 0)
        return fail(12, "MOD re-arm");
    if (one_event(ep, 0, 120) != EPOLLIN)
        return fail(12, "MOD did not re-arm");
    if (wait_one(ep, 0, &(struct epoll_event){0}) != 0)
        return fail(12, "reported again after the re-armed shot");
    if (epoll_ctl(ep, EPOLL_CTL_DEL, p[0], 0) != 0)
        return fail(12, "DEL of a disabled item");
    close(ep); close(p[0]); close(p[1]);
    return 0;
}

/* a and b are a connected stream pair; b shuts down its writing side. */
static int check_rdhup(int check, int a, int b)
{
    int with = epoll_create1(0), without = epoll_create1(0);
    long events;
    if (with < 0 || without < 0) return fail(10, "epoll_create1");
    if (ctl(with, EPOLL_CTL_ADD, a, EPOLLIN | EPOLLRDHUP, 1) != 0 ||
            ctl(without, EPOLL_CTL_ADD, a, EPOLLIN, 2) != 0)
        return fail(check, "ADD");
    if (wait_one(with, 0, &(struct epoll_event){0}) != 0 ||
            wait_one(without, 0, &(struct epoll_event){0}) != 0)
        return fail(check, "event before the shutdown");
    if (shutdown(b, SHUT_WR) != 0) return fail(10, "shutdown(SHUT_WR)");
    events = one_event(with, 2000, 1);
    if (events < 0) return fail(check, "no event after the shutdown");
    if (events != (EPOLLIN | EPOLLRDHUP)) {
        fprintf(stderr, "epoll-modes check %d: events 0x%lx\n", check,
            events);
        errno = 0;
        return fail(check, "not EPOLLIN|EPOLLRDHUP after SHUT_WR");
    }
    /* level-triggered: reported again */
    if (one_event(with, 0, 1) != (EPOLLIN | EPOLLRDHUP))
        return fail(check, "EPOLLRDHUP not level-triggered");
    events = one_event(without, 0, 2);
    if (events != EPOLLIN) {
        fprintf(stderr, "epoll-modes check %d: events 0x%lx\n", check,
            events);
        errno = 0;
        return fail(check, "unrequested EPOLLRDHUP (or no EPOLLIN)");
    }
    close(with); close(without);
    return 0;
}

static int check_rdhup_tcp(void)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int listener = socket(AF_INET, SOCK_STREAM, 0), client, accepted, result;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener < 0 ||
            bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
            listen(listener, 4) != 0 ||
            getsockname(listener, (struct sockaddr *)&address, &length))
        return fail(10, "TCP listener");
    client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0 ||
            connect(client, (struct sockaddr *)&address, sizeof(address)))
        return fail(10, "TCP connect");
    accepted = accept(listener, 0, 0);
    if (accepted < 0) return fail(10, "accept");
    result = check_rdhup(13, accepted, client);
    close(listener); close(client); close(accepted);
    return result;
}

static int check_rdhup_unix(void)
{
    int pair[2], result;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        return fail(10, "socketpair");
    result = check_rdhup(14, pair[0], pair[1]);
    close(pair[0]); close(pair[1]);
    return result;
}

static int check_exclusive(void)
{
    int ep = epoll_create1(0), p[2], q[2], e;
    if (ep < 0 || pipe(p) != 0 || pipe(q) != 0)
        return fail(10, "epoll_create1/pipe");
    if (ctl(ep, EPOLL_CTL_ADD, p[0], EPOLLIN | EPOLLEXCLUSIVE, 15) != 0)
        return fail(15, "ADD EPOLLIN|EPOLLEXCLUSIVE");
    if (write(p[1], "a", 1) != 1) return fail(10, "write");
    if (one_event(ep, 0, 15) != EPOLLIN)
        return fail(15, "exclusive item not reported");
    if ((e = ctl_error(ep, EPOLL_CTL_MOD, p[0],
            EPOLLIN | EPOLLEXCLUSIVE)) != EINVAL)
        { errno = e; return fail(15, "MOD with EPOLLEXCLUSIVE not EINVAL"); }
    if ((e = ctl_error(ep, EPOLL_CTL_MOD, p[0], EPOLLIN)) != EINVAL)
        { errno = e; return fail(15, "MOD of an exclusive item not EINVAL"); }
    if ((e = ctl_error(ep, EPOLL_CTL_ADD, q[0],
            EPOLLIN | EPOLLEXCLUSIVE | EPOLLONESHOT)) != EINVAL)
        { errno = e; return fail(15, "EPOLLEXCLUSIVE|EPOLLONESHOT not EINVAL"); }
    if ((e = ctl_error(ep, EPOLL_CTL_ADD, q[0],
            EPOLLIN | EPOLLEXCLUSIVE | EPOLLRDHUP)) != EINVAL)
        { errno = e; return fail(15, "EPOLLEXCLUSIVE|EPOLLRDHUP not EINVAL"); }
    if (ctl(ep, EPOLL_CTL_ADD, q[0], EPOLLIN | EPOLLOUT | EPOLLET |
            EPOLLEXCLUSIVE, 150) != 0)
        return fail(15, "EPOLLEXCLUSIVE with EPOLLOUT|EPOLLET refused");
    if (epoll_ctl(ep, EPOLL_CTL_DEL, p[0], 0) != 0)
        return fail(15, "DEL of an exclusive item");
    close(ep); close(p[0]); close(p[1]); close(q[0]); close(q[1]);
    return 0;
}

static int check_wakeup(void)
{
    int ep = epoll_create1(0), p[2];
    if (ep < 0 || pipe(p) != 0) return fail(10, "epoll_create1/pipe");
    if (ctl(ep, EPOLL_CTL_ADD, p[0], EPOLLIN | EPOLLWAKEUP, 16) != 0)
        return fail(16, "ADD EPOLLIN|EPOLLWAKEUP");
    if (write(p[1], "a", 1) != 1) return fail(10, "write");
    if (one_event(ep, 0, 16) != EPOLLIN)
        return fail(16, "not exactly EPOLLIN");
    if (ctl(ep, EPOLL_CTL_MOD, p[0], EPOLLIN | EPOLLWAKEUP, 160) != 0)
        return fail(16, "MOD EPOLLIN|EPOLLWAKEUP");
    if (one_event(ep, 0, 160) != EPOLLIN)
        return fail(16, "not exactly EPOLLIN after MOD");
    close(ep); close(p[0]); close(p[1]);
    return 0;
}

struct blocked_wait {
    int ep;
    int count;
    uint64_t data;
    long long elapsed;
};

static void *blocked_wait_main(void *argument)
{
    struct blocked_wait *wait = argument;
    struct epoll_event event;
    long long start = now_ms();
    memset(&event, 0, sizeof(event));
    wait->count = wait_one(wait->ep, 4000, &event);
    wait->data = event.data.u64;
    wait->elapsed = now_ms() - start;
    return 0;
}

static int check_et_eventfd(void)
{
    int ep = epoll_create1(0), efd = eventfd(0, EFD_NONBLOCK);
    uint64_t one = 1, value;
    struct blocked_wait wait;
    pthread_t thread;
    if (ep < 0 || efd < 0) return fail(10, "epoll_create1/eventfd");
    if (ctl(ep, EPOLL_CTL_ADD, efd, EPOLLIN | EPOLLET, 17) != 0)
        return fail(17, "ADD eventfd EPOLLIN|EPOLLET");
    if (write(efd, &one, 8) != 8) return fail(10, "eventfd write");
    if (one_event(ep, 0, 17) != EPOLLIN) return fail(17, "first write");
    if (wait_one(ep, 0, &(struct epoll_event){0}) != 0)
        return fail(17, "reported twice for one write");
    if (read(efd, &value, 8) != 8 || value != 1)
        return fail(10, "eventfd read");
    if (wait_one(ep, 0, &(struct epoll_event){0}) != 0)
        return fail(17, "event after the read");
    if (write(efd, &one, 8) != 8) return fail(10, "eventfd write 2");
    if (one_event(ep, 0, 17) != EPOLLIN) return fail(17, "second write");
    /* Reported and not read: another thread blocks, this one writes. */
    if (wait_one(ep, 0, &(struct epoll_event){0}) != 0)
        return fail(17, "reported twice for the second write");
    memset(&wait, 0, sizeof(wait));
    wait.ep = ep;
    if (pthread_create(&thread, 0, blocked_wait_main, &wait) != 0)
        return fail(10, "pthread_create");
    sleep_ms(150);
    if (write(efd, &one, 8) != 8) return fail(10, "eventfd write 3");
    pthread_join(thread, 0);
    if (wait.count != 1 || wait.data != 17 || wait.elapsed >= 3000) {
        fprintf(stderr, "epoll-modes check 17: count %d after %lld ms\n",
            wait.count, wait.elapsed);
        errno = 0;
        return fail(17, "a write did not wake the blocked wait");
    }
    close(ep); close(efd);
    return 0;
}

static int check_pipe_eof(void)
{
    int level = epoll_create1(0), edge = epoll_create1(0), p[2];
    char buffer[4];
    struct pollfd entry;
    long events;
    if (level < 0 || edge < 0 || pipe(p) != 0)
        return fail(10, "epoll_create1/pipe");
    if (ctl(level, EPOLL_CTL_ADD, p[0], EPOLLIN, 18) != 0 ||
            ctl(edge, EPOLL_CTL_ADD, p[0], EPOLLIN | EPOLLET, 180) != 0)
        return fail(18, "ADD");
    if (write(p[1], "ab", 2) != 2) return fail(10, "write");
    close(p[1]);
    if (read(p[0], buffer, sizeof(buffer)) != 2) return fail(10, "read");
    events = one_event(level, 0, 18);
    if (events != EPOLLHUP) {
        fprintf(stderr, "epoll-modes check 18: events 0x%lx\n", events);
        errno = 0;
        return fail(18, "epoll at pipe EOF not EPOLLHUP");
    }
    if (one_event(level, 0, 18) != EPOLLHUP)
        return fail(18, "EPOLLHUP at pipe EOF not level-triggered");
    entry.fd = p[0];
    entry.events = POLLIN;
    entry.revents = 0;
    if (poll(&entry, 1, 0) != 1 || entry.revents != POLLHUP) {
        fprintf(stderr, "epoll-modes check 18: revents 0x%x\n",
            entry.revents);
        errno = 0;
        return fail(18, "poll at pipe EOF not POLLHUP");
    }
    if (one_event(edge, 0, 180) != EPOLLHUP)
        return fail(18, "EPOLLET at pipe EOF not EPOLLHUP");
    if (wait_one(edge, 100, &(struct epoll_event){0}) != 0)
        return fail(18, "EPOLLET reported pipe EOF twice");
    close(level); close(edge); close(p[0]);
    return 0;
}

int main(void)
{
    int result;
    arm(11);
    if ((result = check_et_pipe()) != 0) return result;
    arm(12);
    if ((result = check_oneshot()) != 0) return result;
    arm(13);
    if ((result = check_rdhup_tcp()) != 0) return result;
    arm(14);
    if ((result = check_rdhup_unix()) != 0) return result;
    arm(15);
    if ((result = check_exclusive()) != 0) return result;
    arm(16);
    if ((result = check_wakeup()) != 0) return result;
    arm(17);
    if ((result = check_et_eventfd()) != 0) return result;
    arm(18);
    if ((result = check_pipe_eof()) != 0) return result;
    printf("epoll-modes: all checks passed\n");
    return 0;
}
