/*
 * signalfd across fork, execve and SCM_RIGHTS (guest fixture, musl).
 *
 * SIGUSR1 and SIGUSR2 are blocked in main before anything else, so every
 * process and image below blocks them (the mask survives fork and exec).
 * A check that needs a signal collected waits until a signalfd polls
 * readable (the collector moved it into the process queue).
 *
 * Checks (the exit code is the check number, also printed on stderr with
 * errno; a check that hangs is ended by the watchdog thread with exit code
 * 100 + the check number; forked children and exec'd images are bounded
 * by alarm(5), which shows as a SIGALRM death):
 *   10 setup failed
 *   11 fork: the parent collects a SIGUSR1 on one signalfd (a second one,
 *      for SIGUSR2, stays idle), then forks; a SIGUSR2 the child sends
 *      itself makes the idle signalfd poll readable in the child (its
 *      collector starts on that first poll) and is read there with the
 *      child's pid; then the child's read of the first signalfd fails
 *      EAGAIN (a child's process queue starts empty); afterwards select
 *      shows the parent's signalfd readable and the parent still reads
 *      its own SIGUSR1 once
 *   12 exec: a fork child makes a signalfd for SIGUSR1 without
 *      SFD_CLOEXEC and execs this fixture; in the new image the fd is
 *      still a signalfd with the SIGUSR1 mask: after SIGUSR2 and SIGUSR1
 *      are sent, it polls readable and reads SIGUSR1 only; widened to
 *      SIGUSR2 by signalfd(fd, ...), it reads SIGUSR2
 *   13 exec with a collected SIGUSR1 still queued: a fork child's
 *      signalfd collects SIGUSR1, then the child execs this fixture; in
 *      the new image a fresh signalfd for SIGUSR1 polls readable (the
 *      signal is pending for the process again, so the new image's
 *      collector takes it), the inherited signalfd reads it once, and
 *      both then fail EAGAIN (ssi_pid is not checked)
 *   14 SCM_RIGHTS: a signalfd sent to a forked child that closed its own
 *      copy reads the child's signals, as on Linux (Linuxemu's documented
 *      choice: the object is rebuilt in the receiver from its shared
 *      page, so the mask travels with it): the parent kills the child
 *      with SIGUSR1, the child polls the received fd readable and reads
 *      SIGUSR1 with the parent's pid, and the parent's own signalfd
 *      reads nothing
 *   15 a shared signalfd keeps each process's wake-up: a fork child
 *      spins non-blocking reads of the inherited signalfd (each empty
 *      read settles the readiness) while the parent, 2000 times, enters
 *      poll(s + 600 idle pipe fds, 1000 ms) and a helper thread sends the
 *      parent SIGUSR1 at a varying point of that entry or sleep (the idle
 *      fds widen the entry); every poll must return
 *      readable and the read return SIGUSR1 (a wake-up drained by the
 *      child times out)
 *
 * Exit codes of a fork child or exec'd image (printed by the parent):
 * 2 mask wrong, 3 exec failed, 4-9 the step that failed (see the code).
 *
 * SIGNALFD_SHARE_ONLY=N runs check N alone.
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
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NR_SIGNALFD4 355
#define SFD_NONBLOCK_FLAG 0x800
#define RECORD 128

struct record {
    uint32_t ssi_signo;
    int32_t ssi_errno;
    int32_t ssi_code;
    uint32_t ssi_pid;
    uint8_t rest[RECORD - 16];
};

static volatile int current_check;
static volatile int watchdog_ticks;

static int fail(int check, const char *what)
{
    fprintf(stderr, "signalfd-share check %d failed: %s (errno %d %s)\n",
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

static void *watchdog(void *argument)
{
    (void)argument;
    for (;;) {
        sleep_ms(100);
        if (__sync_add_and_fetch(&watchdog_ticks, 1) >= 100) {
            fprintf(stderr, "signalfd-share check %d timed out\n",
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

static void on_usr1(int signal_number)
{
    (void)signal_number;
}

static uint64_t bit(int signal_number)
{
    return (uint64_t)1 << (signal_number - 1);
}

static int sfd(int fd, uint64_t mask, int flags)
{
    return (int)syscall(NR_SIGNALFD4, fd, &mask, 8, flags);
}

static int read_eagain(int fd)
{
    struct record record;
    errno = 0;
    return read(fd, &record, sizeof(record)) == -1 && errno == EAGAIN;
}

/* One record into *record (the buffer has room for two): 1, else 0. */
static int read_one(int fd, struct record *record)
{
    struct record records[2];
    if (read(fd, records, sizeof(records)) != RECORD) return 0;
    *record = records[0];
    return 1;
}

