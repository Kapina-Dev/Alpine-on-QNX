/*
 * dup3 racing close on one destination number (guest fixture, musl).
 *
 * Thread A dup3()s a pipe end onto fd 100 with O_CLOEXEC; thread B closes
 * fd 100 and opens /dev/null, 20000 times each.  Afterwards fd 100 is
 * either closed or a valid descriptor (11), and a wake eventfd created
 * before the race still reads and writes (12).  Exit 0, or the check
 * number, which is also printed on stderr.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define ITERATIONS 20000
#define TARGET 100

static int source_fd;

static int fail(int check, const char *what)
{
    fprintf(stderr, "dup3-race check %d failed: %s\n", check, what);
    return check;
}

static void *dup_thread(void *argument)
{
    int i;
    (void)argument;
    for (i = 0; i < ITERATIONS; ++i) (void)dup3(source_fd, TARGET, O_CLOEXEC);
    return NULL;
}

static void *close_thread(void *argument)
{
    int i;
    (void)argument;
    for (i = 0; i < ITERATIONS; ++i) {
        int fd;
        (void)close(TARGET);
        fd = open("/dev/null", O_RDONLY);
        if (fd >= 0 && fd != TARGET) (void)close(fd);
    }
    return NULL;
}

int main(void)
{
    int pipe_fds[2];
    int wake;
    uint64_t value = 1;
    pthread_t first, second;

    if (pipe(pipe_fds) != 0) return fail(10, "pipe");
    source_fd = pipe_fds[0];
    wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake < 0) return fail(10, "eventfd");

    if (pthread_create(&first, NULL, dup_thread, NULL) != 0 ||
            pthread_create(&second, NULL, close_thread, NULL) != 0)
        return fail(10, "pthread_create");
    pthread_join(first, NULL);
    pthread_join(second, NULL);

    if (fcntl(TARGET, F_GETFD) < 0 && errno != EBADF)
        return fail(11, "fd 100 neither closed nor valid");

    if (write(wake, &value, sizeof(value)) != (ssize_t)sizeof(value))
        return fail(12, "wake eventfd write");
    value = 0;
    if (read(wake, &value, sizeof(value)) != (ssize_t)sizeof(value) ||
            value != 1)
        return fail(12, "wake eventfd read");
    return 0;
}
