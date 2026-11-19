/*
 * The Android Looper pattern (guest fixture, musl): one level-triggered
 * epoll watching an eventfd wake, two pipes and a socketpair.  The main
 * thread loops epoll_wait(..., 100) like Looper::pollOnce; a waker thread
 * writes the eventfd 1000 times (and the socketpair now and then) and then
 * one "quit" byte; a churn thread keeps adding and removing the two pipes,
 * which always hold data.
 *
 * Checks (the exit code is the check number, also printed on stderr with
 * errno; the watchdog ends a hang with exit code 100 + the check number):
 *   10 setup failed
 *   11 every wake is observed: the eventfd counter reads sum to 1000
 *   12 no event names a pipe registration removed before that wait began
 *   13 the loop exits within 2 s of the final "quit" byte
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
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define WAKES 1000
#define TOKENS 200000

/* Event data: kind in the low byte, the registration token above it. */
#define KIND_WAKE 1
#define KIND_PIPE 2
#define KIND_SOCKET 3
#define KIND_QUIT 4

static volatile int current_check = 10;
static volatile int watchdog_ticks;
static volatile int stop;

static int ep, wake_fd, pipes[2][2], sockets[2], quit_pipe[2];
static volatile long long quit_written_ms;
/* removed_at[token]: the sequence number stamped after the DEL of that
   registration returned, 0 while it is registered. */
static volatile unsigned removed_at[TOKENS];
static volatile unsigned sequence;
static unsigned next_token = 1;
static volatile int churn_failed;

