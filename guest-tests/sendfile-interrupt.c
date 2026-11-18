/*
 * A signal arriving mid-transfer (guest fixture, musl).
 *
 * Checks (the exit code is the check number, also printed on stderr):
 *   10 setup failed
 *   11 sendfile into a full pipe with an SA_RESTART handler and a signal from a forked child
 *      did not return a positive partial count after the signal fired
 *   12 the offset or the pipe contents do not match the partial count
 *   13 preadv2 (offset -1) on an empty pipe without SA_RESTART did not
 *      fail with EINTR
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_preadv2
#define SYS_preadv2 392
#endif

#define TOTAL (4 * 1024 * 1024)

static volatile int fired;

static void on_alarm(int signal_number)
{
    (void)signal_number;
    fired++;
}

static int fail(int check, const char *what)
{
    fprintf(stderr, "sendfile-interrupt check %d failed: %s (errno %d)\n",
        check, what, errno);
    return check;
}

/* alarm() needs setitimer, which comes later; a forked child signals the
   parent after one second instead. */
static pid_t arm_signal(void)
{
    pid_t child = fork();
    if (child == 0) {
        struct timespec delay = { 1, 0 };
        nanosleep(&delay, 0);
        kill(getppid(), SIGALRM);
        _exit(0);
    }
    return child;
}

static void install(int flags)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_alarm;
    action.sa_flags = flags;
    sigemptyset(&action.sa_mask);
    sigaction(SIGALRM, &action, 0);
}

int main(void)
{
    static unsigned char source[TOTAL];
    static unsigned char sink[TOTAL];
    char path[] = "/tmp/linuxemu-sendfile-XXXXXX";
    int file, pipefd[2], empty[2];
    off_t offset = 0;
    size_t drained = 0;
    ssize_t result;
    size_t i;
    char byte;
    struct iovec vector;
    pid_t child;

    for (i = 0; i != TOTAL; ++i) source[i] = (unsigned char)(i * 7 + (i >> 8));
    file = mkstemp(path);
    if (file < 0 || write(file, source, TOTAL) != TOTAL ||
            pipe(pipefd) != 0 || pipe(empty) != 0)
        return fail(10, "setup");
    unlink(path);

    /* 11: the pipe fills, the signal interrupts the blocked write */
    install(SA_RESTART);
    child = arm_signal();
    result = sendfile(pipefd[1], file, &offset, TOTAL);
    if (result <= 0 || fired == 0 || result >= TOTAL)
        return fail(11, "no partial count after the signal");

    /* 12: offset and pipe contents agree with the count */
    if (offset != result) return fail(12, "offset != partial count");
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    while (drained < (size_t)result) {
        ssize_t got = read(pipefd[0], sink + drained, (size_t)result - drained);
        if (got <= 0) break;
        drained += (size_t)got;
    }
    if (drained != (size_t)result || memcmp(sink, source, drained) != 0)
        return fail(12, "pipe contents differ from the source");

    /* 13: preadv2 offset -1 on an empty pipe, no SA_RESTART -> EINTR */
    vector.iov_base = &byte;
    vector.iov_len = 1;
    install(0);
    fired = 0;
    waitpid(child, 0, 0);
    child = arm_signal();
    result = syscall(SYS_preadv2, empty[0], &vector, 1, -1, -1, 0);
    if (result != -1 || errno != EINTR)
        return fail(13, "preadv2 on an empty pipe was not interrupted");
    waitpid(child, 0, 0);
    return 0;
}
