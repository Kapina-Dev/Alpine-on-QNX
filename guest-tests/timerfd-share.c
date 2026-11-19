/*
 * The Linux timerfd: blocking reads, signals, fork, exec, close while
 * blocked, SCM_RIGHTS, TFD_TIMER_CANCEL_ON_SET, many timers, and the
 * CLOCK_BOOTTIME family in clock_gettime (guest fixture, musl).
 *
 * Checks (the exit code is the check number, also printed on stderr with
 * errno; a check that hangs is ended by the watchdog thread, or SIGALRM
 * in a forked child, with exit code 100 + the check number):
 *   10 setup failed
 *   11 a blocking read on a 50 ms one-shot returns 1 after 40-150 ms
 *   12 a blocking read interrupted by SIGUSR1 without SA_RESTART fails
 *      EINTR; with SA_RESTART it keeps waiting and returns the count
 *   13 fork: the parent arms a 20 ms periodic timer, the child's blocking
 *      read gets >= 1 and the parent's later read gets the rest; then
 *      parent and child both read a 1 ms periodic timer nonblocking for
 *      300 ms and every expiration is counted once: parent + child +
 *      final read equals the expirations due by the clock (+-1)
 *   14 exec: a timerfd without TFD_CLOEXEC, armed for 100 ms by a forked
 *      child, survives that child's execve of this fixture; the new image
 *      polls it readable (its own timer fires) and reads 1
 *   15 thread B blocks in read on a disarmed timerfd; the main thread
 *      closes it; B fails EBADF within 1 s of the close; then 4 x 64 new
 *      timerfds can be created, armed and closed (no QNX timer leak)
 *   16 SCM_RIGHTS: a timerfd sent to a forked child that closed its own
 *      copy becomes readable there and reads 1 when the parent arms it
 *   17 TFD_TIMER_CANCEL_ON_SET: accepted with an absolute CLOCK_REALTIME
 *      deadline, and (as Linux) accepted and ignored for a relative one
 *      and for CLOCK_MONOTONIC; no false ECANCELED within 100 ms; if
 *      clock_settime is allowed, a blocked read fails ECANCELED after the
 *      clock moves 2 s (else skipped with the reason printed)
 *   18 50 timerfds with periods of 5-54 ms all count correctly (within
 *      10 %, at least +-1) over 500 ms
 *   19 clock_gettime works for CLOCK_BOOTTIME (7), CLOCK_REALTIME_ALARM
 *      (8) and CLOCK_BOOTTIME_ALARM (9), clock_getres for 7, and
 *      CLOCK_BOOTTIME moves with CLOCK_MONOTONIC over 50 ms
 *
 * "timerfd-share --only N" runs check N alone; "--only 13-spin" runs
 * the nonblocking double-count half of check 13 alone.
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
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NR_TIMERFD_CREATE 350
#define NR_TIMERFD_SETTIME 353
#define NR_TIMERFD_GETTIME 354
#define TFD_NONBLOCK_FLAG 0x800
#define TFD_ABSTIME 1
#define TFD_CANCEL_ON_SET 2
#define CLOCK_BOOTTIME_ID 7
#define CLOCK_REALTIME_ALARM_ID 8
#define CLOCK_BOOTTIME_ALARM_ID 9
#define MS 1000000LL
#define SECOND 1000000000LL

struct spec32 { int32_t value_sec, value_nsec, interval_sec, interval_nsec; };

static volatile int current_check;
static volatile int watchdog_ticks;     /* 100 ms ticks since arm() */
static volatile int usr1_count;

