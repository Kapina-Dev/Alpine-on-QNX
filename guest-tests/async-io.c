/*
 * Socket async I/O: FIOASYNC, O_ASYNC, F_SETOWN/F_GETOWN and SIGIO
 * (guest fixture, musl).
 *
 * Linuxemu passes async I/O through to QNX for AF_UNIX stream and datagram
 * sockets only and refuses every other descriptor with EINVAL: QNX would
 * accept it and never send SIGIO (pipes, ptys), or, for AF_INET, leak SIGIO
 * to other processes (docs/FINDINGS.md, "QNX socket async I/O" and "QNX
 * io-pkt SIGIO leak after TCP async").  QNX's siginfo has no
 * si_fd, so only si_signo is checked.  "Delivered" means the handler ran
 * within 2 s; "not delivered" means it did not run within 500 ms.
 *
 * Checks (the exit code is the check number, also printed on stderr with
 * errno; a check that hangs is ended by the watchdog thread with exit code
 * 100 + the check number).  Each check makes its own sockets, so
 * "only N" as arguments runs check N alone.  An "async socket" is
 * socketpair(AF_UNIX, SOCK_STREAM) end 0 with FIOASYNC 1 and
 * F_SETOWN(getpid()).
 *   10 ioctl(FIOASYNC, &1) is 0 and F_GETFL shows O_ASYNC (0x2000)
 *   11 F_SETOWN(getpid()) is 0 and F_GETOWN returns getpid()
 *   12 a peer write delivers SIGIO to an SA_SIGINFO handler, si_signo 29
 *   13 FIOASYNC 0 clears O_ASYNC in F_GETFL; a peer write is not delivered
 *   14 F_SETFL(F_GETFL | O_ASYNC) alone (owner set, no FIOASYNC) makes a
 *      peer write deliver
 *   15 F_SETFL O_ASYNC|O_NONBLOCK sets both; F_SETFL O_ASYNC alone clears
 *      O_NONBLOCK and keeps O_ASYNC; F_SETFL 0 clears O_ASYNC
 *   16 a pipe: FIOASYNC fails EINVAL; F_SETFL O_ASYNC fails EINVAL and
 *      F_GETFL is unchanged; F_SETOWN fails EINVAL
 *   17 an eventfd and a regular file: FIOASYNC fails EINVAL
 *   18 ioctl(FIOASYNC, (int *)1) on a socket fails EFAULT
 *   19 a fork child sees O_ASYNC and F_GETOWN == the parent's pid (QNX
 *      keeps both in the socket)
 *   20 a fork child that re-execs this fixture ("exec-20") still sees
 *      O_ASYNC and F_GETOWN == the original parent's pid
 *   21 a fork child with SIGIO at its default action that enables async
 *      I/O for itself is killed by a peer write: WTERMSIG 29
 *   22 AF_INET is refused (QNX io-pkt leaks SIGIO from a TCP socket that
 *      had async I/O to later, unrelated processes): on a TCP loopback
 *      accepted socket FIOASYNC, F_SETFL O_ASYNC (F_GETFL unchanged) and
 *      F_SETOWN fail EINVAL, FIOASYNC on the listener and on a UDP socket
 *      fails EINVAL, and so does an AF_INET6 socket ("skip" when the guest
 *      has no AF_INET6); when LINUXEMU_DIAGNOSTICS names a file the guest
 *      can read, it holds the AF_UNIX-only diagnostic ("skip" otherwise)
 *   23 a handler without SA_RESTART interrupts a blocked poll (no events,
 *      5 s timeout) with EINTR when a helper thread's peer write signals;
 *      with SA_RESTART a blocked read returns the written byte
 *   24 F_SETOWN(-getpgrp()) fails EINVAL (QNX has no process-group owner)
 *      and the owner stays getpid()
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LINUX_O_ASYNC 0x2000
#define FILE_PATH "/tmp/async-io-file"

static volatile int current_check;
static volatile int watchdog_ticks;     /* 100 ms ticks since arm() */
static volatile sig_atomic_t sigio_count;
static volatile sig_atomic_t sigio_signo;

static int fail(int check, const char *what)
{
    fprintf(stderr, "async-io check %d failed: %s (errno %d %s)\n",
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

static void block_sigio(void)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGIO);
    pthread_sigmask(SIG_BLOCK, &set, 0);
}

/* Each check arms the watchdog for itself: BerryCore timeout never
   fires on the device.  SIGIO stays off this thread, so it reaches the
   thread under test. */