/* Waits up to 2 s for the signalfd to poll readable. */
static int wait_collected(int fd)
{
    struct pollfd entry;
    entry.fd = fd;
    entry.events = POLLIN;
    entry.revents = 0;
    return poll(&entry, 1, 2000) == 1 && (entry.revents & POLLIN) != 0;
}

/* select (not poll) shows the signalfd readable within 2 s. */
static int select_readable(int fd)
{
    struct timeval timeout = { 2, 0 };
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    return select(fd + 1, &set, 0, 0, &timeout) == 1 && FD_ISSET(fd, &set);
}

static int both_blocked(void)
{
    sigset_t set;
    return sigprocmask(SIG_BLOCK, 0, &set) == 0 &&
        sigismember(&set, SIGUSR1) == 1 && sigismember(&set, SIGUSR2) == 1;
}

/* Exit status of a child, or 0x100 + signal when it died of one. */
static int wait_child(pid_t pid)
{
    int status;
    if (waitpid(pid, &status, 0) != pid) return 0x200;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return WIFSIGNALED(status) ? 0x100 + WTERMSIG(status) : 0x200;
}

static int child_failed(int check, int status, const char *what)
{
    fprintf(stderr, "signalfd-share: check %d child status %#x\n", check,
        status);
    errno = 0;
    return fail(check, what);
}

static int fork_child(void)
{
    struct record record;
    int s = sfd(-1, bit(SIGUSR1), SFD_NONBLOCK_FLAG), status;
    int idle = sfd(-1, bit(SIGUSR2), SFD_NONBLOCK_FLAG);
    pid_t parent = getpid(), child;
    if (s < 0 || idle < 0) return fail(10, "signalfd");
    if (kill(parent, SIGUSR1) != 0 || !wait_collected(s))
        return fail(11, "parent SIGUSR1 not collected");
    child = fork();
    if (child < 0) return fail(10, "fork");
    if (child == 0) {
        alarm(5);
        /* poll first: the child's collector must start without a read
           or a signalfd call (idle's channel is empty). */
        if (kill(getpid(), SIGUSR2) != 0) _exit(5);
        if (!wait_collected(idle)) _exit(6);
        if (!read_one(idle, &record) || record.ssi_signo != SIGUSR2 ||
                record.ssi_pid != (uint32_t)getpid()) _exit(7);
        if (!read_eagain(s)) _exit(4);
        if (!read_eagain(idle)) _exit(8);
        _exit(0);
    }
    status = wait_child(child);
    if (status != 0)
        return child_failed(11, status, "child read the parent's signal "
            "or not its own (4: read the parent's, 6: no readiness, 7: "
            "wrong record, 8: read twice)");
    if (!select_readable(s))
        return fail(11, "select doesn't show the parent's SIGUSR1");
    if (!read_one(s, &record) || record.ssi_signo != SIGUSR1 ||
            record.ssi_pid != (uint32_t)parent)
        return fail(11, "parent's collected SIGUSR1 lost after fork");
    if (!read_eagain(s) || !read_eagain(idle))
        return fail(11, "parent read a signal twice");
    close(s);
    close(idle);
    return 0;
}