static int fail(int check, const char *what)
{
    fprintf(stderr, "timerfd-share check %d failed: %s (errno %d %s)\n",
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
    if (clock_gettime(clock, &now) != 0) return -1;
    return (int64_t)now.tv_sec * SECOND + now.tv_nsec;
}

static int64_t mono(void)
{
    return now_ns(CLOCK_MONOTONIC);
}

static void *watchdog(void *argument)
{
    (void)argument;
    for (;;) {
        sleep_ms(100);
        if (__sync_add_and_fetch(&watchdog_ticks, 1) >= 100) {
            fprintf(stderr, "timerfd-share check %d timed out\n",
                current_check);
            _exit(100 + current_check);
        }
    }
    return 0;
}

/* Starts the check's 10 s bound (watchdog thread in this process). */
static void arm(int check)
{
    static int started;
    current_check = check;
    watchdog_ticks = 0;
    __sync_synchronize();
    if (!started) {
        pthread_t thread;
        sigset_t all, old;
        /* The watchdog never takes the test's SIGUSR1. */
        sigfillset(&all);
        pthread_sigmask(SIG_SETMASK, &all, &old);
        if (pthread_create(&thread, 0, watchdog, 0) == 0) started = 1;
        pthread_sigmask(SIG_SETMASK, &old, 0);
    }
}

static void child_timed_out(int signal_number)
{
    static const char text[] = "timerfd-share: a child timed out\n";
    (void)signal_number;
    (void)write(2, text, sizeof(text) - 1);
    _exit(100 + current_check);
}

/* A forked child has no watchdog thread: SIGALRM bounds it. */
static void child_bound(void)
{
    signal(SIGALRM, child_timed_out);
    alarm(10);
}

static int tfd_create(int clock, int flags)
{
    return (int)syscall(NR_TIMERFD_CREATE, clock, flags);
}

static int settime(int fd, int flags, int64_t value, int64_t interval)
{
    struct spec32 spec;
    spec.value_sec = (int32_t)(value / SECOND);
    spec.value_nsec = (int32_t)(value % SECOND);
    spec.interval_sec = (int32_t)(interval / SECOND);
    spec.interval_nsec = (int32_t)(interval % SECOND);
    return (int)syscall(NR_TIMERFD_SETTIME, fd, flags, &spec, 0);
}

static int gettime(int fd, int64_t *value, int64_t *interval)
{
    struct spec32 spec;
    if (syscall(NR_TIMERFD_GETTIME, fd, &spec) != 0) return -1;
    *value = spec.value_sec * SECOND + spec.value_nsec;
    *interval = spec.interval_sec * SECOND + spec.interval_nsec;
    return 0;
}

/* 8 bytes read into *value: 1; EAGAIN: 0 with *value 0; else -1. */
static int read_count(int fd, uint64_t *value)
{
    ssize_t got = read(fd, value, sizeof(*value));
    if (got == (ssize_t)sizeof(*value)) return 1;
    *value = 0;
    if (got < 0 && errno == EAGAIN) return 0;
    if (got >= 0) errno = EIO;
    return -1;
}

static int wait_child(pid_t pid)
{
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 200 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

static int check_blocking_read(void)
{
    uint64_t value = 0;
    int64_t start, elapsed;
    int t = tfd_create(CLOCK_MONOTONIC, 0);
    if (t < 0) return fail(10, "timerfd_create");
    start = mono();
    if (settime(t, 0, 50 * MS, 0) != 0) return fail(10, "settime 50 ms");
    if (read_count(t, &value) != 1) return fail(11, "blocking read");
    elapsed = mono() - start;
    if (value != 1) {
        fprintf(stderr, "count %llu\n", (unsigned long long)value);
        errno = 0;
        return fail(11, "count != 1");
    }
    if (elapsed < 40 * MS || elapsed > 150 * MS) {
        fprintf(stderr, "read returned after %lld ms\n",
            (long long)(elapsed / MS));
        errno = 0;
        return fail(11, "read outside 40-150 ms");
    }
    close(t);
    return 0;
}

static void on_usr1(int signal_number)
{
    (void)signal_number;
    ++usr1_count;
}

struct kick { pthread_t target; int delay_ms; };

static void *kicker(void *argument)
{
    struct kick *kick = argument;
    sleep_ms(kick->delay_ms);
    pthread_kill(kick->target, SIGUSR1);
    return 0;
}

static int interrupted_read(int restart, int64_t timer, ssize_t *got,
    int *error, uint64_t *value, int64_t *elapsed)
{
    struct sigaction action;
    struct kick kick;
    pthread_t thread;
    int64_t start;
    int t = tfd_create(CLOCK_MONOTONIC, 0);
    if (t < 0) return -1;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_usr1;
    action.sa_flags = restart ? SA_RESTART : 0;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, 0) != 0) return -1;
    usr1_count = 0;
    kick.target = pthread_self();
    kick.delay_ms = 100;
    start = mono();
    if (settime(t, 0, timer, 0) != 0) return -1;
    if (pthread_create(&thread, 0, kicker, &kick) != 0) return -1;
    errno = 0;
    *got = read(t, value, sizeof(*value));
    *error = errno;
    *elapsed = mono() - start;
    pthread_join(thread, 0);
    close(t);
    return 0;
}

