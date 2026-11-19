/*
 * epoll under concurrency: ctl and close while another thread waits
 * (guest fixture, musl).
 *
 * Checks (the exit code is the check number, also printed on stderr with
 * errno; a check that hangs is ended by the watchdog thread with exit code
 * 100 + the check number):
 *   10 setup failed
 *   16 two different pipes have different (st_dev, st_ino), and a new pipe
 *      on a reused fd number has a different one from the closed pipe
 *   11 thread B waits (timeout -1); thread A adds a ready pipe: B returns
 *      it within 200 ms
 *   12 B waits on a pipe's write end registered for EPOLLIN; A MODs it to
 *      EPOLLOUT: B returns EPOLLOUT with the new data
 *   13 a ready pipe that A DELs first is never reported: B's 100 ms wait
 *      returns 0
 *   14 B waits on pipes r and x; A closes r, a new pipe takes r's number
 *      and is written, then x is written: B reports only x, and the
 *      number is no longer registered (MOD gives ENOENT)
 *   15 as 14, but A dup2s a fresh pipe onto r's number instead of closing
 *   31 B waits on a listening socket and an idle pipe; A closes the pipe
 *      and a client connects; the accepted socket gets the closed pipe's
 *      number and data arrives on it: no event for the pipe's item is ever
 *      reported (200 rounds)
 *   33 a watched accepted socket s1 is closed and a new connection is
 *      accepted onto its number and sent data, while B waits (woken by a
 *      write to an idle pipe x, or by its timeout) or before B waits: no
 *      wait ever reports s1's item (60 rounds).  QNX gives every TCP socket
 *      the same (st_dev, st_ino), so identity alone can't catch this
 *   32 stress: 4 threads add, modify, delete and close 64 pipes for 5 s
 *      while 2 threads wait; no reported data belongs to a registration
 *      that was removed (DEL, MOD or close) before that wait began
 *
 * EPOLL_CONCURRENT_CHECK=N in the environment runs only check N.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef char epoll_event_is_16_bytes[sizeof(struct epoll_event) == 16 ? 1 : -1];

static volatile int current_check;
static volatile int watchdog_ticks;     /* 100 ms ticks since arm() */
static volatile int watchdog_limit = 100;

