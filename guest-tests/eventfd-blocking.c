/*
 * Blocking eventfd I/O, signals and write readiness (guest fixture, musl).
 *
 * Checks (the exit code is the check number, also printed on stderr with
 * errno; a check that hangs is ended by the watchdog thread with exit code
 * 100 + the check number):
 *   10 setup failed
 *   11 a blocking read in thread B returns 9 after thread A writes 9
 *      100 ms later
 *   12 a blocking write of 2 at max - 1 in thread B completes after thread
 *      A reads; the counter is then 2
 *   13 thread B blocks in read; a SIGUSR1 with an SA_RESTART handler
 *      arrives (tgkill to B); B's read keeps waiting and returns the value
 *      A writes afterwards; the handler ran once
 *   14 the same without SA_RESTART: the read fails EINTR
 *   15 poll for POLLOUT on a fresh eventfd reports POLLOUT at once; at the
 *      maximum (0xfffffffffffffffe) it doesn't, and POLLIN is reported
 *   16 select with the eventfd in the write set reports it writable
 *   17 a forked child blocks in read and is SIGKILLed while blocked; the
 *      parent's write and read still work afterwards
 *   18 200 threads block in reads of one EFD_SEMAPHORE eventfd at once;
 *      none fails (EMFILE included: a two-fd signal-wait pipe per blocked
 *      thread ran the emulator's 256 hidden fds out at 127 threads); one
 *      write of 200 then completes every read with 1
 *      (EVENTFD_BLOCKING_THREADS=N changes the count)
 *
 * Not checked: a counter above 0 with an empty readiness channel (left by
 * a process killed inside the few instructions between draining the
 * channel and re-raising it).  poll reports POLLIN from the counter in
 * that state, but the guest has no deliberate way to create it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define COUNTER_MAX 0xfffffffffffffffeULL

static volatile int current_check;
static volatile int watchdog_ticks;     /* 100 ms ticks since arm() */

