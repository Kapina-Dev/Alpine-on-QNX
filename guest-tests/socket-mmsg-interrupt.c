/*
 * A signal arriving while recvmmsg blocks (guest fixture, musl).
 *
 * Checks (the exit code is the check number, also printed on stderr):
 *   10 setup failed
 *   18 recvmmsg with an SA_RESTART handler did not restart after the signal
 *      and return the datagram a forked child sent later
 *   19 recvmmsg without SA_RESTART did not fail with EINTR
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The 32-bit ARM kernel layout of struct mmsghdr, spelled out. */
struct slot_layout {
    unsigned int name, name_length, iov, iov_count, control, control_length,
        flags, length;
};

static volatile int fired;

static void on_alarm(int signal_number)
{
    (void)signal_number;
    fired++;
}

static int fail(int check, const char *what)
{
    fprintf(stderr, "socket-mmsg-interrupt check %d failed: %s (errno %d)\n",
        check, what, errno);
    return check;
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

/* A forked child signals the parent after a delay and then, if asked,
   sends a datagram. */
static pid_t child_signals(int send_fd, int send_after)
{
    pid_t child = fork();
    if (child == 0) {
        struct timespec delay = { 1, 0 };
        nanosleep(&delay, 0);
        kill(getppid(), SIGALRM);
        if (send_after) {
            nanosleep(&delay, 0);
            if (write(send_fd, "hello", 5) != 5) _exit(1);
        }
        _exit(0);
    }
    return child;
}

int main(void)
{
    int pair[2];
    char buffer[16];
    struct iovec iov;
    struct slot_layout slot;
    long result;
    pid_t child;
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0)
        return fail(10, "socketpair");

    install(SA_RESTART);
    fired = 0;
    child = child_signals(pair[0], 1);
    memset(&slot, 0, sizeof(slot));
    iov.iov_base = buffer;
    iov.iov_len = sizeof(buffer);
    slot.iov = (unsigned int)(unsigned long)&iov;
    slot.iov_count = 1;
    result = syscall(365, pair[1], &slot, 1, 0, 0);
    waitpid(child, 0, 0);
    if (result != 1 || fired != 1 || slot.length != 5 ||
            memcmp(buffer, "hello", 5) != 0)
        return fail(18, "SA_RESTART recvmmsg did not restart");

    install(0);
    fired = 0;
    child = child_signals(pair[0], 0);
    memset(&slot, 0, sizeof(slot));
    slot.iov = (unsigned int)(unsigned long)&iov;
    slot.iov_count = 1;
    result = syscall(365, pair[1], &slot, 1, 0, 0);
    waitpid(child, 0, 0);
    if (result != -1 || errno != EINTR || fired != 1)
        return fail(19, "recvmmsg without SA_RESTART was not EINTR");
    return 0;
}