static int fail(int check, const char *what)
{
    fprintf(stderr, "epoll-concurrent check %d failed: %s (errno %d %s)\n",
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

/* BerryCore timeout never fires on the device, so each check arms its own
   bound (10 s, 30 s for the stress): a hang exits 100 + check. */
static void *watchdog(void *argument)
{
    (void)argument;
    for (;;) {
        sleep_ms(100);
        if (__sync_add_and_fetch(&watchdog_ticks, 1) >= watchdog_limit) {
            fprintf(stderr, "epoll-concurrent check %d timed out\n",
                current_check);
            _exit(100 + current_check);
        }
    }
    return 0;
}

static void arm(int check, int seconds)
{
    static int started;
    current_check = check;
    watchdog_limit = seconds * 10;
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
    return epoll_ctl(ep, op, fd, &event);
}

/* Thread B: one epoll_wait. */
struct waiter {
    pthread_t thread;
    int ep, timeout, result, error;
    struct epoll_event events[8];
    volatile int done;
};

static void *waiter_main(void *argument)
{
    struct waiter *w = argument;
    w->result = epoll_wait(w->ep, w->events, 8, w->timeout);
    w->error = errno;
    __sync_synchronize();
    w->done = 1;
    return 0;
}

static int start_wait(struct waiter *w, int ep, int timeout)
{
    memset(w, 0, sizeof(*w));
    w->ep = ep;
    w->timeout = timeout;
    return pthread_create(&w->thread, 0, waiter_main, w) == 0 ? 0 : -1;
}

/* 0 once B returned within limit_ms, else -1 (B is left blocked). */
static int join_wait(struct waiter *w, int limit_ms)
{
    long long end = now_ms() + limit_ms;
    while (!w->done) {
        if (now_ms() >= end) return -1;
        sleep_ms(2);
    }
    pthread_join(w->thread, 0);
    errno = w->error;
    return 0;
}

static int reported(const struct waiter *w, uint64_t data)
{
    int i;
    for (i = 0; i < w->result; ++i)
        if (w->events[i].data.u64 == data) return 1;
    return 0;
}

static int same_file(int a, int b)
{
    struct stat sa, sb;
    if (fstat(a, &sa) != 0 || fstat(b, &sb) != 0) return -1;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static int check_identity(void)
{
    int p[2], q[2], r[2], number;
    struct stat old_status, new_status;
    if (pipe(p) != 0 || pipe(q) != 0) return fail(10, "pipe");
    if (same_file(p[0], q[0]) != 0)
        { errno = 0; return fail(16, "two pipes share (st_dev, st_ino)"); }
    if (fstat(p[0], &old_status) != 0) return fail(10, "fstat");
    number = p[0];
    close(p[0]);
    if (pipe(r) != 0) return fail(10, "pipe");
    if (r[0] != number)
        { errno = 0; return fail(10, "new pipe did not reuse the number"); }
    if (fstat(r[0], &new_status) != 0) return fail(10, "fstat");
    if (old_status.st_dev == new_status.st_dev &&
            old_status.st_ino == new_status.st_ino)
        { errno = 0; return fail(16, "reused number: same identity"); }
    close(p[1]); close(q[0]); close(q[1]); close(r[0]); close(r[1]);
    return 0;
}

static int check_add_wakes(void)
{
    int ep = epoll_create1(0), p[2];
    struct waiter w;
    long long added;
    if (ep < 0 || pipe(p) != 0) return fail(10, "epoll_create1/pipe");
    if (write(p[1], "x", 1) != 1) return fail(10, "write");
    if (start_wait(&w, ep, -1) != 0) return fail(10, "pthread_create");
    sleep_ms(100);
    if (w.done) { errno = w.error; return fail(11, "B returned before ADD"); }
    added = now_ms();
    if (ctl(ep, EPOLL_CTL_ADD, p[0], EPOLLIN, 0x1111) != 0)
        return fail(11, "ADD");
    if (join_wait(&w, 2000) != 0)
        { errno = 0; return fail(11, "B slept through the ADD"); }
    if (now_ms() - added > 200)
        { errno = 0; return fail(11, "B returned later than 200 ms"); }
    if (w.result != 1 || w.events[0].data.u64 != 0x1111 ||
            w.events[0].events != EPOLLIN)
        return fail(11, "B did not report the added pipe");
    close(p[0]); close(p[1]); close(ep);
    return 0;
}

static int check_mod_wakes(void)
{
    int ep = epoll_create1(0), p[2];
    struct waiter w;
    if (ep < 0 || pipe(p) != 0) return fail(10, "epoll_create1/pipe");
    if (ctl(ep, EPOLL_CTL_ADD, p[1], EPOLLIN, 0x1201) != 0)
        return fail(12, "ADD write end");
    if (start_wait(&w, ep, -1) != 0) return fail(10, "pthread_create");
    sleep_ms(100);
    if (w.done) { errno = w.error; return fail(12, "B returned before MOD"); }
    if (ctl(ep, EPOLL_CTL_MOD, p[1], EPOLLOUT, 0x1202) != 0)
        return fail(12, "MOD");
    if (join_wait(&w, 2000) != 0)
        { errno = 0; return fail(12, "B slept through the MOD"); }
    if (w.result != 1 || w.events[0].events != EPOLLOUT ||
            w.events[0].data.u64 != 0x1202)
        return fail(12, "B did not report EPOLLOUT with the new data");
    close(p[0]); close(p[1]); close(ep);
    return 0;
}

static int check_del_first(void)
{
    int ep = epoll_create1(0), p[2];
    struct waiter w;
    if (ep < 0 || pipe(p) != 0) return fail(10, "epoll_create1/pipe");
    if (write(p[1], "x", 1) != 1) return fail(10, "write");
    if (ctl(ep, EPOLL_CTL_ADD, p[0], EPOLLIN, 0x1301) != 0)
        return fail(13, "ADD");
    if (epoll_ctl(ep, EPOLL_CTL_DEL, p[0], 0) != 0) return fail(13, "DEL");
    if (start_wait(&w, ep, 100) != 0) return fail(10, "pthread_create");
    if (join_wait(&w, 2000) != 0)
        { errno = 0; return fail(13, "B's 100 ms wait did not return"); }
    if (w.result != 0) return fail(13, "a deleted pipe was reported");
    close(p[0]); close(p[1]); close(ep);
    return 0;
}

/* 14 (close and a new pipe on the number) and 15 (dup2 onto it). */
static int check_replaced(int check, int use_dup2)
{
    int ep = epoll_create1(0), r[2], x[2], fresh[2], number;
    struct waiter w;
    if (ep < 0 || pipe(r) != 0 || pipe(x) != 0)
        return fail(10, "epoll_create1/pipe");
    number = r[0];
    if (ctl(ep, EPOLL_CTL_ADD, r[0], EPOLLIN, 0xdead) != 0 ||
            ctl(ep, EPOLL_CTL_ADD, x[0], EPOLLIN, 0x0f0f) != 0)
        return fail(check, "ADD");
    if (start_wait(&w, ep, -1) != 0) return fail(10, "pthread_create");
    sleep_ms(100);
    if (use_dup2) {
        if (pipe(fresh) != 0) return fail(10, "pipe");
        if (dup2(fresh[0], number) != number) return fail(check, "dup2");
        close(fresh[0]);
    } else {
        close(r[0]);
        if (pipe(fresh) != 0) return fail(10, "pipe");
        if (fresh[0] != number)
            { errno = 0; return fail(10, "new pipe did not reuse the number"); }
    }
    if (write(fresh[1], "y", 1) != 1) return fail(10, "write new pipe");
    sleep_ms(100);
    if (w.done && w.result > 0 && reported(&w, 0xdead))
        { errno = 0; return fail(check, "the replaced number's item was reported"); }
    if (write(x[1], "x", 1) != 1) return fail(10, "write x");
    if (join_wait(&w, 2000) != 0)
        { errno = 0; return fail(check, "B did not return for x"); }
    if (w.result < 0) return fail(check, "epoll_wait");
    if (reported(&w, 0xdead))
        { errno = 0; return fail(check, "the replaced number's item was reported"); }
    if (w.result != 1 || w.events[0].data.u64 != 0x0f0f)
        { errno = 0; return fail(check, "x not reported alone"); }
    errno = 0;
    if (ctl(ep, EPOLL_CTL_MOD, number, EPOLLIN, 1) != -1 || errno != ENOENT)
        return fail(check, "the new file on the number is registered");
    close(number); close(fresh[1]); close(r[1]); close(x[0]); close(x[1]);
    close(ep);
    return 0;
}

static int listener(struct sockaddr_in *address)
{
    socklen_t length = sizeof(*address);
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)address, sizeof(*address)) != 0 ||
            listen(fd, 16) != 0 ||
            getsockname(fd, (struct sockaddr *)address, &length) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

#define TAG_LISTEN 0x4c4c4c4cULL
#define TAG_OLD 0x0bad0badULL
#define TAG_X 0x78787878ULL

/* Waits for the connection (B must report the listener and not the old
   item), accepts it onto `number`, writes to the client and lets B wait
   100 ms more: nothing may be reported for the old item. */
static int accept_onto(int check, int ep, int lfd, int client,
    const struct sockaddr_in *address, int number, struct waiter *w,
    int *accepted)
{
    if (connect(client, (const struct sockaddr *)address,
            sizeof(*address)) != 0)
        return fail(10, "connect");
    if (join_wait(w, 2000) != 0)
        { errno = 0; return fail(check, "B did not return for the connection"); }
    if (w->result < 0) return fail(check, "epoll_wait");
    if (reported(w, TAG_OLD))
        { errno = 0; return fail(check, "the closed fd's item was reported"); }
    *accepted = accept(lfd, 0, 0);
    if (*accepted < 0) return fail(10, "accept");
    if (*accepted != number)
        { errno = 0; return fail(10, "accept did not reuse the number"); }
    if (start_wait(w, ep, 100) != 0) return fail(10, "pthread_create");
    if (write(client, "d", 1) != 1) return fail(10, "write client");
    if (join_wait(w, 2000) != 0)
        { errno = 0; return fail(check, "B's 100 ms wait did not return"); }
    if (w->result < 0) return fail(check, "epoll_wait");
    if (reported(w, TAG_OLD))
        { errno = 0; return fail(check, "the closed fd's item was reported for the new socket"); }
    return 0;
}

static int check_pipe_then_socket(void)
{
    struct sockaddr_in address;
    int ep = epoll_create1(0), lfd = listener(&address), round, result;
    if (ep < 0 || lfd < 0) return fail(10, "epoll_create1/listen");
    if (ctl(ep, EPOLL_CTL_ADD, lfd, EPOLLIN, TAG_LISTEN) != 0)
        return fail(31, "ADD listener");
    for (round = 0; round != 200; ++round) {
        int client = socket(AF_INET, SOCK_STREAM, 0), p[2], accepted, number;
        struct waiter w;
        if (client < 0 || pipe(p) != 0) return fail(10, "socket/pipe");
        number = p[0];
        if (ctl(ep, EPOLL_CTL_ADD, p[0], EPOLLIN, TAG_OLD) != 0)
            return fail(31, "ADD pipe");
        if (start_wait(&w, ep, -1) != 0) return fail(10, "pthread_create");
        sleep_ms(5);
        close(p[0]);
        close(p[1]);
        result = accept_onto(31, ep, lfd, client, &address, number, &w,
            &accepted);
        if (result != 0) {
            fprintf(stderr, "epoll-concurrent check 31: round %d\n", round);
            return result;
        }
        close(accepted);
        close(client);
    }
    close(lfd); close(ep);
    return 0;
}

/* A watched accepted socket is closed, a pending connection is accepted
   onto its number and sent data.  Rounds cycle through three modes:
   0, B waits (-1) on the socket and an idle pipe x meanwhile and is woken
   by a write to x; 1, B's 300 ms wait times out instead; 2, nobody waits
   during the replacement.  Then B waits 100 ms more.  No wait may report
   the old item: the new socket has the same (st_dev, st_ino) on QNX, so
   only the close itself can retire the item.  (QNX wakes a poll() on a
   socket that another thread closes, with POLLHUP; modes 0 and 1 race
   that wake against the accept.) */
static int check_socket_then_socket(void)
{
    struct sockaddr_in address;
    int ep = epoll_create1(0), lfd = listener(&address), round;
    if (ep < 0 || lfd < 0) return fail(10, "epoll_create1/listen");
    for (round = 0; round != 60; ++round) {
        int first = socket(AF_INET, SOCK_STREAM, 0);
        int second = socket(AF_INET, SOCK_STREAM, 0);
        int old, accepted, x[2], mode = round % 3;
        char sink[8];
        struct waiter w;
        if (first < 0 || second < 0) return fail(10, "socket");
        if (connect(first, (struct sockaddr *)&address, sizeof(address)) != 0)
            return fail(10, "connect");
        old = accept(lfd, 0, 0);
        if (old < 0) return fail(10, "accept");
        if (connect(second, (struct sockaddr *)&address, sizeof(address)) != 0)
            return fail(10, "connect");
        if (pipe(x) != 0) return fail(10, "pipe");
        if (ctl(ep, EPOLL_CTL_ADD, old, EPOLLIN, TAG_OLD) != 0 ||
                ctl(ep, EPOLL_CTL_ADD, x[0], EPOLLIN, TAG_X) != 0)
            return fail(33, "ADD");
        if (mode != 2) {
            if (start_wait(&w, ep, mode == 1 ? 300 : -1) != 0)
                return fail(10, "pthread_create");
            sleep_ms(5);
        }
        close(old);
        accepted = accept(lfd, 0, 0);
        if (accepted < 0) return fail(10, "accept");
        if (accepted != old)
            { errno = 0; return fail(10, "accept did not reuse the number"); }
        if (write(second, "d", 1) != 1) return fail(10, "write second");
        if (mode != 2) {
            sleep_ms(20);
            if (mode == 0 && write(x[1], "x", 1) != 1)
                return fail(10, "write x");
            if (join_wait(&w, 2000) != 0)
                { errno = 0; return fail(33, "B did not return"); }
            if (w.result < 0) return fail(33, "epoll_wait");
            if (reported(&w, TAG_OLD) || (mode == 0) != reported(&w, TAG_X)) {
                fprintf(stderr, "epoll-concurrent check 33: round %d\n",
                    round);
                errno = 0;
                return fail(33, reported(&w, TAG_OLD) ?
                    "the closed socket's item was reported" :
                    "x reported wrongly");
            }
            if (mode == 0 && read(x[0], sink, sizeof(sink)) != 1)
                return fail(10, "read x");
        }
        if (start_wait(&w, ep, 100) != 0) return fail(10, "pthread_create");
        if (join_wait(&w, 2000) != 0)
            { errno = 0; return fail(33, "B's 100 ms wait did not return"); }
        if (w.result != 0) {
            fprintf(stderr, "epoll-concurrent check 33: round %d\n", round);
            errno = 0;
            return fail(33, reported(&w, TAG_OLD) ?
                "the closed socket's item was reported for the new socket" :
                "unexpected event");
        }
        if (epoll_ctl(ep, EPOLL_CTL_DEL, x[0], 0) != 0)
            return fail(33, "DEL x");
        close(accepted); close(first); close(second);
        close(x[0]); close(x[1]);
    }
    close(lfd); close(ep);
    return 0;
}

/* Stress (check 32).  Each mutator owns 16 of the 64 slots.  A slot's
   registration carries a fresh token as its data; when a DEL, MOD or close
   ends it, the token's retire sequence is recorded.  A waiter notes the
   sequence before each wait: a reported token retired at or before that
   sequence was removed before the wait began, so reporting it is stale. */
#define STRESS_SLOTS 64
#define STRESS_MUTATORS 4
#define STRESS_WAITERS 2
#define STRESS_TOKENS (1u << 20)

static int stress_ep;
static pthread_mutex_t stress_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned stress_sequence;
static unsigned stress_next_token = 1;
static unsigned *stress_retired;        /* by token; 0 while registered */
static volatile int stress_stop;
static volatile int stress_failed;
static volatile unsigned stress_reports, stress_operations;

struct stress_slot { int read_fd, write_fd; unsigned token; };
static struct stress_slot stress_slots[STRESS_SLOTS];

static unsigned new_token(void)
{
    unsigned token;
    pthread_mutex_lock(&stress_lock);
    token = stress_next_token < STRESS_TOKENS ? stress_next_token++ : 0;
    pthread_mutex_unlock(&stress_lock);
    return token;
}

/* Runs op (DEL, MOD to new_token, or close) and retires the old token
   under the lock, so a waiter's sequence is either before or after it. */
static void retire(struct stress_slot *slot, int op, unsigned new_token_value)
{
    pthread_mutex_lock(&stress_lock);
    if (op == EPOLL_CTL_DEL) {
        if (epoll_ctl(stress_ep, EPOLL_CTL_DEL, slot->read_fd, 0) != 0)
            stress_failed = 1;
    } else if (op == EPOLL_CTL_MOD) {
        if (ctl(stress_ep, EPOLL_CTL_MOD, slot->read_fd, EPOLLIN,
                new_token_value) != 0)
            stress_failed = 1;
    } else {
        close(slot->read_fd);
        slot->read_fd = -1;
    }
    stress_retired[slot->token] = ++stress_sequence;
    slot->token = op == EPOLL_CTL_MOD ? new_token_value : 0;
    pthread_mutex_unlock(&stress_lock);
}

static void *stress_mutator(void *argument)
{
    int first = (int)(intptr_t)argument * (STRESS_SLOTS / STRESS_MUTATORS);
    unsigned seed = (unsigned)first * 2654435761u + 1;
    while (!stress_stop && !stress_failed) {
        struct stress_slot *slot = &stress_slots[first +
            (int)(rand_r(&seed) % (STRESS_SLOTS / STRESS_MUTATORS))];
        int choice = (int)(rand_r(&seed) % 4);
        __sync_add_and_fetch(&stress_operations, 1);
        if (slot->read_fd < 0) {
            int p[2];
            if (slot->write_fd >= 0) { close(slot->write_fd); slot->write_fd = -1; }
            if (pipe(p) != 0) continue;
            slot->read_fd = p[0];
            slot->write_fd = p[1];
            if (rand_r(&seed) % 2) (void)write(p[1], "s", 1);
        } else if (slot->token == 0) {
            unsigned token;
            if (choice == 0) { retire(slot, -1, 0); continue; }
            token = new_token();
            if (token == 0) { stress_stop = 1; break; }
            if (ctl(stress_ep, EPOLL_CTL_ADD, slot->read_fd, EPOLLIN,
                    token) != 0) {
                stress_failed = 2;
                break;
            }
            slot->token = token;
        } else if (choice == 0) {
            retire(slot, EPOLL_CTL_DEL, 0);
        } else if (choice == 1) {
            unsigned token = new_token();
            if (token == 0) { stress_stop = 1; break; }
            retire(slot, EPOLL_CTL_MOD, token);
        } else if (choice == 2) {
            retire(slot, -1, 0);
        } else {
            (void)write(slot->write_fd, "s", 1);
        }
    }
    return 0;
}

static void *stress_waiter(void *argument)
{
    struct epoll_event events[16];
    (void)argument;
    while (!stress_stop && !stress_failed) {
        unsigned start;
        int n, i;
        pthread_mutex_lock(&stress_lock);
        start = stress_sequence;
        pthread_mutex_unlock(&stress_lock);
        n = epoll_wait(stress_ep, events, 16, 20);
        if (n < 0) {
            if (errno == EINTR) continue;
            stress_failed = 3;
            break;
        }
        pthread_mutex_lock(&stress_lock);
        for (i = 0; i != n; ++i) {
            uint64_t token = events[i].data.u64;
            if (token == 0 || token >= stress_next_token ||
                    (stress_retired[token] != 0 &&
                    stress_retired[token] <= start)) {
                fprintf(stderr, "epoll-concurrent check 32: stale data %llu "
                    "(retired at %u, wait began at %u)\n",
                    (unsigned long long)token,
                    token < STRESS_TOKENS ? stress_retired[token] : 0, start);
                stress_failed = 4;
            }
        }
        pthread_mutex_unlock(&stress_lock);
        __sync_add_and_fetch(&stress_reports, (unsigned)n);
    }
    return 0;
}

static int check_stress(void)
{
    pthread_t mutators[STRESS_MUTATORS], waiters[STRESS_WAITERS];
    int i;
    stress_ep = epoll_create1(0);
    stress_retired = calloc(STRESS_TOKENS, sizeof(*stress_retired));
    if (stress_ep < 0 || stress_retired == 0) return fail(10, "setup");
    for (i = 0; i != STRESS_SLOTS; ++i) {
        stress_slots[i].read_fd = -1;
        stress_slots[i].write_fd = -1;
    }
    for (i = 0; i != STRESS_WAITERS; ++i)
        if (pthread_create(&waiters[i], 0, stress_waiter, 0) != 0)
            return fail(10, "pthread_create");
    for (i = 0; i != STRESS_MUTATORS; ++i)
        if (pthread_create(&mutators[i], 0, stress_mutator,
                (void *)(intptr_t)i) != 0)
            return fail(10, "pthread_create");
    for (i = 0; i != 50 && !stress_failed && !stress_stop; ++i) sleep_ms(100);
    stress_stop = 1;
    for (i = 0; i != STRESS_MUTATORS; ++i) pthread_join(mutators[i], 0);
    for (i = 0; i != STRESS_WAITERS; ++i) pthread_join(waiters[i], 0);
    fprintf(stderr, "epoll-concurrent check 32: %u operations, %u events, "
        "%u tokens\n", stress_operations, stress_reports,
        stress_next_token - 1);
    if (stress_failed) {
        errno = 0;
        return fail(32, stress_failed == 4 ? "stale data reported" :
            stress_failed == 3 ? "epoll_wait failed" : "epoll_ctl failed");
    }
    if (stress_reports == 0)
        { errno = 0; return fail(32, "no events were reported at all"); }
    for (i = 0; i != STRESS_SLOTS; ++i) {
        if (stress_slots[i].read_fd >= 0) close(stress_slots[i].read_fd);
        if (stress_slots[i].write_fd >= 0) close(stress_slots[i].write_fd);
    }
    close(stress_ep);
    return 0;
}

/* EPOLL_CONCURRENT_CHECK=N runs only check N (to show one check's RED on
   a core that fails an earlier one). */
static int wanted(int check)
{
    const char *only = getenv("EPOLL_CONCURRENT_CHECK");
    return only == 0 || atoi(only) == check;
}

int main(void)
{
    int result = 0;
    if (wanted(16) && (arm(16, 10), result = check_identity()) != 0)
        return result;
    if (wanted(11) && (arm(11, 10), result = check_add_wakes()) != 0)
        return result;
    if (wanted(12) && (arm(12, 10), result = check_mod_wakes()) != 0)
        return result;
    if (wanted(13) && (arm(13, 10), result = check_del_first()) != 0)
        return result;
    if (wanted(14) && (arm(14, 10), result = check_replaced(14, 0)) != 0)
        return result;
    if (wanted(15) && (arm(15, 10), result = check_replaced(15, 1)) != 0)
        return result;
    if (wanted(31) && (arm(31, 60), result = check_pipe_then_socket()) != 0)
        return result;
    if (wanted(33) && (arm(33, 30), result = check_socket_then_socket()) != 0)
        return result;
    if (wanted(32) && (arm(32, 30), result = check_stress()) != 0)
        return result;
    return 0;
}
