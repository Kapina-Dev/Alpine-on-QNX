/*
 * The Linux signalfd: create, read, coalescing, real-time queueing,
 * readiness, several signalfds, mask updates and a signal storm (guest
 * fixture, musl).
 *
 * SIGUSR1, SIGUSR2 and SIGRTMIN are blocked in main before any thread is
 * made, so every thread blocks them and a process-directed signal stays
 * pending for the signalfd (Linuxemu's collector moves it into the
 * process-wide queue).  The calls go through syscall() so the mask size
 * argument is explicit.
 *
 * Checks (the exit code is the check number, also printed on stderr with
 * errno; a check that hangs is ended by the watchdog thread with exit code
 * 100 + the check number):
 *   10 setup failed
 *   11 signalfd(-1, {SIGUSR1}, 8, 0), kill(getpid(), SIGUSR1): a read
 *      returns one 128-byte record with ssi_signo 10, ssi_pid getpid(),
 *      ssi_uid getuid()
 *   12 mask size 7 fails EINVAL; an unknown flag fails EINVAL; a read
 *      with a 127-byte buffer fails EINVAL
 *   13 three kills of SIGUSR1 before reading give one record
 *   14 sigqueue SIGRTMIN with values 1, 2, 3: one read with a 384-byte
 *      buffer returns three records in order with ssi_int 1, 2, 3
 *   15 an empty SFD_NONBLOCK signalfd read fails EAGAIN
 *   16 poll and epoll: not ready, then ready after a kill
 *   17 a fork child kills the parent: ssi_pid is the child's pid
 *   18 two signalfds {SIGUSR1} and {SIGUSR1, SIGUSR2}: after one SIGUSR1
 *      exactly one of the two reads returns it, the other fails EAGAIN
 *   19 signalfd(fd, {SIGUSR2}, 8, 0) on an existing signalfd returns fd and
 *      changes its mask: SIGUSR1 is no longer read; SIGUSR2 makes it
 *      readable (poll) and is read; an update of a non-signalfd fails
 *      EINVAL
 *   20 SIGKILL and SIGSTOP in the mask are accepted (and ignored)
 *   21 a blocking read is woken by a kill from another thread after 50 ms
 *   22 5000 kills of SIGUSR1 plus 200 sigqueues of SIGRTMIN: one SIGUSR1
 *      record and one SIGRTMIN record per accepted sigqueue, in order
 *      (Linuxemu keeps them all: real-time signals past the process
 *      queue's ring stay pending in the QNX kernel until read; a sigqueue
 *      the kernel refuses with EAGAIN ends the sending early); the open
 *      fd count is unchanged afterwards
 *   23 the blocked set survives execve: a fork child (SIGUSR1 blocked)
 *      execs this fixture, whose sigprocmask old set holds SIGUSR1
 *
 * Linuxemu moves a process-directed signal into its queue on a collector
 * thread, a few microseconds after the kill returns, so the checks that
 * count records give it a short settle time first (SETTLE_MS).
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
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NR_SIGNALFD 349
#define NR_SIGNALFD4 355
#define SFD_NONBLOCK_FLAG 0x800
#define SFD_CLOEXEC_FLAG 0x80000
#define RECORD 128
#define SETTLE_MS 30

struct record {
    uint32_t ssi_signo;
    int32_t ssi_errno;
    int32_t ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t ssi_status;
    int32_t ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint16_t ssi_addr_lsb;
    uint8_t pad[128 - 82];
};

typedef char verify_record_size[sizeof(struct record) == RECORD ? 1 : -1];

static volatile int current_check;
static volatile int watchdog_ticks;     /* 100 ms ticks since arm() */

