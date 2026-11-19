/*
 * Nested epoll, epoll in poll/select, and epoll across fork and exec
 * (guest fixture, musl).
 *
 * Checks (the exit code is the check number, also printed on stderr with
 * errno; a check that hangs is ended by the watchdog thread with exit code
 * 100 + the check number):
 *   10 setup failed (EPOLL_NESTING_CHECK=N runs check N alone)
 *   11 epoll A watches epoll B, which watches a pipe: nothing before a
 *      write; a write (also one from another thread while A's wait blocks)
 *      makes A report B with EPOLLIN; B's own wait then reports the pipe;
 *      once the pipe is read A reports nothing
 *   12 poll() on B reports POLLIN while its pipe holds data and nothing
 *      otherwise, and a blocked poll() wakes on a write; select() agrees
 *   13 cycles and depth: adding A to B after B is in A fails ELOOP; in a
 *      chain of 6 nested epolls the 6th ADD fails ELOOP (5 succeed)
 *   14 an EPOLLONESHOT item inside B is still reported by B's own wait
 *      after A (and poll() on B) saw B ready: probing doesn't consume it;
 *      after B's wait took the one shot, A reports nothing
 *   15 fork: the child's epoll still reports the inherited pipe; a
 *      child-side DEL doesn't affect the parent's wait
 *   16 exec without EPOLL_CLOEXEC: the new image's epoll (3 items) reports
 *      the inherited pipe with its data, and its eventfd once written
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile int current_check;
static volatile int watchdog_ticks;     /* 100 ms ticks since arm() */