static int exec_image_12(int fd)
{
    struct record record;
    alarm(5);
    if (!both_blocked()) return 2;
    if (!read_eagain(fd)) return 4;
    if (kill(getpid(), SIGUSR2) != 0 || kill(getpid(), SIGUSR1) != 0)
        return 9;
    if (!wait_collected(fd)) return 5;
    if (!read_one(fd, &record) || record.ssi_signo != SIGUSR1) return 6;
    if (!read_eagain(fd)) return 6;
    if (sfd(fd, bit(SIGUSR1) | bit(SIGUSR2), SFD_NONBLOCK_FLAG) != fd)
        return 7;
    if (!wait_collected(fd) || !read_one(fd, &record) ||
            record.ssi_signo != SIGUSR2) return 8;
    return 0;
}

static int exec_image_13(int fd)
{
    struct record record;
    int fresh;
    alarm(5);
    if (!both_blocked()) return 2;
    fresh = sfd(-1, bit(SIGUSR1), SFD_NONBLOCK_FLAG);
    if (fresh < 0) return 9;
    if (!wait_collected(fresh)) return 5;
    if (!read_one(fd, &record) || record.ssi_signo != SIGUSR1) return 6;
    if (!read_eagain(fd) || !read_eagain(fresh)) return 7;
    return 0;
}

