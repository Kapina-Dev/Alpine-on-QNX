/*
 * dup3 racing close and eventfd re-creation on one destination number
 * (guest fixture, musl).
 *
 * Before the race a wake eventfd sits at fd 100, so the destination has a
 * typed record and a hidden write end.  Thread A dup3()s a pipe read end
 * onto 100 with O_CLOEXEC; thread B alternates between closing 100 and
 * re-creating a wake eventfd there (eventfd, dup3 onto 100, close the
 * original), 20000 iterations each.
 *
 * Checks (the exit code is the check number, also printed on stderr):
 *   10 setup failed
 *   11 a dup3 in thread A did not return exactly 100
 *   12 a dup3 in thread B did not return exactly 100
 *   13 fd 100 is open but behaves like neither the pipe nor a wake
 *      eventfd, or like a mix of the two
 *   14 creating and closing wake eventfds failed: hidden write ends leaked
 *   15 a wake eventfd created before the race no longer reads and writes
 *   16 a fork child hung or failed: a thread churning close/dup3 on fd 100
 *      while main forks 200 times; each child does dup2 and close on 100
 *      and exits 0.  A child that inherits a locked emulator lock hangs,
 *      so the parent bounds each wait to about 10 s and kills it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ITERATIONS 20000
#define TARGET 100
#define LEAK_LOOPS 1000
#define FORKS 200
#define CHILD_WAIT_MS 10000

static int source_fd;
static volatile int a_failures, a_errno, a_result;
static volatile int b_failures, b_errno, b_result;

static int fail(int check, const char *what)
{
    fprintf(stderr, "dup3-race check %d failed: %s\n", check, what);
    return check;
}

static void *dup_thread(void *argument)
{
    int i;
    (void)argument;
    for (i = 0; i < ITERATIONS; ++i) {
        int result = dup3(source_fd, TARGET, O_CLOEXEC);
        if (result != TARGET) {
            if (__sync_fetch_and_add(&a_failures, 1) == 0) {
                a_errno = errno;
                a_result = result;
            }
        }
    }
    return NULL;
}

static void *churn_thread(void *argument)
{
    int i;
    (void)argument;
    for (i = 0; i < ITERATIONS; ++i) {
        if ((i & 1) == 0) {
            (void)close(TARGET);
        } else {
            int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (wake >= 0 && wake != TARGET) {
                int result = dup3(wake, TARGET, 0);
                if (result != TARGET) {
                    if (__sync_fetch_and_add(&b_failures, 1) == 0) {
                        b_errno = errno;
                        b_result = result;
                    }
                }
                (void)close(wake);
            }
        }
    }
    return NULL;
}

/* Returns 0 if fd 100 is closed or exactly one of the two objects. */
static int check_target(int pipe_write, int pipe_read)
{
    unsigned char byte = 0x5a, got = 0;
    uint64_t value = 1, readback = 0;
    ssize_t count;

    if (fcntl(TARGET, F_GETFD) < 0)
        return errno == EBADF ? 0 : -9;
    if (write(pipe_write, &byte, 1) != 1) return -1;
    count = read(TARGET, &readback, sizeof(readback));
    if (count == 1 || (count > 0 && count != (ssize_t)sizeof(readback))) {
        /* the pipe: the byte arrived through 100, and 100 is read-only */
        unsigned char *bytes = (unsigned char *)&readback;
        if (count != 1 || bytes[0] != byte) return -2;
        if (write(TARGET, &value, sizeof(value)) >= 0) return -3;
        return 0;
    }
    /* not the pipe: our byte is still queued, and 100 must be a wake
       eventfd that reads empty and round-trips a value */
    if (count >= 0 || errno != EAGAIN) return -4;
    if (read(pipe_read, &got, 1) != 1 || got != byte) return -5;
    if (write(TARGET, &value, sizeof(value)) != (ssize_t)sizeof(value))
        return -6;
    if (read(TARGET, &readback, sizeof(readback)) != (ssize_t)sizeof(readback)
            || readback != value)
        return -7;
    return 0;
}

static volatile int churn_stop;