static int fail(int check, const char *what)
{
    fprintf(stderr, "looper-pattern check %d failed: %s (errno %d %s)\n",
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

/* BerryCore timeout never fires on the device: 30 s bounds the run. */
static void *watchdog(void *argument)
{
    (void)argument;
    for (;;) {
        sleep_ms(100);
        if (__sync_add_and_fetch(&watchdog_ticks, 1) >= 300) {
            fprintf(stderr, "looper-pattern check %d timed out\n",
                current_check);
            _exit(100 + current_check);
        }
    }
    return 0;
}

static int ctl(int op, int fd, uint32_t events, uint64_t data)
{
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = events;
    event.data.u64 = data;
    return epoll_ctl(ep, op, fd, &event);
}

static void *waker(void *argument)
{
    uint64_t one = 1;
    int i;
    (void)argument;
    for (i = 0; i != WAKES; ++i) {
        if (write(wake_fd, &one, sizeof(one)) != sizeof(one)) {
            fprintf(stderr, "looper-pattern: wake write %d (errno %d)\n", i,
                errno);
            _exit(10);
        }
        if (i % 100 == 50 && write(sockets[1], "s", 1) != 1) _exit(10);
        if (i % 7 == 0) sleep_ms(1);
    }
    sleep_ms(50);
    quit_written_ms = now_ms();
    __sync_synchronize();
    if (write(quit_pipe[1], "q", 1) != 1) _exit(10);
    return 0;
}

/* Adds and removes the two pipes over and over.  Each ADD is a new
   registration token; after each DEL returns, the token is stamped. */
static void *churn(void *argument)
{
    unsigned token[2] = { 0, 0 };
    int which = 0;
    (void)argument;
    while (!stop) {
        int fd = pipes[which][0];
        if (token[which] == 0) {
            if (next_token >= TOKENS) { sleep_ms(1); continue; }
            token[which] = next_token++;
            if (ctl(EPOLL_CTL_ADD, fd, EPOLLIN,
                    ((uint64_t)token[which] << 8) | KIND_PIPE) != 0) {
                churn_failed = errno;
                return 0;
            }
        } else {
            struct epoll_event ignored;
            if (epoll_ctl(ep, EPOLL_CTL_DEL, fd, &ignored) != 0) {
                churn_failed = errno;
                return 0;
            }
            removed_at[token[which]] = __sync_add_and_fetch(&sequence, 1);
            token[which] = 0;
        }
        which ^= 1;
        if ((next_token & 15) == 0) sleep_ms(1);
    }
    return 0;
}

int main(void)
{
    pthread_t watchdog_thread, waker_thread, churn_thread;
    struct epoll_event events[16];
    unsigned long long total = 0, loops = 0, pipe_events = 0;
    long long quit_seen_ms = 0;
    int stale_events = 0, quit = 0, i;
    signal(SIGPIPE, SIG_IGN);
    if (pthread_create(&watchdog_thread, 0, watchdog, 0) != 0)
        return fail(10, "watchdog thread");
    ep = epoll_create1(EPOLL_CLOEXEC);
    wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ep < 0 || wake_fd < 0 || pipe(pipes[0]) != 0 || pipe(pipes[1]) != 0 ||
            pipe(quit_pipe) != 0 ||
            socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        return fail(10, "epoll/eventfd/pipe/socketpair");
    if (write(pipes[0][1], "a", 1) != 1 || write(pipes[1][1], "b", 1) != 1)
        return fail(10, "pipe data");
    if (ctl(EPOLL_CTL_ADD, wake_fd, EPOLLIN, KIND_WAKE) != 0 ||
            ctl(EPOLL_CTL_ADD, sockets[0], EPOLLIN, KIND_SOCKET) != 0 ||
            ctl(EPOLL_CTL_ADD, quit_pipe[0], EPOLLIN, KIND_QUIT) != 0)
        return fail(10, "ADD");
    if (pthread_create(&churn_thread, 0, churn, 0) != 0 ||
            pthread_create(&waker_thread, 0, waker, 0) != 0)
        return fail(10, "threads");
    current_check = 11;
    while (!quit) {
        unsigned before = __sync_add_and_fetch(&sequence, 0);
        int count = epoll_wait(ep, events, 16, 100);
        ++loops;
        if (count < 0) {
            if (errno == EINTR) continue;
            return fail(11, "epoll_wait");
        }
        for (i = 0; i != count; ++i) {
            uint64_t data = events[i].data.u64, value;
            unsigned token = (unsigned)(data >> 8);
            char byte;
            switch ((int)(data & 0xff)) {
            case KIND_WAKE:
                if (read(wake_fd, &value, sizeof(value)) == sizeof(value))
                    total += value;
                else if (errno != EAGAIN) return fail(11, "eventfd read");
                break;
            case KIND_PIPE:
                ++pipe_events;
                if (token == 0 || token >= TOKENS) {
                    fprintf(stderr, "looper-pattern: bad token %u\n", token);
                    ++stale_events;
                } else if (removed_at[token] != 0 &&
                        removed_at[token] <= before) {
                    if (stale_events < 5)
                        fprintf(stderr, "looper-pattern: token %u removed "
                            "at %u, wait began at %u\n", token,
                            removed_at[token], before);
                    ++stale_events;
                }
                break;
            case KIND_SOCKET:
                if (read(sockets[0], &byte, 1) != 1)
                    return fail(10, "socket read");
                break;
            case KIND_QUIT:
                if (read(quit_pipe[0], &byte, 1) != 1 || byte != 'q')
                    return fail(10, "quit read");
                quit_seen_ms = now_ms();
                quit = 1;
                break;
            default:
                fprintf(stderr, "looper-pattern: unknown data %#llx\n",
                    (unsigned long long)data);
                ++stale_events;
            }
        }
        if (churn_failed) {
            errno = churn_failed;
            return fail(10, "churn ctl");
        }
    }
    stop = 1;
    pthread_join(waker_thread, 0);
    pthread_join(churn_thread, 0);
    {
        uint64_t value;
        if (read(wake_fd, &value, sizeof(value)) == sizeof(value))
            total += value;
    }
    fprintf(stderr, "looper-pattern: %llu loops, %llu pipe events, %u "
        "registrations, wake total %llu\n", loops, pipe_events,
        next_token - 1, total);
    if (total != WAKES) {
        errno = 0;
        return fail(11, "eventfd reads do not sum to 1000");
    }
    current_check = 12;
    if (stale_events != 0) {
        errno = 0;
        return fail(12, "an event named a removed registration");
    }
    current_check = 13;
    if (quit_seen_ms - quit_written_ms > 2000) {
        fprintf(stderr, "looper-pattern: quit seen after %lld ms\n",
            quit_seen_ms - quit_written_ms);
        errno = 0;
        return fail(13, "the loop did not exit within 2 s of quit");
    }
    fprintf(stderr, "looper-pattern: all checks passed\n");
    return 0;
}