static int fail(int check, const char *what)
{
    fprintf(stderr, "epoll-nesting check %d failed: %s (errno %d %s)\n",
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
            fprintf(stderr, "epoll-nesting check %d timed out\n",
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

/* The count of one wait; the first event in *event. */
static int wait_one(int ep, int timeout, struct epoll_event *event)
{
    struct epoll_event events[8], ignored;
    int count;
    if (event == 0) event = &ignored;
    memset(events, 0, sizeof(events));
    do count = epoll_wait(ep, events, 8, timeout);
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

static int poll_in(int fd, int timeout)
{
    struct pollfd entry;
    int count;
    entry.fd = fd;
    entry.events = POLLIN;
    entry.revents = 0;
    do count = poll(&entry, 1, timeout);
    while (count < 0 && errno == EINTR);
    if (count < 0) return -1;
    return count == 0 ? 0 : entry.revents;
}

static int select_in(int fd, int timeout_ms)
{
    fd_set set;
    struct timeval timeout;
    int count;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    count = select(fd + 1, &set, 0, 0, &timeout);
    if (count < 0) return -1;
    return count == 1 && FD_ISSET(fd, &set);
}

static int drain(int fd)
{
    char buffer[64];
    int flags = fcntl(fd, F_GETFL);
    ssize_t got;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    do got = read(fd, buffer, sizeof(buffer));
    while (got > 0);
    fcntl(fd, F_SETFL, flags);
    return got == -1 && errno == EAGAIN ? 0 : -1;
}

struct delayed_write { int fd; int delay_ms; };

static void *write_later(void *argument)
{
    struct delayed_write *job = argument;
    sleep_ms(job->delay_ms);
    if (write(job->fd, "w", 1) != 1) _exit(10);
    return 0;
}

static int start_write(pthread_t *thread, struct delayed_write *job, int fd,
    int delay_ms)
{
    job->fd = fd;
    job->delay_ms = delay_ms;
    return pthread_create(thread, 0, write_later, job);
}

static int a_ep, b_ep, b_pipe[2];

/* A and B, with B watching its pipe (A doesn't watch B yet). */
static int setup_ab(void)
{
    arm(10);
    a_ep = epoll_create1(0);
    b_ep = epoll_create1(0);
    if (a_ep < 0 || b_ep < 0 || pipe(b_pipe) != 0)
        return fail(10, "epoll_create1/pipe");
    if (ctl(b_ep, EPOLL_CTL_ADD, b_pipe[0], EPOLLIN, 0x50) != 0)
        return fail(10, "ADD pipe to B");
    return 0;
}

static int check_nested(void)
{
    struct delayed_write job;
    pthread_t thread;
    long long started;
    long events;
    arm(11);
    if (ctl(a_ep, EPOLL_CTL_ADD, b_ep, EPOLLIN, 0xb) != 0)
        return fail(11, "ADD B to A");
    if (wait_one(a_ep, 0, 0) != 0) return fail(11, "A reported B early");
    if (wait_one(a_ep, 50, 0) != 0)
        return fail(11, "A reported B early (50 ms)");
    if (write(b_pipe[1], "x", 1) != 1) return fail(10, "write");
    events = one_event(a_ep, 0, 0xb);
    if (events != EPOLLIN) {
        fprintf(stderr, "epoll-nesting: A gave events %#lx\n", events);
        return fail(11, "A did not report B with EPOLLIN");
    }
    /* Level-triggered: still ready, and B's own wait is unaffected. */
    if (one_event(a_ep, 0, 0xb) != EPOLLIN)
        return fail(11, "A did not report B a second time");
    if (one_event(b_ep, 0, 0x50) != EPOLLIN)
        return fail(11, "B did not report the pipe");
    if (drain(b_pipe[0]) != 0) return fail(10, "drain");
    if (wait_one(a_ep, 0, 0) != 0)
        return fail(11, "A reported B with the pipe empty");
    /* A blocked wait on A wakes on a write into B's pipe. */
    if (start_write(&thread, &job, b_pipe[1], 150) != 0)
        return fail(10, "pthread_create");
    started = now_ms();
    if (one_event(a_ep, 3000, 0xb) != EPOLLIN)
        return fail(11, "blocked wait on A missed the write");
    pthread_join(thread, 0);
    if (now_ms() - started > 2000)
        return fail(11, "blocked wait on A woke late");
    if (drain(b_pipe[0]) != 0) return fail(10, "drain");
    return 0;
}

static int check_poll_select(void)
{
    struct delayed_write job;
    pthread_t thread;
    int revents;
    arm(12);
    if (poll_in(b_ep, 0) != 0) return fail(12, "poll(B) ready while empty");
    if (poll_in(b_ep, 50) != 0)
        return fail(12, "poll(B, 50) ready while empty");
    if (select_in(b_ep, 0) != 0)
        return fail(12, "select(B) ready while empty");
    if (write(b_pipe[1], "y", 1) != 1) return fail(10, "write");
    revents = poll_in(b_ep, 0);
    if (revents < 0 || (revents & POLLIN) == 0) {
        fprintf(stderr, "epoll-nesting: poll(B) revents %#x\n", revents);
        return fail(12, "poll(B) not POLLIN with data");
    }
    if ((revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL)) != 0) {
        fprintf(stderr, "epoll-nesting: poll(B) revents %#x\n", revents);
        return fail(12, "poll(B) reported more than POLLIN");
    }
    if (select_in(b_ep, 0) != 1)
        return fail(12, "select(B) not readable with data");
    /* Probing B didn't take the event. */
    if (one_event(b_ep, 0, 0x50) != EPOLLIN)
        return fail(12, "B lost its event after poll/select");
    if (drain(b_pipe[0]) != 0) return fail(10, "drain");
    if (poll_in(b_ep, 0) != 0) return fail(12, "poll(B) ready after drain");
    if (select_in(b_ep, 0) != 0)
        return fail(12, "select(B) ready after drain");
    /* Blocked poll and select wake on a write. */
    if (start_write(&thread, &job, b_pipe[1], 150) != 0)
        return fail(10, "pthread_create");
    revents = poll_in(b_ep, 3000);
    pthread_join(thread, 0);
    if (revents <= 0 || (revents & POLLIN) == 0)
        return fail(12, "blocked poll(B) missed the write");
    if (drain(b_pipe[0]) != 0) return fail(10, "drain");
    if (start_write(&thread, &job, b_pipe[1], 150) != 0)
        return fail(10, "pthread_create");
    revents = select_in(b_ep, 3000);
    pthread_join(thread, 0);
    if (revents != 1) return fail(12, "blocked select(B) missed the write");
    if (drain(b_pipe[0]) != 0) return fail(10, "drain");
    return 0;
}

static int check_loops(void)
{
    int chain[7], i, e;
    arm(13);
    /* Run alone, A doesn't watch B yet. */
    if (ctl(a_ep, EPOLL_CTL_ADD, b_ep, EPOLLIN, 0xb) != 0 && errno != EEXIST)
        return fail(13, "ADD B to A");
    if ((e = ctl_error(b_ep, EPOLL_CTL_ADD, a_ep, EPOLLIN)) != ELOOP) {
        errno = e;
        return fail(13, "A into B (a cycle) not ELOOP");
    }
    for (i = 0; i != 7; ++i)
        if ((chain[i] = epoll_create1(0)) < 0)
            return fail(10, "epoll_create1 (chain)");
    for (i = 0; i != 5; ++i)
        if (ctl(chain[i], EPOLL_CTL_ADD, chain[i + 1], EPOLLIN, i) != 0) {
            fprintf(stderr, "epoll-nesting: chain ADD %d\n", i + 1);
            return fail(13, "chain ADD within the depth limit");
        }
    if ((e = ctl_error(chain[5], EPOLL_CTL_ADD, chain[6], EPOLLIN)) != ELOOP) {
        errno = e;
        return fail(13, "6th chain ADD not ELOOP");
    }
    /* The other direction: chain[6] holding a chain top is as deep. */
    if ((e = ctl_error(chain[6], EPOLL_CTL_ADD, chain[0], EPOLLIN)) != ELOOP) {
        errno = e;
        return fail(13, "ADD above a full chain not ELOOP");
    }
    for (i = 0; i != 7; ++i) close(chain[i]);
    return 0;
}

static int check_oneshot_probe(void)
{
    int a = epoll_create1(0), b = epoll_create1(0), p[2], revents;
    arm(14);
    if (a < 0 || b < 0 || pipe(p) != 0) return fail(10, "epoll/pipe");
    if (ctl(b, EPOLL_CTL_ADD, p[0], EPOLLIN | EPOLLONESHOT, 0x14) != 0)
        return fail(14, "ADD one-shot pipe to B");
    if (ctl(a, EPOLL_CTL_ADD, b, EPOLLIN, 0xb) != 0)
        return fail(14, "ADD B to A");
    if (write(p[1], "z", 1) != 1) return fail(10, "write");
    if (one_event(a, 1000, 0xb) != EPOLLIN)
        return fail(14, "A not woken by the one-shot item");
    if (one_event(a, 0, 0xb) != EPOLLIN)
        return fail(14, "A's second look consumed the one shot");
    revents = poll_in(b, 0);
    if (revents <= 0 || (revents & POLLIN) == 0)
        return fail(14, "poll(B) not ready with the one shot armed");
    if (one_event(b, 0, 0x14) != EPOLLIN)
        return fail(14, "B's own wait lost the one-shot event");
    if (wait_one(b, 0, 0) != 0) return fail(14, "one shot reported twice");
    if (wait_one(a, 0, 0) != 0)
        return fail(14, "A reported B after its one shot was taken");
    if (poll_in(b, 0) != 0)
        return fail(14, "poll(B) ready after its one shot was taken");
    close(a); close(b); close(p[0]); close(p[1]);
    return 0;
}

static int wait_child(pid_t pid)
{
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 200 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

static int check_fork(void)
{
    int ep = epoll_create1(0), p[2], status;
    pid_t pid;
    arm(15);
    if (ep < 0 || pipe(p) != 0) return fail(10, "epoll/pipe");
    if (ctl(ep, EPOLL_CTL_ADD, p[0], EPOLLIN, 0x15) != 0)
        return fail(15, "ADD");
    if (write(p[1], "f", 1) != 1) return fail(10, "write");
    pid = fork();
    if (pid < 0) return fail(10, "fork");
    if (pid == 0) {
        struct epoll_event event;
        if (one_event(ep, 1000, 0x15) != EPOLLIN) _exit(2);
        if (epoll_ctl(ep, EPOLL_CTL_DEL, p[0], &event) != 0) _exit(3);
        if (wait_one(ep, 0, 0) != 0) _exit(4);
        if (wait_one(ep, 50, 0) != 0) _exit(5);
        /* The child's own instance still works for a new item. */
        if (ctl(ep, EPOLL_CTL_ADD, p[0], EPOLLIN, 0x51) != 0) _exit(6);
        if (one_event(ep, 0, 0x51) != EPOLLIN) _exit(7);
        _exit(0);
    }
    status = wait_child(pid);
    if (status != 0) {
        fprintf(stderr, "epoll-nesting: fork child exit %d\n", status);
        errno = 0;
        return fail(15, "child's epoll");
    }
    if (one_event(ep, 0, 0x15) != EPOLLIN)
        return fail(15, "parent's item gone after the child's DEL/ADD");
    close(ep); close(p[0]); close(p[1]);
    return 0;
}

/* The image check 16 execs: argv "exec-child" ep efd. */
static int exec_child(const char *ep_text, const char *efd_text)
{
    struct epoll_event events[8];
    int ep = atoi(ep_text), efd = atoi(efd_text), count, i, saw_eventfd = 0;
    uint64_t one = 1;
    char byte = 0;
    arm(16);
    memset(events, 0, sizeof(events));
    count = epoll_wait(ep, events, 8, 1000);
    if (count != 1) {
        fprintf(stderr, "epoll-nesting exec child: %d events (errno %d)\n",
            count, count < 0 ? errno : 0);
        return 2;
    }
    if (events[0].data.u64 != 0x1616000000000001ull ||
            events[0].events != EPOLLIN) return 3;
    if (write(efd, &one, sizeof(one)) != sizeof(one)) return 4;
    count = epoll_wait(ep, events, 8, 1000);
    for (i = 0; i < count; ++i)
        if (events[i].data.u64 == 0x1616000000000002ull &&
                events[i].events == EPOLLIN) saw_eventfd = 1;
    if (count != 2 || !saw_eventfd) {
        fprintf(stderr, "epoll-nesting exec child: %d events after the "
            "eventfd write\n", count);
        return 5;
    }
    /* The pipe item's number is the same file in this image. */
    for (i = 0; i < count; ++i)
        if (events[i].data.u64 == 0x1616000000000001ull) byte = 1;
    if (!byte) return 6;
    return 0;
}

static int check_exec(const char *self)
{
    int ep = epoll_create1(0), p[2], s[2], efd = eventfd(0, EFD_NONBLOCK),
        status;
    char ep_text[16], efd_text[16];
    pid_t pid;
    arm(16);
    if (ep < 0 || efd < 0 || pipe(p) != 0 ||
            socketpair(AF_UNIX, SOCK_STREAM, 0, s) != 0)
        return fail(10, "epoll/eventfd/pipe/socketpair");
    if (ctl(ep, EPOLL_CTL_ADD, p[0], EPOLLIN, 0x1616000000000001ull) != 0 ||
            ctl(ep, EPOLL_CTL_ADD, efd, EPOLLIN,
                0x1616000000000002ull) != 0 ||
            ctl(ep, EPOLL_CTL_ADD, s[0], EPOLLIN,
                0x1616000000000003ull) != 0)
        return fail(16, "ADD the three items");
    if (write(p[1], "e", 1) != 1) return fail(10, "write");
    snprintf(ep_text, sizeof(ep_text), "%d", ep);
    snprintf(efd_text, sizeof(efd_text), "%d", efd);
    pid = fork();
    if (pid < 0) return fail(10, "fork");
    if (pid == 0) {
        execl("/proc/self/exe", self, "exec-child", ep_text, efd_text,
            (char *)0);
        execl(self, self, "exec-child", ep_text, efd_text, (char *)0);
        _exit(9);
    }
    status = wait_child(pid);
    if (status != 0) {
        fprintf(stderr, "epoll-nesting: exec child exit %d\n", status);
        errno = 0;
        return fail(16, "the new image's epoll");
    }
    close(ep); close(efd); close(p[0]); close(p[1]); close(s[0]);
    close(s[1]);
    return 0;
}

/* EPOLL_NESTING_CHECK=N runs check N alone (after the A/B setup). */
int main(int argc, char **argv)
{
    const char *only_text = getenv("EPOLL_NESTING_CHECK");
    int only = only_text != 0 ? atoi(only_text) : 0, result;
    signal(SIGPIPE, SIG_IGN);
    if (argc == 4 && strcmp(argv[1], "exec-child") == 0)
        return exec_child(argv[2], argv[3]);
    if ((result = setup_ab()) != 0) return result;
    if ((only == 0 || only == 11) && (result = check_nested()) != 0)
        return result;
    if ((only == 0 || only == 12) && (result = check_poll_select()) != 0)
        return result;
    if ((only == 0 || only == 13) && (result = check_loops()) != 0)
        return result;
    if ((only == 0 || only == 14) && (result = check_oneshot_probe()) != 0)
        return result;
    if ((only == 0 || only == 15) && (result = check_fork()) != 0)
        return result;
    if ((only == 0 || only == 16) && (result = check_exec(argv[0])) != 0)
        return result;
    fprintf(stderr, "epoll-nesting: all checks passed\n");
    return 0;
}