static int check_signals(void)
{
    ssize_t got;
    uint64_t value = 0;
    int64_t elapsed;
    int error;
    if (interrupted_read(0, 2 * SECOND, &got, &error, &value, &elapsed) != 0)
        return fail(10, "setup without SA_RESTART");
    if (got != -1 || error != EINTR || usr1_count != 1) {
        fprintf(stderr, "got %d errno %d handler ran %d times\n", (int)got,
            error, usr1_count);
        errno = error;
        return fail(12, "read without SA_RESTART not EINTR");
    }
    if (interrupted_read(1, 300 * MS, &got, &error, &value, &elapsed) != 0)
        return fail(10, "setup with SA_RESTART");
    if (got != 8 || value != 1 || usr1_count != 1 || elapsed < 250 * MS) {
        fprintf(stderr, "got %d errno %d value %llu handler %d after %lld "
            "ms\n", (int)got, error, (unsigned long long)value, usr1_count,
            (long long)(elapsed / MS));
        errno = error;
        return fail(12, "read with SA_RESTART did not keep waiting");
    }
    signal(SIGUSR1, SIG_DFL);
    return 0;
}

/* Reads a nonblocking timerfd until the monotonic time end; the sum. */
static int64_t spin_sum(int t, int64_t end)
{
    uint64_t value;
    int64_t sum = 0;
    while (mono() < end) {
        int r = read_count(t, &value);
        if (r < 0) return -1;
        sum += (int64_t)value;
    }
    return sum;
}

static int64_t due(int64_t first, int64_t interval, int64_t at)
{
    return at < first ? 0 : (at - first) / interval + 1;
}

static int check_fork(void)
{
    uint64_t value = 0;
    int t = tfd_create(CLOCK_MONOTONIC, 0), status;
    pid_t pid;
    if (t < 0) return fail(10, "timerfd_create");
    if (settime(t, 0, 20 * MS, 20 * MS) != 0) return fail(10, "settime");
    pid = fork();
    if (pid < 0) return fail(10, "fork");
    if (pid == 0) {
        child_bound();
        if (read_count(t, &value) != 1) _exit(2);
        _exit(value >= 1 ? 0 : 3);
    }
    status = wait_child(pid);
    if (status != 0) {
        fprintf(stderr, "child status %d\n", status);
        errno = 0;
        return fail(13, "child blocking read");
    }
    sleep_ms(50);
    if (read_count(t, &value) != 1 || value < 1)
        return fail(13, "parent read after the child");
    close(t);
    return 0;
}

/* Every expiration counted once across the two processes (alone with
   "--only 13-spin": the blocking half needs blocking reads). */