static void *watchdog(void *argument)
{
    (void)argument;
    block_sigio();
    for (;;) {
        sleep_ms(100);
        if (__sync_add_and_fetch(&watchdog_ticks, 1) >= 150) {
            fprintf(stderr, "async-io check %d timed out\n", current_check);
            _exit(100 + current_check);
        }
    }
    return 0;
}

/* Starts the check's 15 s bound. */
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

static void on_sigio(int signo, siginfo_t *information, void *context)
{
    (void)signo;
    (void)context;
    sigio_signo = information != 0 ? information->si_signo : -1;
    sigio_count = sigio_count + 1;
}

static int install_handler(int flags)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = on_sigio;
    action.sa_flags = SA_SIGINFO | flags;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGIO, &action, 0);
}

static void reset_signals(void)
{
    sigio_count = 0;
    sigio_signo = 0;
}

/* 1 when the handler ran within milliseconds. */
static int handler_ran(int milliseconds)
{
    int waited;
    for (waited = 0; waited < milliseconds; waited += 10) {
        if (sigio_count != 0) return 1;
        sleep_ms(10);
    }
    return sigio_count != 0;
}

static void drain(int fd)
{
    char buffer[64];
    while (recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT) > 0) {}
}

static int is_async(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && (flags & LINUX_O_ASYNC) != 0;
}

/* socketpair end 0 with FIOASYNC 1 and owner getpid(); 0 or -1. */
static int async_pair(int check, int pair[2])
{
    int on = 1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        return fail(check, "socketpair"), -1;
    if (ioctl(pair[0], FIOASYNC, &on) != 0)
        return fail(check, "ioctl FIOASYNC 1"), -1;
    if (fcntl(pair[0], F_SETOWN, getpid()) != 0)
        return fail(check, "F_SETOWN getpid"), -1;
    return 0;
}

static void close_pair(int pair[2])
{
    close(pair[0]);
    close(pair[1]);
}

/* fd's call returned -1/EINVAL. */
static int einval(int result)
{
    return result == -1 && errno == EINVAL;
}

static int check_10(void)
{
    int pair[2], on = 1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        return fail(10, "socketpair");
    if (ioctl(pair[0], FIOASYNC, &on) != 0)
        return fail(10, "ioctl FIOASYNC 1");
    if (!is_async(pair[0])) return fail(10, "F_GETFL lacks O_ASYNC");
    close_pair(pair);
    return 0;
}

static int check_11(void)
{
    int pair[2], owner;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        return fail(11, "socketpair");
    if (fcntl(pair[0], F_SETOWN, getpid()) != 0)
        return fail(11, "F_SETOWN getpid");
    owner = fcntl(pair[0], F_GETOWN);
    if (owner != getpid()) {
        fprintf(stderr, "async-io F_GETOWN %d, pid %d\n", owner,
            (int)getpid());
        return fail(11, "F_GETOWN is not getpid()");
    }
    close_pair(pair);
    return 0;
}

static int check_12(void)
{
    int pair[2];
    if (async_pair(12, pair) != 0) return 12;
    reset_signals();
    if (write(pair[1], "x", 1) != 1) return fail(12, "peer write");
    if (!handler_ran(2000)) return fail(12, "SIGIO not delivered");
    if (sigio_signo != 29) {
        fprintf(stderr, "async-io si_signo %d\n", (int)sigio_signo);
        return fail(12, "si_signo is not 29");
    }
    close_pair(pair);
    return 0;
}

static int check_13(void)
{
    int pair[2], off = 0;
    if (async_pair(13, pair) != 0) return 13;
    if (ioctl(pair[0], FIOASYNC, &off) != 0)
        return fail(13, "ioctl FIOASYNC 0");
    if (is_async(pair[0])) return fail(13, "F_GETFL still shows O_ASYNC");
    reset_signals();
    if (write(pair[1], "x", 1) != 1) return fail(13, "peer write");
    if (handler_ran(500)) return fail(13, "SIGIO delivered after FIOASYNC 0");
    close_pair(pair);
    return 0;
}

static int check_14(void)
{
    int pair[2], flags;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        return fail(14, "socketpair");
    if (fcntl(pair[0], F_SETOWN, getpid()) != 0)
        return fail(14, "F_SETOWN getpid");
    flags = fcntl(pair[0], F_GETFL);
    if (flags < 0) return fail(14, "F_GETFL");
    if (fcntl(pair[0], F_SETFL, flags | LINUX_O_ASYNC) != 0)
        return fail(14, "F_SETFL O_ASYNC");
    if (!is_async(pair[0])) return fail(14, "F_GETFL lacks O_ASYNC");
    reset_signals();
    if (write(pair[1], "x", 1) != 1) return fail(14, "peer write");
    if (!handler_ran(2000)) return fail(14, "SIGIO not delivered");
    close_pair(pair);
    return 0;
}