static int exec_check(int check, const char *self)
{
    char argument[16], fd_text[16];
    int status;
    pid_t child = fork();
    if (child < 0) return fail(10, "fork");
    if (child == 0) {
        int s = sfd(-1, bit(SIGUSR1), SFD_NONBLOCK_FLAG);
        alarm(5);
        if (s < 0) _exit(9);
        if (check == 13 && (kill(getpid(), SIGUSR1) != 0 ||
                !wait_collected(s))) _exit(4);
        snprintf(argument, sizeof(argument), "exec-%d", check);
        snprintf(fd_text, sizeof(fd_text), "%d", s);
        /* The new image sets its own alarm. */
        alarm(0);
        execl(self, self, argument, fd_text, (char *)0);
        _exit(3);
    }
    status = wait_child(child);
    if (status != 0)
        return child_failed(check, status, check == 12 ?
            "signalfd lost across execve (4: not empty, 5: no readiness, "
            "6: wrong record or mask, 7: not a signalfd, 8: widened mask)" :
            "queued signal lost across execve (4: not collected before "
            "exec, 5: not pending for the new process, 6: inherited "
            "signalfd didn't read it, 7: read twice)");
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

static int rights_child(int socket_fd, int s)
{
    struct record record;
    int received;
    alarm(5);
    close(s);
    received = receive_fd(socket_fd);
    if (received < 0) return 9;
    if (write(socket_fd, "r", 1) != 1) return 9;
    if (!wait_collected(received)) return 4;
    if (!read_one(received, &record) || record.ssi_signo != SIGUSR1 ||
            record.ssi_pid != (uint32_t)getppid()) return 5;
    if (!read_eagain(received)) return 6;
    return 0;
}

static int check_rights(void)
{
    int s = sfd(-1, bit(SIGUSR1), SFD_NONBLOCK_FLAG), pair[2], status;
    char ready = 0;
    pid_t child;
    if (s < 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        return fail(10, "signalfd or socketpair");
    child = fork();
    if (child < 0) return fail(10, "fork");
    if (child == 0) {
        close(pair[0]);
        _exit(rights_child(pair[1], s));
    }
    close(pair[1]);
    if (send_fd(pair[0], s) != 0) return fail(14, "sendmsg");
    if (read(pair[0], &ready, 1) != 1 || ready != 'r')
        return fail(14, "child never received");
    if (kill(child, SIGUSR1) != 0) return fail(14, "kill child");
    status = wait_child(child);
    if (status != 0)
        return child_failed(14, status, "received signalfd did not read "
            "the child's signal (4: no readiness, 5: wrong record, 6: read "
            "twice)");
    if (!read_eagain(s)) return fail(14, "parent read the child's signal");
    close(s);
    close(pair[0]);
    return 0;
}

#define SHARED_ROUNDS 2000
#define SHARED_FILLERS 600

static volatile int kick_request, kick_stop, kick_spin;

/* Sends the parent SIGUSR1 after each request, after a spin that grows
   each round, so the signal lands at every point of the main thread's
   way into poll and while it sleeps there. */
static void *kicker(void *argument)
{
    (void)argument;
    while (!kick_stop) {
        volatile int spin;
        if (!kick_request) continue;
        for (spin = 0; spin < kick_spin; ++spin) {}
        kill(getpid(), SIGUSR1);
        kick_request = 0;
        __sync_synchronize();
    }
    return 0;
}

static int check_shared_wakeup(void)
{
    static struct pollfd entries[1 + SHARED_FILLERS];
    struct record record;
    pthread_t thread;
    int s = sfd(-1, bit(SIGUSR1), SFD_NONBLOCK_FLAG), round, result = 0;
    int idle[2], i;
    pid_t child;
    if (s < 0 || pipe(idle) != 0) return fail(10, "signalfd or pipe");
    /* s first, then idle fds: Linuxemu checks s's readiness first and
       the rest before the host poll sleeps, which widens the window in
       which a wake-up can be taken away. */
    entries[0].fd = s;
    for (i = 1; i <= SHARED_FILLERS; ++i) {
        entries[i].fd = i == 1 ? idle[0] : dup(idle[0]);
        if (entries[i].fd < 0) return fail(10, "dup");
    }
    child = fork();
    if (child < 0) return fail(10, "fork");
    if (child == 0) {
        alarm(20);
        for (;;) (void)read_eagain(s);
    }
    kick_request = 0;
    kick_stop = 0;
    if (pthread_create(&thread, 0, kicker, 0) != 0) {
        kill(child, SIGKILL);
        waitpid(child, 0, 0);
        return fail(10, "pthread_create");
    }
    sleep_ms(50);
    for (round = 0; round < SHARED_ROUNDS && result == 0; ++round) {
        int polled;
        kick_spin = (round % 250) * 80;
        kick_request = 1;
        __sync_synchronize();
        for (i = 0; i <= SHARED_FILLERS; ++i) {
            entries[i].events = POLLIN;
            entries[i].revents = 0;
        }
        polled = poll(entries, 1 + SHARED_FILLERS, 1000);
        while (kick_request) sleep_ms(1);
        if (polled != 1 || (entries[0].revents & POLLIN) == 0) {
            fprintf(stderr, "signalfd-share: round %d poll returned %d\n",
                round, polled);
            result = fail(15, "parent's wake-up lost to the child's read");
        } else if (!read_one(s, &record) || record.ssi_signo != SIGUSR1) {
            sleep_ms(50);
            if (!read_one(s, &record) || record.ssi_signo != SIGUSR1)
                result = fail(15, "readable but SIGUSR1 not read");
        }
        watchdog_ticks = 0;
    }
    kick_stop = 1;
    pthread_join(thread, 0);
    kill(child, SIGKILL);
    waitpid(child, 0, 0);
    for (i = 1; i <= SHARED_FILLERS; ++i) close(entries[i].fd);
    close(idle[1]);
    close(s);
    return result;
}

static int run_check(int check, const char *self)
{
    switch (check) {
    case 11: return fork_child();
    case 12: return exec_check(12, self);
    case 13: return exec_check(13, self);
    case 14: return check_rights();
    default: return check_shared_wakeup();
    }
}

int main(int argc, char **argv)
{
    struct sigaction action;
    sigset_t blocked;
    const char *only_text = getenv("SIGNALFD_SHARE_ONLY");
    int result, check, only = only_text == 0 ? 0 : atoi(only_text);
    if (argc > 2 && strcmp(argv[1], "exec-12") == 0)
        return exec_image_12(atoi(argv[2]));
    if (argc > 2 && strcmp(argv[1], "exec-13") == 0)
        return exec_image_13(atoi(argv[2]));
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    sigaddset(&blocked, SIGUSR2);
    if (sigprocmask(SIG_BLOCK, &blocked, 0) != 0)
        return fail(10, "sigprocmask");
    /* A stray delivery is counted as nothing rather than fatal. */
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_usr1;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, 0) != 0) return fail(10, "sigaction");
    for (check = 11; check <= 15; ++check) {
        if (only != 0 && check != only) continue;
        arm(check);
        if ((result = run_check(check, argv[0])) != 0) return result;
    }
    return 0;
}