static int check_fork_spin(void)
{
    uint64_t value = 0;
    int64_t first, end, parent_sum, child_sum = -1, final_sum, t_a, t_b, lo,
        hi, total;
    int t, status, sums[2];
    pid_t pid;
    t = tfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK_FLAG);
    if (t < 0 || pipe(sums) != 0) return fail(10, "timerfd or pipe");
    first = mono() + 5 * MS;
    end = first + 300 * MS;
    if (settime(t, TFD_ABSTIME, first, 1 * MS) != 0)
        return fail(10, "settime 1 ms periodic");
    pid = fork();
    if (pid < 0) return fail(10, "fork");
    if (pid == 0) {
        int64_t sum;
        child_bound();
        sum = spin_sum(t, end);
        _exit(write(sums[1], &sum, sizeof(sum)) == sizeof(sum) ? 0 : 2);
    }
    parent_sum = spin_sum(t, end);
    status = wait_child(pid);
    if (parent_sum < 0 || status != 0 ||
            read(sums[0], &child_sum, sizeof(child_sum)) !=
            (ssize_t)sizeof(child_sum) || child_sum < 0) {
        fprintf(stderr, "parent sum %lld child status %d\n",
            (long long)parent_sum, status);
        return fail(13, "spinning readers");
    }
    t_a = mono();
    if (read_count(t, &value) < 0) return fail(13, "final read");
    t_b = mono();
    final_sum = (int64_t)value;
    total = parent_sum + child_sum + final_sum;
    lo = due(first, 1 * MS, t_a);
    hi = due(first, 1 * MS, t_b);
    fprintf(stderr, "timerfd-share 13: parent %lld + child %lld + final "
        "%lld = %lld, due %lld-%lld\n", (long long)parent_sum,
        (long long)child_sum, (long long)final_sum, (long long)total,
        (long long)lo, (long long)hi);
    if (child_sum < 1 || parent_sum < 1) {
        errno = 0;
        return fail(13, "a process counted no expiration");
    }
    if (total < lo - 1 || total > hi + 1) {
        errno = 0;
        return fail(13, "expirations counted twice or lost across fork");
    }
    close(t);
    close(sums[0]);
    close(sums[1]);
    return 0;
}

/* The image check 14 execs: argv "exec-child" fd. */
static int exec_child(const char *fd_text)
{
    struct pollfd entry;
    uint64_t value = 0;
    int t = atoi(fd_text), ready;
    arm(14);
    entry.fd = t;
    entry.events = POLLIN;
    entry.revents = 0;
    ready = poll(&entry, 1, 2000);
    if (ready != 1 || (entry.revents & POLLIN) == 0) {
        fprintf(stderr, "exec image poll %d revents 0x%x errno %d\n", ready,
            entry.revents, errno);
        return 2;
    }
    if (read_count(t, &value) != 1) {
        fprintf(stderr, "exec image read errno %d\n", errno);
        return 3;
    }
    if (value != 1) return 4;
    return 0;
}

static int check_exec(const char *self)
{
    char t_text[16];
    pid_t pid;
    int t = tfd_create(CLOCK_MONOTONIC, 0), status;
    if (t < 0) return fail(10, "timerfd_create");
    snprintf(t_text, sizeof(t_text), "%d", t);
    pid = fork();
    if (pid < 0) return fail(10, "fork");
    if (pid == 0) {
        if (settime(t, 0, 100 * MS, 0) != 0) _exit(6);
        execl("/proc/self/exe", self, "exec-child", t_text, (char *)0);
        execl(self, self, "exec-child", t_text, (char *)0);
        _exit(5);
    }
    status = wait_child(pid);
    if (status != 0) {
        fprintf(stderr, "timerfd-share: exec child exit %d\n", status);
        errno = 0;
        return fail(14, "exec image");
    }
    close(t);
    return 0;
}

struct blocked { int fd; ssize_t got; int error; int64_t done; };

static void *blocked_reader(void *argument)
{
    struct blocked *blocked = argument;
    uint64_t value;
    errno = 0;
    blocked->got = read(blocked->fd, &value, sizeof(value));
    blocked->error = errno;
    blocked->done = mono();
    return 0;
}

