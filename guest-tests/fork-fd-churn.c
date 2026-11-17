/*
 * fork() while another thread opens and closes fds (guest fixture, musl).
 *
 * Native QNX fork() fails with EBADF when a descriptor is closed while it
 * duplicates the fd table; Linux fork() never does.  Git's HTTPS fetch hits
 * this (an async thread closes pipe ends while the main thread forks
 * index-pack).  Every fork here must succeed: exit 2 reports a failure.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#define FORKS 500

static volatile int stop;

static void *churn(void *argument)
{
    int p[2];
    (void)argument;
    while (!stop)
        if (pipe(p) == 0) { close(p[0]); close(p[1]); }
    return 0;
}

int main(void)
{
    pthread_t thread;
    int i, failed = 0, first_errno = 0;
    if (pthread_create(&thread, 0, churn, 0) != 0) return 3;
    for (i = 0; i < FORKS; ++i) {
        pid_t pid = fork();
        if (pid == 0) _exit(0);
        if (pid < 0) {
            if (failed++ == 0) first_errno = errno;
            continue;
        }
        waitpid(pid, 0, 0);
    }
    stop = 1;
    pthread_join(thread, 0);
    printf("forks=%d failed=%d first_errno=%d\n", FORKS, failed, first_errno);
    return failed == 0 ? 0 : 2;
}
