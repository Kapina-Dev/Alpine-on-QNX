/*
 * fork() while another thread opens and closes fds (guest fixture, musl).
 *
 * Native QNX fork() fails with EBADF when a descriptor is closed while it
 * duplicates the fd table; Linux fork() never does.  Git's HTTPS fetch hits
 * this (an async thread closes pipe ends while the main thread forks
 * index-pack).  A signal arriving mid-fork makes QNX fork() fail with
 * EINTR (make -j forking while SIGCHLDs arrive), and the same fd-table race
 * occasionally yields EINVAL.  A second thread keeps signalling the process
 * (handler without SA_RESTART).  Every fork must succeed: exit 2 reports a
 * failure.
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
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

static void on_signal(int number) { (void)number; }

static void *signaller(void *argument)
{
    (void)argument;
    while (!stop) kill(getpid(), SIGUSR1);
    return 0;
}

int main(void)
{
    struct sigaction action;
    pthread_t thread, signal_thread;
    int i, failed = 0, first_errno = 0;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigaction(SIGUSR1, &action, 0);
    if (pthread_create(&thread, 0, churn, 0) != 0 ||
            pthread_create(&signal_thread, 0, signaller, 0) != 0) return 3;
    for (i = 0; i < FORKS; ++i) {
        pid_t pid = fork();
        if (pid == 0) _exit(0);
        if (pid < 0) {
            if (failed++ == 0) first_errno = errno;
            continue;
        }
        while (waitpid(pid, 0, 0) < 0 && errno == EINTR) { }
    }
    stop = 1;
    pthread_join(thread, 0);
    pthread_join(signal_thread, 0);
    printf("forks=%d failed=%d first_errno=%d\n", FORKS, failed, first_errno);
    return failed == 0 ? 0 : 2;
}