static int check_close_blocked(void)
{
    struct blocked blocked;
    pthread_t thread;
    int64_t closed;
    int fds[64], round, i;
    blocked.fd = tfd_create(CLOCK_MONOTONIC, 0);
    if (blocked.fd < 0) return fail(10, "timerfd_create");
    if (pthread_create(&thread, 0, blocked_reader, &blocked) != 0)
        return fail(10, "pthread_create");
    sleep_ms(100);
    closed = mono();
    close(blocked.fd);
    pthread_join(thread, 0);
    if (blocked.got != -1 || blocked.error != EBADF ||
            blocked.done - closed > 1100 * MS) {
        fprintf(stderr, "got %d errno %d %lld ms after the close\n",
            (int)blocked.got, blocked.error,
            (long long)((blocked.done - closed) / MS));
        errno = blocked.error;
        return fail(15, "blocked read not EBADF within 1 s of close");
    }
    for (round = 0; round < 4; ++round) {
        for (i = 0; i < 64; ++i) {
            fds[i] = tfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK_FLAG);
            if (fds[i] < 0) return fail(15, "timerfd_create after close");
            if (settime(fds[i], 0, SECOND, 10 * MS) != 0)
                return fail(15, "settime after close");
        }
        for (i = 0; i < 64; ++i) close(fds[i]);
    }
    return 0;
}

static int send_fd(int socket_fd, int fd)
{
    char byte = 'm';
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; }
        control;
    struct iovec iov = { &byte, 1 };
    struct msghdr message;
    struct cmsghdr *header;
    memset(&message, 0, sizeof(message));
    memset(&control, 0, sizeof(control));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = CMSG_SPACE(sizeof(int));
    header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &fd, sizeof(int));
    return sendmsg(socket_fd, &message, 0) == 1 ? 0 : -1;
}

static int receive_fd(int socket_fd)
{
    char byte;
    union { struct cmsghdr align; char bytes[CMSG_SPACE(4 * sizeof(int))]; }
        control;
    struct iovec iov = { &byte, 1 };
    struct msghdr message;
    struct cmsghdr *header;
    int fd;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    if (recvmsg(socket_fd, &message, 0) != 1) return -1;
    header = CMSG_FIRSTHDR(&message);
    if (header == 0 || header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len != CMSG_LEN(sizeof(int))) return -1;
    memcpy(&fd, CMSG_DATA(header), sizeof(int));
    return fd;
}

static int rights_child(int socket_fd, int t)
{
    struct pollfd entry;
    uint64_t value = 0;
    int received;
    child_bound();
    close(t);
    received = receive_fd(socket_fd);
    if (received < 0) return 2;
    if (write(socket_fd, "r", 1) != 1) return 3;
    entry.fd = received;
    entry.events = POLLIN;
    entry.revents = 0;
    if (poll(&entry, 1, 2000) != 1 || (entry.revents & POLLIN) == 0)
        return 4;
    if (read_count(received, &value) != 1) return 5;
    return value == 1 ? 0 : 6;
}