static int fail(int check, const char *what)
{
    fprintf(stderr, "eventfd-blocking check %d failed: %s (errno %d %s)\n",
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

/* Each check arms the watchdog for itself, so a regression that blocks
   forever ends the run (exit 100 + check) instead of hanging it: BerryCore
   timeout never fires on the device. */
static void *watchdog(void *argument)
{
    (void)argument;
    for (;;) {
        sleep_ms(100);
        if (__sync_add_and_fetch(&watchdog_ticks, 1) >= 100) {
            fprintf(stderr, "eventfd-blocking check %d timed out\n",
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

static int read_value(int fd, uint64_t *value)
{
    return read(fd, value, sizeof(*value)) == (ssize_t)sizeof(*value);
}

static int write_value(int fd, uint64_t value)
{
    return write(fd, &value, sizeof(value)) == (ssize_t)sizeof(value);
}

struct blocked_io {
    int fd;
    uint64_t value;
    volatile pid_t tid;
    volatile int done;
    ssize_t result;
    int error;
};

static void *blocking_reader(void *argument)
{
    struct blocked_io *io = argument;
    io->tid = (pid_t)syscall(SYS_gettid);
    errno = 0;
    io->result = read(io->fd, &io->value, sizeof(io->value));
    io->error = errno;
    __sync_synchronize();
    io->done = 1;
    return 0;
}

static void *blocking_writer(void *argument)
{
    struct blocked_io *io = argument;
    io->tid = (pid_t)syscall(SYS_gettid);
    errno = 0;
    io->result = write(io->fd, &io->value, sizeof(io->value));
    io->error = errno;
    __sync_synchronize();
    io->done = 1;
    return 0;
}

static int check_read(void)
{
    struct blocked_io io;
    pthread_t thread;
    int e = eventfd(0, 0);
    if (e < 0) return fail(10, "eventfd(0, 0)");
    memset(&io, 0, sizeof(io));
    io.fd = e;
    if (pthread_create(&thread, 0, blocking_reader, &io) != 0)
        return fail(10, "pthread_create");
    sleep_ms(100);
    if (io.done) {
        errno = io.error;
        pthread_join(thread, 0);
        return fail(11, "read on an empty blocking eventfd returned");
    }
    if (!write_value(e, 9)) return fail(11, "write of 9");
    pthread_join(thread, 0);
    errno = io.error;
    if (io.result != 8 || io.value != 9)
        return fail(11, "blocked read did not return 9");
    close(e);
    return 0;
}

static int check_write(void)
{
    struct blocked_io io;
    pthread_t thread;
    uint64_t value = 0;
    int e = eventfd(0, 0);
    if (e < 0) return fail(10, "eventfd(0, 0)");
    if (!write_value(e, COUNTER_MAX - 1)) return fail(12, "write of max - 1");
    memset(&io, 0, sizeof(io));
    io.fd = e;
    io.value = 2;
    if (pthread_create(&thread, 0, blocking_writer, &io) != 0)
        return fail(10, "pthread_create");
    sleep_ms(100);
    if (io.done) {
        errno = io.error;
        pthread_join(thread, 0);
        return fail(12, "write past the maximum returned");
    }
    if (!read_value(e, &value) || value != COUNTER_MAX - 1)
        return fail(12, "read of max - 1");
    pthread_join(thread, 0);
    errno = io.error;
    if (io.result != 8) return fail(12, "blocked write failed");
    if (!read_value(e, &value) || value != 2)
        return fail(12, "counter after the blocked write is not 2");
    close(e);
    return 0;
}

static volatile int handler_runs;

static void on_usr1(int signal_number)
{
    (void)signal_number;
    __sync_fetch_and_add(&handler_runs, 1);
}

static int check_signal(int check, int restart)
{
    struct blocked_io io;
    struct sigaction action;
    pthread_t thread;
    int e = eventfd(0, 0);
    if (e < 0) return fail(10, "eventfd(0, 0)");
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_usr1;
    action.sa_flags = restart ? SA_RESTART : 0;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, 0) != 0) return fail(10, "sigaction");
    handler_runs = 0;
    memset(&io, 0, sizeof(io));
    io.fd = e;
    if (pthread_create(&thread, 0, blocking_reader, &io) != 0)
        return fail(10, "pthread_create");
    sleep_ms(100);
    if (io.done || io.tid == 0) {
        errno = io.error;
        pthread_join(thread, 0);
        return fail(check, "read on an empty blocking eventfd returned");
    }
    if (syscall(SYS_tgkill, getpid(), io.tid, SIGUSR1) != 0)
        return fail(10, "tgkill");
    sleep_ms(150);
    if (handler_runs != 1) {
        pthread_join(thread, 0);
        return fail(check, "the handler did not run exactly once");
    }
    if (restart) {
        if (io.done) {
            errno = io.error;
            pthread_join(thread, 0);
            return fail(check, "SA_RESTART read returned after the signal");
        }
        if (!write_value(e, 5)) return fail(check, "write of 5");
        pthread_join(thread, 0);
        errno = io.error;
        if (io.result != 8 || io.value != 5)
            return fail(check, "restarted read did not return 5");
    } else {
        pthread_join(thread, 0);
        errno = io.error;
        if (io.result != -1 || io.error != EINTR)
            return fail(check, "interrupted read did not fail EINTR");
    }
    if (handler_runs != 1)
        return fail(check, "the handler ran more than once");
    signal(SIGUSR1, SIG_DFL);
    close(e);
    return 0;
}

static int check_pollout(void)
{
    struct pollfd entry;
    int e = eventfd(0, EFD_NONBLOCK), n;
    if (e < 0) return fail(10, "eventfd(0, EFD_NONBLOCK)");
    entry.fd = e;
    entry.events = POLLOUT;
    entry.revents = 0;
    n = poll(&entry, 1, 0);
    if (n != 1 || (entry.revents & POLLOUT) == 0)
        return fail(15, "fresh eventfd not POLLOUT");
    if (!write_value(e, COUNTER_MAX)) return fail(15, "write of the maximum");
    entry.revents = 0;
    n = poll(&entry, 1, 0);
    if (n != 0 || entry.revents != 0)
        return fail(15, "eventfd at the maximum reported POLLOUT");
    entry.events = POLLIN | POLLOUT;
    entry.revents = 0;
    n = poll(&entry, 1, 0);
    if (n != 1 || (entry.revents & POLLIN) == 0 ||
            (entry.revents & POLLOUT) != 0) {
        fprintf(stderr, "eventfd-blocking revents 0x%x\n", entry.revents);
        return fail(15, "eventfd at the maximum not POLLIN without POLLOUT");
    }
    close(e);
    return 0;
}

static int check_select(void)
{
    fd_set writable;
    struct timeval zero;
    int e = eventfd(0, EFD_NONBLOCK), n;
    if (e < 0) return fail(10, "eventfd(0, EFD_NONBLOCK)");
    FD_ZERO(&writable);
    FD_SET(e, &writable);
    zero.tv_sec = 0;
    zero.tv_usec = 0;
    n = select(e + 1, 0, &writable, 0, &zero);
    if (n != 1 || !FD_ISSET(e, &writable))
        return fail(16, "select did not report the eventfd writable");
    close(e);
    return 0;
}

static int check_fork_kill(void)
{
    uint64_t value = 0;
    int e = eventfd(0, 0), status = 0, i;
    pid_t child, waited = 0;
    if (e < 0) return fail(10, "eventfd(0, 0)");
    child = fork();
    if (child < 0) return fail(10, "fork");
    if (child == 0) {
        /* Bounded on its own: the watchdog thread isn't copied. */
        alarm(10);
        if (read_value(e, &value)) _exit(3);
        _exit(4);
    }
    sleep_ms(200);
    if (waitpid(child, &status, WNOHANG) != 0) {
        errno = 0;
        return fail(17, "the child's read did not block");
    }
    if (kill(child, SIGKILL) != 0) return fail(17, "kill");
    for (i = 0; i < 100 && waited == 0; ++i) {
        waited = waitpid(child, &status, WNOHANG);
        if (waited == 0) sleep_ms(50);
    }
    if (waited != child || !WIFSIGNALED(status) ||
            WTERMSIG(status) != SIGKILL)
        return fail(17, "the child did not die of SIGKILL");
    if (!write_value(e, 4)) return fail(17, "parent write after the kill");
    if (!read_value(e, &value) || value != 4)
        return fail(17, "parent read after the kill");
    close(e);
    return 0;
}

#define MANY_READERS 200

static int check_many_readers(void)
{
    static struct blocked_io io[1024];
    static pthread_t threads[1024];
    pthread_attr_t attributes;
    const char *text = getenv("EVENTFD_BLOCKING_THREADS");
    int count = text != 0 ? atoi(text) : MANY_READERS, started = 0, i;
    int e = eventfd(0, EFD_SEMAPHORE), result = 0;
    if (count <= 0 || count > 1024) count = MANY_READERS;
    if (e < 0) return fail(10, "eventfd(0, EFD_SEMAPHORE)");
    if (pthread_attr_init(&attributes) != 0 ||
            pthread_attr_setstacksize(&attributes, 64 * 1024) != 0)
        return fail(10, "pthread_attr");
    memset(io, 0, sizeof(io));
    for (i = 0; i != count; ++i) {
        io[i].fd = e;
        if (pthread_create(&threads[i], &attributes, blocking_reader,
                &io[i]) != 0) {
            result = fail(10, "pthread_create");
            break;
        }
        ++started;
    }
    pthread_attr_destroy(&attributes);
    /* Every reader is in its read by now (or failed out of it). */
    for (i = 0; i != 20; ++i) sleep_ms(50);
    for (i = 0; i != started && result == 0; ++i)
        if (io[i].done) {
            errno = io[i].error;
            fprintf(stderr, "eventfd-blocking reader %d of %d returned "
                "%ld\n", i, started, (long)io[i].result);
            result = fail(18, "a read on an empty blocking eventfd returned");
        }
    if (!write_value(e, (uint64_t)count) && result == 0)
        result = fail(18, "write of the reader count");
    for (i = 0; i != started; ++i) pthread_join(threads[i], 0);
    for (i = 0; i != started && result == 0; ++i)
        if (io[i].result != 8 || io[i].value != 1) {
            errno = io[i].error;
            result = fail(18, "a blocked read did not return 1");
        }
    close(e);
    return result;
}

int main(void)
{
    int result;
    arm(11);
    if ((result = check_read()) != 0) return result;
    arm(12);
    if ((result = check_write()) != 0) return result;
    arm(13);
    if ((result = check_signal(13, 1)) != 0) return result;
    arm(14);
    if ((result = check_signal(14, 0)) != 0) return result;
    arm(15);
    if ((result = check_pollout()) != 0) return result;
    arm(16);
    if ((result = check_select()) != 0) return result;
    arm(17);
    if ((result = check_fork_kill()) != 0) return result;
    arm(18);
    if ((result = check_many_readers()) != 0) return result;
    return 0;
}