static int fail(int check, const char *what)
{
    fprintf(stderr, "signalfd-basic check %d failed: %s (errno %d %s)\n",
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

static int64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Each check arms the watchdog for itself: BerryCore timeout never
   fires on the device. */
static void *watchdog(void *argument)
{
    (void)argument;
    for (;;) {
        sleep_ms(100);
        if (__sync_add_and_fetch(&watchdog_ticks, 1) >= 100) {
            fprintf(stderr, "signalfd-basic check %d timed out\n",
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
    if (getenv("SIGNALFD_BASIC_VERBOSE") != 0)
        fprintf(stderr, "signalfd-basic check %d\n", check);
    if (!started) {
        pthread_t thread;
        if (pthread_create(&thread, 0, watchdog, 0) == 0) started = 1;
    }
}

static uint64_t bit(int signal_number)
{
    return (uint64_t)1 << (signal_number - 1);
}

static int sfd(int fd, uint64_t mask, int flags)
{
    return (int)syscall(NR_SIGNALFD4, fd, &mask, 8, flags);
}

/* One read; the record count, or -1 with errno. */
static int read_records(int fd, struct record *records, int count)
{
    ssize_t got = read(fd, records, (size_t)count * RECORD);
    if (got < 0) return -1;
    if (got % RECORD != 0) { errno = EPROTO; return -1; }
    return (int)(got / RECORD);
}

static int read_eagain(int fd)
{
    struct record record;
    errno = 0;
    return read(fd, &record, sizeof(record)) == -1 && errno == EAGAIN;
}

/* Reads everything an SFD_NONBLOCK signalfd holds and discards it. */
static void drain(int fd)
{
    struct record records[8];
    while (read_records(fd, records, 8) > 0) {}
}

static int count_open_fds(void)
{
    int fd, count = 0;
    for (fd = 0; fd < 1024; ++fd)
        if (fcntl(fd, F_GETFD) >= 0) ++count;
    return count;
}

static int check_basic(void)
{
    struct record record;
    int fd = sfd(-1, bit(SIGUSR1), 0);
    if (fd < 0) return fail(11, "signalfd");
    if (kill(getpid(), SIGUSR1) != 0) return fail(11, "kill");
    if (read_records(fd, &record, 1) != 1) return fail(11, "read");
    if (record.ssi_signo != SIGUSR1) {
        fprintf(stderr, "ssi_signo %u\n", record.ssi_signo);
        return fail(11, "ssi_signo");
    }
    if (record.ssi_pid != (uint32_t)getpid()) {
        fprintf(stderr, "ssi_pid %u getpid %d\n", record.ssi_pid,
            (int)getpid());
        return fail(11, "ssi_pid");
    }
    if (record.ssi_uid != (uint32_t)getuid()) {
        fprintf(stderr, "ssi_uid %u getuid %u\n", record.ssi_uid,
            (unsigned)getuid());
        return fail(11, "ssi_uid");
    }
    if (record.ssi_code != 0) {
        fprintf(stderr, "ssi_code %d\n", record.ssi_code);
        return fail(11, "ssi_code of kill (SI_USER)");
    }
    close(fd);
    return 0;
}

static int check_validation(void)
{
    char buffer[RECORD - 1];
    uint64_t mask = bit(SIGUSR1);
    int fd;
    errno = 0;
    if (syscall(NR_SIGNALFD4, -1, &mask, 7, 0) != -1 || errno != EINVAL)
        return fail(12, "mask size 7 not EINVAL");
    errno = 0;
    if (syscall(NR_SIGNALFD, -1, &mask, 7) != -1 || errno != EINVAL)
        return fail(12, "signalfd (349) mask size 7 not EINVAL");
    errno = 0;
    if (sfd(-1, mask, 0x1) != -1 || errno != EINVAL)
        return fail(12, "unknown flag not EINVAL");
    fd = sfd(-1, mask, SFD_NONBLOCK_FLAG | SFD_CLOEXEC_FLAG);
    if (fd < 0) return fail(12, "signalfd");
    if ((fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0)
        return fail(12, "SFD_CLOEXEC did not set FD_CLOEXEC");
    errno = 0;
    if (read(fd, buffer, sizeof(buffer)) != -1 || errno != EINVAL)
        return fail(12, "127-byte read not EINVAL");
    close(fd);
    fd = (int)syscall(NR_SIGNALFD, -1, &mask, 8);
    if (fd < 0) return fail(12, "signalfd (349)");
    close(fd);
    return 0;
}

static int check_coalesce(void)
{
    struct record records[4];
    int fd = sfd(-1, bit(SIGUSR1), SFD_NONBLOCK_FLAG), got, i;
    if (fd < 0) return fail(13, "signalfd");
    for (i = 0; i < 3; ++i)
        if (kill(getpid(), SIGUSR1) != 0) return fail(13, "kill");
    sleep_ms(SETTLE_MS);
    got = read_records(fd, records, 4);
    if (got != 1) {
        fprintf(stderr, "records %d\n", got);
        return fail(13, "three kills did not give one record");
    }
    if (!read_eagain(fd)) return fail(13, "second read not EAGAIN");
    close(fd);
    return 0;
}

static int check_realtime(void)
{
    struct record records[3];
    union sigval value;
    int fd = sfd(-1, bit(SIGRTMIN), SFD_NONBLOCK_FLAG), got, i;
    if (fd < 0) return fail(14, "signalfd");
    for (i = 1; i <= 3; ++i) {
        value.sival_int = i;
        if (sigqueue(getpid(), SIGRTMIN, value) != 0)
            return fail(14, "sigqueue");
    }
    sleep_ms(SETTLE_MS);
    got = read_records(fd, records, 3);
    if (got != 3) {
        fprintf(stderr, "records %d\n", got);
        return fail(14, "one 384-byte read did not return three records");
    }
    for (i = 0; i < 3; ++i) {
        if (records[i].ssi_signo != (uint32_t)SIGRTMIN ||
                records[i].ssi_int != i + 1) {
            fprintf(stderr, "record %d signo %u int %d code %d\n", i,
                records[i].ssi_signo, records[i].ssi_int,
                records[i].ssi_code);
            return fail(14, "real-time records out of order");
        }
        if (records[i].ssi_code != -1) {
            fprintf(stderr, "ssi_code %d\n", records[i].ssi_code);
            return fail(14, "ssi_code of sigqueue (SI_QUEUE -1)");
        }
    }
    if (!read_eagain(fd)) return fail(14, "fourth record");
    close(fd);
    return 0;
}

static int check_empty(void)
{
    int fd = sfd(-1, bit(SIGUSR2), SFD_NONBLOCK_FLAG);
    if (fd < 0) return fail(15, "signalfd");
    if (!read_eagain(fd)) return fail(15, "empty read not EAGAIN");
    close(fd);
    return 0;
}

static int check_readiness(void)
{
    struct pollfd entry;
    struct epoll_event event, ready;
    int fd = sfd(-1, bit(SIGUSR1), SFD_NONBLOCK_FLAG), ep;
    if (fd < 0) return fail(16, "signalfd");
    ep = epoll_create1(0);
    if (ep < 0) return fail(16, "epoll_create1");
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.fd = fd;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event) != 0)
        return fail(16, "epoll_ctl");
    entry.fd = fd;
    entry.events = POLLIN;
    entry.revents = 0;
    if (poll(&entry, 1, 0) != 0) return fail(16, "poll ready before kill");
    if (epoll_wait(ep, &ready, 1, 0) != 0)
        return fail(16, "epoll ready before kill");
    if (kill(getpid(), SIGUSR1) != 0) return fail(16, "kill");
    entry.revents = 0;
    if (poll(&entry, 1, 2000) != 1 || (entry.revents & POLLIN) == 0)
        return fail(16, "poll not ready after kill");
    if (epoll_wait(ep, &ready, 1, 2000) != 1 ||
            (ready.events & EPOLLIN) == 0 || ready.data.fd != fd)
        return fail(16, "epoll not ready after kill");
    drain(fd);
    entry.revents = 0;
    if (poll(&entry, 1, 0) != 0) return fail(16, "poll ready after read");
    close(ep);
    close(fd);
    return 0;
}

static int check_fork_sender(void)
{
    struct record record;
    int fd = sfd(-1, bit(SIGUSR1), 0), status;
    pid_t child;
    if (fd < 0) return fail(17, "signalfd");
    child = fork();
    if (child < 0) return fail(17, "fork");
    if (child == 0) _exit(kill(getppid(), SIGUSR1) == 0 ? 0 : 1);
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0)
        return fail(17, "child");
    if (read_records(fd, &record, 1) != 1) return fail(17, "read");
    if (record.ssi_signo != SIGUSR1 || record.ssi_pid != (uint32_t)child) {
        fprintf(stderr, "signo %u pid %u child %d\n", record.ssi_signo,
            record.ssi_pid, (int)child);
        return fail(17, "ssi_pid is not the child's");
    }
    close(fd);
    return 0;
}

static int check_two(void)
{
    struct record record;
    int first = sfd(-1, bit(SIGUSR1), SFD_NONBLOCK_FLAG);
    int second = sfd(-1, bit(SIGUSR1) | bit(SIGUSR2), SFD_NONBLOCK_FLAG);
    int got_first, got_second;
    if (first < 0 || second < 0) return fail(18, "signalfd");
    if (kill(getpid(), SIGUSR1) != 0) return fail(18, "kill");
    sleep_ms(SETTLE_MS);
    got_first = read_records(first, &record, 1);
    if (got_first < 0 && errno != EAGAIN) return fail(18, "first read");
    got_second = read_records(second, &record, 1);
    if (got_second < 0 && errno != EAGAIN) return fail(18, "second read");
    if ((got_first == 1) == (got_second == 1)) {
        fprintf(stderr, "first %d second %d\n", got_first, got_second);
        return fail(18, "not exactly one read returned the signal");
    }
    /* The failed read left the buffer as the successful one filled it. */
    if (record.ssi_signo != SIGUSR1) return fail(18, "wrong signal");
    close(first);
    close(second);
    return 0;
}

static int check_update(void)
{
    struct pollfd entry;
    struct record record;
    int fd = sfd(-1, bit(SIGUSR1), SFD_NONBLOCK_FLAG), other;
    if (fd < 0) return fail(19, "signalfd");
    if (sfd(fd, bit(SIGUSR2), 0) != fd) return fail(19, "mask update");
    if (kill(getpid(), SIGUSR1) != 0) return fail(19, "kill SIGUSR1");
    sleep_ms(SETTLE_MS);
    if (!read_eagain(fd)) return fail(19, "SIGUSR1 read after the update");
    if (kill(getpid(), SIGUSR2) != 0) return fail(19, "kill SIGUSR2");
    /* Readable without a read first: the signal reached the signalfd
       (on Linuxemu, the collector now takes SIGUSR2). */
    entry.fd = fd;
    entry.events = POLLIN;
    entry.revents = 0;
    if (poll(&entry, 1, 2000) != 1 || (entry.revents & POLLIN) == 0)
        return fail(19, "not readable after SIGUSR2 with the new mask");
    if (read_records(fd, &record, 1) != 1 || record.ssi_signo != SIGUSR2)
        return fail(19, "SIGUSR2 not read after the update");
    /* The SIGUSR1 is still pending: take it. */
    other = sfd(-1, bit(SIGUSR1), SFD_NONBLOCK_FLAG);
    if (other < 0) return fail(19, "signalfd for the leftover");
    if (read_records(other, &record, 1) != 1 || record.ssi_signo != SIGUSR1)
        return fail(19, "leftover SIGUSR1 lost");
    close(other);
    close(fd);
    errno = 0;
    if (sfd(1, bit(SIGUSR1), 0) != -1 || errno != EINVAL)
        return fail(19, "update of a non-signalfd not EINVAL");
    return 0;
}

static int check_unblockable(void)
{
    struct record record;
    int fd = sfd(-1, bit(SIGUSR1) | bit(SIGKILL) | bit(SIGSTOP),
        SFD_NONBLOCK_FLAG);
    if (fd < 0) return fail(20, "signalfd with SIGKILL and SIGSTOP");
    if (kill(getpid(), SIGUSR1) != 0) return fail(20, "kill");
    sleep_ms(SETTLE_MS);
    if (read_records(fd, &record, 1) != 1 || record.ssi_signo != SIGUSR1)
        return fail(20, "read");
    close(fd);
    return 0;
}

static void *late_kill(void *argument)
{
    (void)argument;
    sleep_ms(50);
    kill(getpid(), SIGUSR1);
    return 0;
}

static int check_blocking(void)
{
    struct record record;
    pthread_t thread;
    int64_t started;
    int fd = sfd(-1, bit(SIGUSR1), 0);
    if (fd < 0) return fail(21, "signalfd");
    started = now_ms();
    if (pthread_create(&thread, 0, late_kill, 0) != 0)
        return fail(21, "pthread_create");
    if (read_records(fd, &record, 1) != 1 || record.ssi_signo != SIGUSR1)
        return fail(21, "blocking read");
    if (now_ms() - started < 40) return fail(21, "read returned early");
    pthread_join(thread, 0);
    close(fd);
    return 0;
}

static int check_storm(void)
{
    static struct record records[32];
    union sigval value;
    int before = count_open_fds(), fd, i, accepted = 0, next = 1, usr1 = 0;
    fd = sfd(-1, bit(SIGUSR1) | bit(SIGRTMIN), SFD_NONBLOCK_FLAG);
    if (fd < 0) return fail(22, "signalfd");
    for (i = 0; i < 5000; ++i)
        if (kill(getpid(), SIGUSR1) != 0) return fail(22, "kill");
    for (i = 1; i <= 200; ++i) {
        value.sival_int = i;
        if (sigqueue(getpid(), SIGRTMIN, value) != 0) {
            if (errno == EAGAIN) break;
            return fail(22, "sigqueue");
        }
        accepted = i;
    }
    sleep_ms(SETTLE_MS);
    for (;;) {
        int got = read_records(fd, records, 32), k;
        if (got < 0) {
            if (errno == EAGAIN) break;
            return fail(22, "read");
        }
        for (k = 0; k < got; ++k) {
            if (records[k].ssi_signo == SIGUSR1) { ++usr1; continue; }
            if (records[k].ssi_signo != (uint32_t)SIGRTMIN ||
                    records[k].ssi_int != next) {
                fprintf(stderr, "signo %u int %d expected %d\n",
                    records[k].ssi_signo, records[k].ssi_int, next);
                return fail(22, "real-time records out of order");
            }
            ++next;
        }
    }
    if (usr1 != 1 || next - 1 != accepted) {
        fprintf(stderr, "SIGUSR1 records %d, SIGRTMIN records %d of %d\n",
            usr1, next - 1, accepted);
        return fail(22, "storm record counts");
    }
    close(fd);
    if (count_open_fds() != before) {
        fprintf(stderr, "open fds %d before %d\n", count_open_fds(), before);
        return fail(22, "fd count changed");
    }
    fprintf(stderr, "signalfd-basic storm: %d SIGRTMIN accepted\n", accepted);
    return 0;
}

/* The blocked set survives execve: a child that blocks SIGUSR1 (it
   inherits main's mask) and execs this fixture sees SIGUSR1 in the old
   set of its first sigprocmask. */
static int check_exec_mask(const char *self)
{
    int status;
    pid_t child = fork();
    if (child < 0) return fail(23, "fork");
    if (child == 0) {
        execl(self, self, "exec-mask", (char *)0);
        _exit(3);
    }
    if (waitpid(child, &status, 0) != child) return fail(23, "waitpid");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "exec'd image status %#x (exit 1: SIGUSR1 not in "
            "its mask, 3: exec failed)\n", status);
        return fail(23, "blocked set lost across execve");
    }
    return 0;
}

int main(int argc, char **argv)
{
    sigset_t blocked;
    int result;
    if (argc > 1 && strcmp(argv[1], "exec-mask") == 0) {
        sigset_t old;
        if (sigprocmask(SIG_BLOCK, 0, &old) != 0) return 2;
        return sigismember(&old, SIGUSR1) == 1 ? 0 : 1;
    }
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    sigaddset(&blocked, SIGUSR2);
    sigaddset(&blocked, SIGRTMIN);
    if (sigprocmask(SIG_BLOCK, &blocked, 0) != 0)
        return fail(10, "sigprocmask");
    arm(11);
    if ((result = check_basic()) != 0) return result;
    arm(12);
    if ((result = check_validation()) != 0) return result;
    arm(13);
    if ((result = check_coalesce()) != 0) return result;
    arm(14);
    if ((result = check_realtime()) != 0) return result;
    arm(15);
    if ((result = check_empty()) != 0) return result;
    arm(16);
    if ((result = check_readiness()) != 0) return result;
    arm(17);
    if ((result = check_fork_sender()) != 0) return result;
    arm(18);
    if ((result = check_two()) != 0) return result;
    arm(19);
    if ((result = check_update()) != 0) return result;
    arm(20);
    if ((result = check_unblockable()) != 0) return result;
    arm(21);
    if ((result = check_blocking()) != 0) return result;
    arm(22);
    if ((result = check_storm()) != 0) return result;
    arm(23);
    if ((result = check_exec_mask(argv[0])) != 0) return result;
    return 0;
}