static int check_rights(void)
{
    int t = tfd_create(CLOCK_MONOTONIC, 0), pair[2], status;
    char ready = 0;
    pid_t pid;
    if (t < 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        return fail(10, "timerfd_create or socketpair");
    pid = fork();
    if (pid < 0) return fail(10, "fork");
    if (pid == 0) {
        close(pair[0]);
        _exit(rights_child(pair[1], t));
    }
    close(pair[1]);
    if (send_fd(pair[0], t) != 0) return fail(16, "sendmsg");
    if (read(pair[0], &ready, 1) != 1 || ready != 'r')
        return fail(16, "child never received");
    if (settime(t, 0, 30 * MS, 0) != 0) return fail(16, "settime");
    status = wait_child(pid);
    if (status != 0) {
        fprintf(stderr, "timerfd-share: rights child exit %d\n", status);
        errno = 0;
        return fail(16, "received timerfd did not deliver");
    }
    close(t);
    close(pair[0]);
    return 0;
}

static void *cancel_reader(void *argument)
{
    return blocked_reader(argument);
}

static int check_cancel_on_set(void)
{
    struct blocked blocked;
    struct timespec moved;
    pthread_t thread;
    uint64_t value;
    int64_t left, interval, real_now, changed;
    int r = tfd_create(CLOCK_REALTIME, TFD_NONBLOCK_FLAG);
    int m = tfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK_FLAG);
    int relative = tfd_create(CLOCK_REALTIME, TFD_NONBLOCK_FLAG);
    if (r < 0 || m < 0 || relative < 0) return fail(10, "timerfd_create");
    if (settime(r, TFD_ABSTIME | TFD_CANCEL_ON_SET,
            now_ns(CLOCK_REALTIME) + 10 * SECOND, 0) != 0)
        return fail(17, "absolute realtime CANCEL_ON_SET refused");
    if (settime(relative, TFD_CANCEL_ON_SET, 10 * SECOND, 0) != 0)
        return fail(17, "relative CANCEL_ON_SET refused (Linux ignores it)");
    if (settime(m, TFD_ABSTIME | TFD_CANCEL_ON_SET,
            mono() + 10 * SECOND, 0) != 0)
        return fail(17, "monotonic CANCEL_ON_SET refused (Linux ignores "
            "it)");
    errno = 0;
    if (settime(r, TFD_ABSTIME | TFD_CANCEL_ON_SET | 4,
            now_ns(CLOCK_REALTIME) + 10 * SECOND, 0) != -1 ||
            errno != EINVAL)
        return fail(17, "unknown settime flag 4 not EINVAL");
    if (settime(r, TFD_ABSTIME | TFD_CANCEL_ON_SET,
            now_ns(CLOCK_REALTIME) + 10 * SECOND, 0) != 0)
        return fail(17, "re-arm with CANCEL_ON_SET");
    sleep_ms(100);
    if (read_count(r, &value) != 0 || read_count(relative, &value) != 0 ||
            read_count(m, &value) != 0)
        return fail(17, "read within 100 ms not EAGAIN (false ECANCELED?)");
    if (gettime(r, &left, &interval) != 0 || left < 9 * SECOND ||
            interval != 0)
        return fail(17, "gettime of the cancelable timer");

    /* A real clock change, when this user may make one. */
    real_now = now_ns(CLOCK_REALTIME);
    moved.tv_sec = (time_t)((real_now + 2 * SECOND) / SECOND);
    moved.tv_nsec = (long)((real_now + 2 * SECOND) % SECOND);
    blocked.fd = tfd_create(CLOCK_REALTIME, 0);
    if (blocked.fd < 0) return fail(10, "timerfd_create blocking");
    if (settime(blocked.fd, TFD_ABSTIME | TFD_CANCEL_ON_SET,
            real_now + 30 * SECOND, 0) != 0)
        return fail(17, "arm the blocking cancelable timer");
    errno = 0;
    if (clock_settime(CLOCK_REALTIME, &moved) != 0) {
        if (errno != EPERM) return fail(17, "clock_settime not EPERM");
        fprintf(stderr, "timerfd-share 17: real clock change skipped: "
            "clock_settime(CLOCK_REALTIME) fails EPERM for this user\n");
        close(blocked.fd);
        close(r);
        close(m);
        close(relative);
        return 0;
    }
    /* Allowed (the clock is now 2 s ahead): the nonblocking cancelable
       timer reports it once; then a blocked read sees the move back. */
    if (read_count(r, &value) != -1 || errno != ECANCELED)
        return fail(17, "nonblocking read after a clock set not ECANCELED");
    if (read_count(r, &value) != 0)
        return fail(17, "second read after ECANCELED not EAGAIN");
    if (settime(blocked.fd, TFD_ABSTIME | TFD_CANCEL_ON_SET,
            now_ns(CLOCK_REALTIME) + 30 * SECOND, 0) != 0)
        return fail(17, "re-arm the blocking cancelable timer");
    if (pthread_create(&thread, 0, cancel_reader, &blocked) != 0)
        return fail(10, "pthread_create");
    sleep_ms(100);
    real_now = now_ns(CLOCK_REALTIME);
    moved.tv_sec = (time_t)((real_now - 2 * SECOND) / SECOND);
    moved.tv_nsec = (long)((real_now - 2 * SECOND) % SECOND);
    changed = mono();
    if (clock_settime(CLOCK_REALTIME, &moved) != 0)
        return fail(17, "clock_settime moving the clock back");
    pthread_join(thread, 0);
    if (blocked.got != -1 || blocked.error != ECANCELED ||
            blocked.done - changed > 1500 * MS) {
        errno = blocked.error;
        return fail(17, "blocked read not ECANCELED after the clock set");
    }
    close(blocked.fd);
    close(r);
    close(m);
    close(relative);
    return 0;
}