static void *fork_churn_thread(void *argument)
{
    (void)argument;
    while (!churn_stop) {
        int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        (void)close(TARGET);
        (void)dup3(source_fd, TARGET, O_CLOEXEC);
        if (wake >= 0 && wake != TARGET) {
            (void)dup3(wake, TARGET, 0);
            (void)close(wake);
        }
    }
    return NULL;
}

/* Returns 0, or -1 on a hung or failed child (the hung one is killed). */
static int fork_phase(void)
{
    pthread_t churn;
    int i, status = 0, bad = 0;

    churn_stop = 0;
    if (pthread_create(&churn, NULL, fork_churn_thread, NULL) != 0) return -1;
    for (i = 0; i < FORKS && !bad; ++i) {
        pid_t child = fork();
        int waited = 0, elapsed;
        if (child < 0) { bad = 1; break; }
        if (child == 0) {
            int ok = dup2(source_fd, TARGET) == TARGET;
            (void)close(TARGET);
            _exit(ok ? 0 : 1);
        }
        for (elapsed = 0; elapsed < CHILD_WAIT_MS; ++elapsed) {
            struct timespec pause = { 0, 1000000 };
            waited = waitpid(child, &status, WNOHANG);
            if (waited != 0) break;
            nanosleep(&pause, NULL);
        }
        if (waited == 0) {
            fprintf(stderr, "fork child %d hung at iteration %d\n",
                (int)child, i);
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            bad = 1;
        } else if (waited < 0 || !WIFEXITED(status) ||
                WEXITSTATUS(status) != 0) {
            fprintf(stderr, "fork child failed at iteration %d status=%d\n",
                i, status);
            bad = 1;
        }
    }
    churn_stop = 1;
    pthread_join(churn, NULL);
    return bad ? -1 : 0;
}

int main(void)
{
    int pipe_fds[2];
    int wake, first_wake, i, verdict;
    uint64_t value = 1;
    pthread_t first, second;

    if (pipe(pipe_fds) != 0) return fail(10, "pipe");
    if (fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK) != 0) return fail(10, "fcntl");
    source_fd = pipe_fds[0];
    wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake < 0) return fail(10, "eventfd");
    first_wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (first_wake < 0 || first_wake == TARGET ||
            dup3(first_wake, TARGET, 0) != TARGET || close(first_wake) != 0)
        return fail(10, "eventfd onto fd 100");

    if (pthread_create(&first, NULL, dup_thread, NULL) != 0 ||
            pthread_create(&second, NULL, churn_thread, NULL) != 0)
        return fail(10, "pthread_create");
    pthread_join(first, NULL);
    pthread_join(second, NULL);

    if (a_failures != 0) {
        fprintf(stderr, "thread A: %d dup3 failures, first result=%d "
            "errno=%d\n", a_failures, a_result, a_errno);
        return fail(11, "dup3 did not return 100");
    }
    if (b_failures != 0) {
        fprintf(stderr, "thread B: %d dup3 failures, first result=%d "
            "errno=%d\n", b_failures, b_result, b_errno);
        return fail(12, "dup3 did not return 100");
    }
    verdict = check_target(pipe_fds[1], pipe_fds[0]);
    if (verdict != 0) {
        fprintf(stderr, "check_target reason=%d errno=%d\n", -verdict, errno);
        return fail(13, "fd 100 is a mix of two objects or unusable");
    }

    (void)close(TARGET);
    for (i = 0; i < LEAK_LOOPS; ++i) {
        int probe = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (probe < 0) {
            fprintf(stderr, "eventfd failed at loop %d errno=%d\n", i, errno);
            return fail(14, "hidden write ends leaked");
        }
        (void)close(probe);
    }

    if (fork_phase() != 0) return fail(16, "fork child hung or failed");
    (void)close(TARGET);

    if (write(wake, &value, sizeof(value)) != (ssize_t)sizeof(value))
        return fail(15, "wake eventfd write");
    value = 0;
    if (read(wake, &value, sizeof(value)) != (ssize_t)sizeof(value) ||
            value != 1)
        return fail(15, "wake eventfd read");
    return 0;
}