static int check_15(void)
{
    int pair[2], flags;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        return fail(15, "socketpair");
    if (fcntl(pair[0], F_SETFL, LINUX_O_ASYNC | O_NONBLOCK) != 0)
        return fail(15, "F_SETFL O_ASYNC|O_NONBLOCK");
    flags = fcntl(pair[0], F_GETFL);
    if (flags < 0 || (flags & LINUX_O_ASYNC) == 0 ||
            (flags & O_NONBLOCK) == 0)
        return fail(15, "F_GETFL lacks O_ASYNC or O_NONBLOCK");
    if (fcntl(pair[0], F_SETFL, LINUX_O_ASYNC) != 0)
        return fail(15, "F_SETFL O_ASYNC");
    flags = fcntl(pair[0], F_GETFL);
    if (flags < 0 || (flags & LINUX_O_ASYNC) == 0 ||
            (flags & O_NONBLOCK) != 0)
        return fail(15, "F_SETFL O_ASYNC alone did not keep only O_ASYNC");
    if (fcntl(pair[0], F_SETFL, 0) != 0) return fail(15, "F_SETFL 0");
    if (is_async(pair[0])) return fail(15, "F_SETFL 0 kept O_ASYNC");
    close_pair(pair);
    return 0;
}

static int check_16(void)
{
    int pipe_fds[2], on = 1, before, after;
    if (pipe(pipe_fds) != 0) return fail(16, "pipe");
    if (!einval(ioctl(pipe_fds[0], FIOASYNC, &on)))
        return fail(16, "pipe FIOASYNC is not -1/EINVAL");
    before = fcntl(pipe_fds[0], F_GETFL);
    if (before < 0) return fail(16, "F_GETFL");
    if (!einval(fcntl(pipe_fds[0], F_SETFL, before | LINUX_O_ASYNC)))
        return fail(16, "pipe F_SETFL O_ASYNC is not -1/EINVAL");
    after = fcntl(pipe_fds[0], F_GETFL);
    if (after != before) return fail(16, "pipe F_GETFL changed");
    if (!einval(fcntl(pipe_fds[0], F_SETOWN, getpid())))
        return fail(16, "pipe F_SETOWN is not -1/EINVAL");
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return 0;
}

static int check_17(void)
{
    int event, file, on = 1;
    event = eventfd(0, 0);
    if (event < 0) return fail(17, "eventfd");
    if (!einval(ioctl(event, FIOASYNC, &on)))
        return fail(17, "eventfd FIOASYNC is not -1/EINVAL");
    close(event);
    file = open(FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (file < 0) return fail(17, "open " FILE_PATH);
    if (!einval(ioctl(file, FIOASYNC, &on)))
        return fail(17, "regular file FIOASYNC is not -1/EINVAL");
    close(file);
    unlink(FILE_PATH);
    return 0;
}

static int check_18(void)
{
    int pair[2], result;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        return fail(18, "socketpair");
    result = ioctl(pair[0], FIOASYNC, (int *)1);
    if (result != -1 || errno != EFAULT)
        return fail(18, "FIOASYNC (int *)1 is not -1/EFAULT");
    close_pair(pair);
    return 0;
}

/* Waits for child; its exit status, or -1 when it did not exit. */
static int child_exit(pid_t child)
{
    int status;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) return -1;
    if (!WIFEXITED(status)) {
        fprintf(stderr, "async-io child status 0x%x\n", status);
        return -1;
    }
    return WEXITSTATUS(status);
}

/* In a child: 0 when fd shows O_ASYNC and owner, else 1 or 2. */
static int child_sees(int fd, pid_t owner)
{
    int seen;
    if (!is_async(fd)) return 1;
    seen = fcntl(fd, F_GETOWN);
    if (seen != owner) {
        fprintf(stderr, "async-io child F_GETOWN %d, expected %d\n", seen,
            (int)owner);
        return 2;
    }
    return 0;
}

static int check_19(void)
{
    int pair[2], status;
    pid_t parent = getpid(), child;
    if (async_pair(19, pair) != 0) return 19;
    child = fork();
    if (child < 0) return fail(19, "fork");
    if (child == 0) _exit(child_sees(pair[0], parent));
    status = child_exit(child);
    if (status != 0) {
        fprintf(stderr, "async-io child exit %d (1: no O_ASYNC, "
            "2: owner)\n", status);
        return fail(19, "fork child state");
    }
    close_pair(pair);
    return 0;
}