static int check_many(void)
{
    enum { COUNT = 50 };
    int fds[COUNT], i;
    int64_t started[COUNT], period;
    for (i = 0; i < COUNT; ++i) {
        fds[i] = tfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK_FLAG);
        if (fds[i] < 0) return fail(10, "timerfd_create");
    }
    for (i = 0; i < COUNT; ++i) {
        period = (5 + i) * MS;
        started[i] = mono();
        if (settime(fds[i], 0, period, period) != 0)
            return fail(10, "settime");
    }
    sleep_ms(500);
    for (i = 0; i < COUNT; ++i) {
        uint64_t value;
        int64_t expected, tolerance, at;
        period = (5 + i) * MS;
        at = mono();
        if (read_count(fds[i], &value) < 0) return fail(18, "read");
        expected = (at - started[i]) / period;
        tolerance = expected / 10 > 1 ? expected / 10 : 1;
        if ((int64_t)value < expected - tolerance ||
                (int64_t)value > expected + tolerance) {
            fprintf(stderr, "timer %d (period %d ms): %llu, expected %lld\n",
                i, 5 + i, (unsigned long long)value, (long long)expected);
            errno = 0;
            return fail(18, "count off by more than 10 %");
        }
    }
    for (i = 0; i < COUNT; ++i) close(fds[i]);
    return 0;
}

static int check_clocks(void)
{
    struct timespec value;
    int64_t boot0, mono0, boot1, mono1, drift;
    if (clock_gettime(CLOCK_BOOTTIME_ID, &value) != 0)
        return fail(19, "clock_gettime(CLOCK_BOOTTIME)");
    if (clock_gettime(CLOCK_REALTIME_ALARM_ID, &value) != 0)
        return fail(19, "clock_gettime(CLOCK_REALTIME_ALARM)");
    if (clock_gettime(CLOCK_BOOTTIME_ALARM_ID, &value) != 0)
        return fail(19, "clock_gettime(CLOCK_BOOTTIME_ALARM)");
    if (clock_getres(CLOCK_BOOTTIME_ID, &value) != 0)
        return fail(19, "clock_getres(CLOCK_BOOTTIME)");
    boot0 = now_ns(CLOCK_BOOTTIME_ID);
    mono0 = mono();
    sleep_ms(50);
    boot1 = now_ns(CLOCK_BOOTTIME_ID);
    mono1 = mono();
    drift = (boot1 - boot0) - (mono1 - mono0);
    if (drift < 0) drift = -drift;
    if (boot1 - boot0 < 45 * MS || drift > 5 * MS) {
        fprintf(stderr, "boottime moved %lld ms, monotonic %lld ms\n",
            (long long)((boot1 - boot0) / MS),
            (long long)((mono1 - mono0) / MS));
        errno = 0;
        return fail(19, "CLOCK_BOOTTIME does not move with CLOCK_MONOTONIC");
    }
    return 0;
}

static int run_check(int check, const char *self)
{
    arm(check);
    switch (check) {
    case 11: return check_blocking_read();
    case 12: return check_signals();
    case 13: {
        int result = check_fork();
        return result != 0 ? result : check_fork_spin();
    }
    case 14: return check_exec(self);
    case 15: return check_close_blocked();
    case 16: return check_rights();
    case 17: return check_cancel_on_set();
    case 18: return check_many();
    case 19: return check_clocks();
    default: errno = EINVAL; return fail(10, "no such check");
    }
}

int main(int argc, char **argv)
{
    int check, result;
    if (argc == 3 && strcmp(argv[1], "exec-child") == 0)
        return exec_child(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--only") == 0 &&
            strcmp(argv[2], "13-spin") == 0) {
        arm(13);
        return check_fork_spin();
    }
    if (argc == 3 && strcmp(argv[1], "--only") == 0)
        return run_check(atoi(argv[2]), argv[0]);
    for (check = 11; check <= 19; ++check)
        if ((result = run_check(check, argv[0])) != 0) return result;
    return 0;
}