static int exec_image_20(int fd, pid_t owner)
{
    return child_sees(fd, owner);
}

static int check_20(const char *self)
{
    int pair[2], status;
    char fd_text[16], owner_text[16];
    pid_t child;
    if (async_pair(20, pair) != 0) return 20;
    snprintf(fd_text, sizeof(fd_text), "%d", pair[0]);
    snprintf(owner_text, sizeof(owner_text), "%d", (int)getpid());
    child = fork();
    if (child < 0) return fail(20, "fork");
    if (child == 0) {
        execl(self, self, "exec-20", fd_text, owner_text, (char *)0);
        _exit(3);
    }
    status = child_exit(child);
    if (status != 0) {
        fprintf(stderr, "async-io exec child exit %d (1: no O_ASYNC, "
            "2: owner, 3: exec failed)\n", status);
        return fail(20, "exec child state");
    }
    close_pair(pair);
    return 0;
}

static int check_21(void)
{
    int status;
    pid_t child = fork();
    if (child < 0) return fail(21, "fork");
    if (child == 0) {
        int pair[2], on = 1;
        signal(SIGIO, SIG_DFL);
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) _exit(1);
        if (ioctl(pair[0], FIOASYNC, &on) != 0) _exit(2);
        if (fcntl(pair[0], F_SETOWN, getpid()) != 0) _exit(3);
        if (write(pair[1], "x", 1) != 1) _exit(4);
        sleep_ms(2000);
        _exit(5);
    }
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) return fail(21, "waitpid");
    if (!WIFSIGNALED(status) || WTERMSIG(status) != 29) {
        fprintf(stderr, "async-io child status 0x%x (exit 1-4: setup, "
            "5: survived)\n", status);
        return fail(21, "child was not killed by SIGIO");
    }
    return 0;
}

/* 1 when LINUXEMU_DIAGNOSTICS names a file this guest can read and it
   holds text; 0 when it doesn't; -1 when the file can't be read. */
static int diagnostics_mention(const char *text)
{
    const char *path = getenv("LINUXEMU_DIAGNOSTICS");
    char line[512];
    int found = 0;
    FILE *file;
    if (path == 0 || path[0] == '\0' || (file = fopen(path, "r")) == 0)
        return -1;
    while (!found && fgets(line, sizeof(line), file) != 0)
        found = strstr(line, text) != 0;
    fclose(file);
    return found;
}

/* AF_INET async I/O is refused: QNX io-pkt leaks SIGIO from a TCP socket
   that had it to later, unrelated processes (docs/FINDINGS.md). */
static int check_22(void)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int listener, client, accepted, udp, inet6, on = 1, flags, seen;
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return fail(22, "socket");
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0)
        return fail(22, "bind 127.0.0.1:0");
    if (listen(listener, 4) != 0) return fail(22, "listen");
    if (getsockname(listener, (struct sockaddr *)&address, &length) != 0)
        return fail(22, "getsockname");
    client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0) return fail(22, "client socket");
    if (connect(client, (struct sockaddr *)&address, sizeof(address)) != 0)
        return fail(22, "connect");
    accepted = accept(listener, 0, 0);
    if (accepted < 0) return fail(22, "accept");
    if (!einval(ioctl(accepted, FIOASYNC, &on)))
        return fail(22, "TCP accepted FIOASYNC is not -1/EINVAL");
    if (!einval(ioctl(listener, FIOASYNC, &on)))
        return fail(22, "TCP listener FIOASYNC is not -1/EINVAL");
    flags = fcntl(accepted, F_GETFL);
    if (flags < 0) return fail(22, "F_GETFL");
    if (!einval(fcntl(accepted, F_SETFL, flags | LINUX_O_ASYNC)))
        return fail(22, "TCP F_SETFL O_ASYNC is not -1/EINVAL");
    if (fcntl(accepted, F_GETFL) != flags)
        return fail(22, "TCP F_GETFL changed");
    if (!einval(fcntl(accepted, F_SETOWN, getpid())))
        return fail(22, "TCP F_SETOWN is not -1/EINVAL");
    close(accepted);
    close(client);
    close(listener);
    udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp < 0) return fail(22, "UDP socket");
    if (!einval(ioctl(udp, FIOASYNC, &on)))
        return fail(22, "UDP FIOASYNC is not -1/EINVAL");
    close(udp);
    inet6 = socket(AF_INET6, SOCK_STREAM, 0);
    if (inet6 < 0) {
        fprintf(stderr, "skip 22 AF_INET6 (errno %d)\n", errno);
    } else {
        if (!einval(ioctl(inet6, FIOASYNC, &on)))
            return fail(22, "AF_INET6 FIOASYNC is not -1/EINVAL");
        close(inet6);
    }
    seen = diagnostics_mention("only supported on AF_UNIX sockets");
    if (seen < 0) fprintf(stderr, "skip 22 diagnostic (not readable)\n");
    else if (seen == 0) {
        errno = 0;
        return fail(22, "no AF_UNIX-only diagnostic recorded");
    }
    return 0;
}

struct writer {
    int fd;
    char byte;
};

static void *delayed_write(void *argument)
{
    struct writer *writer = argument;
    sleep_ms(300);
    if (write(writer->fd, &writer->byte, 1) != 1)
        fprintf(stderr, "async-io helper write failed (errno %d)\n", errno);
    return 0;
}

/* Starts the helper with SIGIO blocked, so SIGIO goes to this thread. */
static int start_writer(pthread_t *thread, struct writer *writer)
{
    sigset_t set, old;
    int result;
    sigemptyset(&set);
    sigaddset(&set, SIGIO);
    pthread_sigmask(SIG_BLOCK, &set, &old);
    result = pthread_create(thread, 0, delayed_write, writer);
    pthread_sigmask(SIG_SETMASK, &old, 0);
    return result;
}

static int check_23(void)
{
    int pair[2], pipe_fds[2], result;
    struct pollfd poller;
    struct writer writer;
    pthread_t thread;
    char byte = 0;
    if (pipe(pipe_fds) != 0) return fail(23, "pipe");
    if (async_pair(23, pair) != 0) return 23;
    if (install_handler(0) != 0) return fail(23, "sigaction");
    reset_signals();
    writer.fd = pair[1];
    writer.byte = 'p';
    if (start_writer(&thread, &writer) != 0)
        return fail(23, "pthread_create");
    poller.fd = pipe_fds[0];
    poller.events = POLLIN;
    poller.revents = 0;
    result = poll(&poller, 1, 5000);
    if (result != -1 || errno != EINTR)
        return fail(23, "poll was not interrupted with EINTR");
    pthread_join(thread, 0);
    drain(pair[0]);
    if (install_handler(SA_RESTART) != 0)
        return fail(23, "sigaction SA_RESTART");
    reset_signals();
    writer.byte = 'r';
    if (start_writer(&thread, &writer) != 0)
        return fail(23, "pthread_create");
    result = (int)read(pair[0], &byte, 1);
    if (result != 1 || byte != 'r')
        return fail(23, "SA_RESTART read did not return the byte");
    pthread_join(thread, 0);
    if (!handler_ran(2000)) return fail(23, "SIGIO not delivered");
    close_pair(pair);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return 0;
}

static int check_24(void)
{
    int pair[2], owner;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        return fail(24, "socketpair");
    if (fcntl(pair[0], F_SETOWN, getpid()) != 0)
        return fail(24, "F_SETOWN getpid");
    if (!einval(fcntl(pair[0], F_SETOWN, -getpgrp())))
        return fail(24, "F_SETOWN(-getpgrp()) is not -1/EINVAL");
    owner = fcntl(pair[0], F_GETOWN);
    if (owner != getpid()) return fail(24, "owner changed");
    close_pair(pair);
    return 0;
}

static int run_check(int check, const char *self)
{
    if (install_handler(0) != 0) return fail(check, "sigaction");
    switch (check) {
    case 10: return check_10();
    case 11: return check_11();
    case 12: return check_12();
    case 13: return check_13();
    case 14: return check_14();
    case 15: return check_15();
    case 16: return check_16();
    case 17: return check_17();
    case 18: return check_18();
    case 19: return check_19();
    case 20: return check_20(self);
    case 21: return check_21();
    case 22: return check_22();
    case 23: return check_23();
    default: return check_24();
    }
}

int main(int argc, char **argv)
{
    int result, check, only = 0;
    if (argc > 3 && strcmp(argv[1], "exec-20") == 0)
        return exec_image_20(atoi(argv[2]), (pid_t)atoi(argv[3]));
    if (argc > 2 && strcmp(argv[1], "only") == 0) only = atoi(argv[2]);
    for (check = 10; check <= 24; ++check) {
        if (only != 0 && check != only) continue;
        arm(check);
        if ((result = run_check(check, argv[0])) != 0) return result;
    }
    return 0;
}
