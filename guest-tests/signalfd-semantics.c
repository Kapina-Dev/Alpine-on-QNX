/*
 * Signal semantics around signalfd's process-wide queue (guest fixture,
 * musl): a signal Linuxemu's collector took must still be delivered
 * exactly as Linux would, with its handler, its default or its ignore
 * action, and every masked wait must see it.
 *
 * SIGUSR1, SIGUSR2 and SIGCHLD are blocked in main before any thread is
 * made, so every thread blocks them unless a check says otherwise.  A
 * check that needs a signal collected waits until its signalfd polls
 * readable (the collector moved it into the process queue).
 *
 * Checks (the exit code is the check number, also printed on stderr with
 * errno; a check that hangs is ended by the watchdog thread with exit code
 * 100 + the check number):
 *   10 setup failed
 *   23 consumed once: SIGUSR1 collected (blocked, a handler installed),
 *      then sigprocmask(SIG_UNBLOCK): the handler runs once and a
 *      signalfd read fails EAGAIN
 *   24 a thread with SIGUSR2 unblocked and a handler gets a
 *      process-directed SIGUSR2 while a signalfd watches it; the signalfd
 *      sees nothing
 *   25 default action: a fork child blocks SIGTERM, makes a signalfd for
 *      it, kills itself (collected), then unblocks SIGTERM: it dies of
 *      SIGTERM (WTERMSIG 15)
 *   26 ignored default: SIGCHLD collected, then unblocked with the
 *      default action: nothing happens, and the signal is gone (a
 *      signalfd read fails EAGAIN)
 *   27 masked waits: SIGUSR1 collected while blocked; ppoll, pselect6,
 *      epoll_pwait and sigsuspend with a temporary mask unblocking it each
 *      fail EINTR at once with the handler run
 *   28 a thread already in sigsuspend with SIGUSR1 unblocked is woken by
 *      a kill(SIGUSR1) that every other thread blocks
 *   29 thread-directed: thread B blocks SIGUSR1 and has a signalfd for
 *      it; tgkill(B, SIGUSR1); B's read returns it (no poll first: poll
 *      doesn't show a thread-directed signal, a documented limit)
 *   30 after the last signalfd for SIGUSR1 closes, a blocked SIGUSR1
 *      kill, then unblocking, runs the handler once
 *   31 thread B blocks in read on a signalfd and A closes it: B fails
 *      EBADF within 1 s; the collector no longer takes SIGUSR1 (as 30)
 *   32 a pending blocked signal survives execve: a fork child blocks
 *      SIGUSR1, raises it and execs this fixture; the new image starts,
 *      still blocks SIGUSR1, and unblocking it with a handler installed
 *      runs the handler once (exit 0; 1: handler count wrong, 2: SIGUSR1
 *      not blocked, 3: exec failed; a signal death means the start-up
 *      unblocked it)
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
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NR_SIGNALFD4 355
#define SFD_NONBLOCK_FLAG 0x800
#define RECORD 128
#define SETTLE_MS 30

struct record {
    uint32_t ssi_signo;
    int32_t ssi_errno;
    int32_t ssi_code;
    uint32_t ssi_pid;
    uint8_t rest[RECORD - 16];
};

static volatile int current_check;
static volatile int watchdog_ticks;
static volatile int usr1_count, usr2_count, usr1_tid, usr2_tid;

static int fail(int check, const char *what)
{
    fprintf(stderr, "signalfd-semantics check %d failed: %s (errno %d %s)\n",
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

static void *watchdog(void *argument)
{
    (void)argument;
    for (;;) {
        sleep_ms(100);
        if (__sync_add_and_fetch(&watchdog_ticks, 1) >= 100) {
            fprintf(stderr, "signalfd-semantics check %d timed out\n",
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
    if (getenv("SIGNALFD_SEMANTICS_VERBOSE") != 0)
        fprintf(stderr, "signalfd-semantics check %d\n", check);
    if (!started) {
        pthread_t thread;
        if (pthread_create(&thread, 0, watchdog, 0) == 0) started = 1;
    }
}

static int gettid_now(void)
{
    return (int)syscall(SYS_gettid);
}

static void on_usr1(int signal_number)
{
    (void)signal_number;
    usr1_tid = gettid_now();
    __sync_add_and_fetch(&usr1_count, 1);
}

static void on_usr2(int signal_number)
{
    (void)signal_number;
    usr2_tid = gettid_now();
    __sync_add_and_fetch(&usr2_count, 1);
}

static int install(int signal_number, void (*handler)(int))
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    return sigaction(signal_number, &action, 0);
}

static uint64_t bit(int signal_number)
{
    return (uint64_t)1 << (signal_number - 1);
}

static int sfd(int fd, uint64_t mask, int flags)
{
    return (int)syscall(NR_SIGNALFD4, fd, &mask, 8, flags);
}

static int change_mask(int how, int signal_number)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, signal_number);
    return sigprocmask(how, &set, 0);
}

/* The current mask without signal_number (a wait's temporary mask). */
static void mask_without(sigset_t *set, int signal_number)
{
    sigprocmask(SIG_BLOCK, 0, set);
    sigdelset(set, signal_number);
}

static int read_eagain(int fd)
{
    struct record record;
    errno = 0;
    return read(fd, &record, sizeof(record)) == -1 && errno == EAGAIN;
}

/* Waits up to 2 s for the signalfd to poll readable: the collector has
   the signal in the process queue. */
static int wait_collected(int fd)
{
    struct pollfd entry;
    entry.fd = fd;
    entry.events = POLLIN;
    entry.revents = 0;
    return poll(&entry, 1, 2000) == 1 && (entry.revents & POLLIN) != 0;
}

/* Waits up to 2 s for *counter to reach value. */
static int wait_count(volatile int *counter, int value)
{
    int64_t end = now_ms() + 2000;
    while (*counter < value && now_ms() < end) sleep_ms(5);
    return *counter >= value;
}

/* kill(getpid(), signal_number) and wait until fd shows it collected. */
static int send_collected(int fd, int signal_number)
{
    if (kill(getpid(), signal_number) != 0) return 0;
    return wait_collected(fd);
}

static int check_consumed_once(void)
{
    int fd = sfd(-1, bit(SIGUSR1), SFD_NONBLOCK_FLAG), before;
    if (fd < 0) return fail(23, "signalfd");
    before = usr1_count;
    if (!send_collected(fd, SIGUSR1)) return fail(23, "SIGUSR1 not collected");
    if (usr1_count != before) return fail(23, "handler ran while blocked");
    if (change_mask(SIG_UNBLOCK, SIGUSR1) != 0) return fail(23, "unblock");
    if (change_mask(SIG_BLOCK, SIGUSR1) != 0) return fail(23, "block");
    if (usr1_count != before + 1) {
        fprintf(stderr, "handler ran %d times\n", usr1_count - before);
        return fail(23, "handler not run exactly once on unblock");
    }
    if (!read_eagain(fd)) return fail(23, "signalfd still reads the signal");
    sleep_ms(SETTLE_MS);
    if (usr1_count != before + 1) return fail(23, "handler ran again");
    close(fd);
    return 0;
}

struct usr2_thread { volatile int ready, stop, tid; };

static void *usr2_receiver(void *argument)
{
    struct usr2_thread *state = argument;
    state->tid = gettid_now();
    change_mask(SIG_UNBLOCK, SIGUSR2);
    __sync_synchronize();
    state->ready = 1;
    while (!state->stop) sleep_ms(5);
    change_mask(SIG_BLOCK, SIGUSR2);
    return 0;
}

static int check_unblocked_thread(void)
{
    struct usr2_thread state;
    pthread_t thread;
    int fd = sfd(-1, bit(SIGUSR2), SFD_NONBLOCK_FLAG), before = usr2_count;
    if (fd < 0) return fail(24, "signalfd");
    memset((void *)&state, 0, sizeof(state));
    if (pthread_create(&thread, 0, usr2_receiver, (void *)&state) != 0)
        return fail(24, "pthread_create");
    while (!state.ready) sleep_ms(1);
    if (kill(getpid(), SIGUSR2) != 0) return fail(24, "kill");
    if (!wait_count(&usr2_count, before + 1))
        return fail(24, "handler did not run in the unblocked thread");
    if (usr2_tid != state.tid) {
        fprintf(stderr, "handler tid %d thread %d\n", usr2_tid, state.tid);
        return fail(24, "handler ran in another thread");
    }
    sleep_ms(SETTLE_MS);
    if (!read_eagain(fd)) return fail(24, "signalfd saw the signal");
    if (usr2_count != before + 1) return fail(24, "handler ran twice");
    state.stop = 1;
    pthread_join(thread, 0);
    close(fd);
    return 0;
}

static int check_default_terminates(void)
{
    int status;
    pid_t child = fork();
    if (child < 0) return fail(25, "fork");
    if (child == 0) {
        int fd;
        if (change_mask(SIG_BLOCK, SIGTERM) != 0) _exit(2);
        fd = sfd(-1, bit(SIGTERM), SFD_NONBLOCK_FLAG);
        if (fd < 0) _exit(3);
        if (!send_collected(fd, SIGTERM)) _exit(4);
        change_mask(SIG_UNBLOCK, SIGTERM);
        sleep_ms(200);
        _exit(1);
    }
    if (waitpid(child, &status, 0) != child) return fail(25, "waitpid");
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM) {
        fprintf(stderr, "child status %#x (exit 1: survived the unblock, "
            "2-4: setup)\n", status);
        return fail(25, "collected SIGTERM did not end the child");
    }
    return 0;
}

static int check_default_ignored(void)
{
    int fd = sfd(-1, bit(SIGCHLD), SFD_NONBLOCK_FLAG);
    if (fd < 0) return fail(26, "signalfd");
    if (signal(SIGCHLD, SIG_DFL) == SIG_ERR) return fail(26, "signal");
    while (!read_eagain(fd) && errno == 0) {}
    if (!send_collected(fd, SIGCHLD)) return fail(26, "SIGCHLD not collected");
    if (change_mask(SIG_UNBLOCK, SIGCHLD) != 0) return fail(26, "unblock");
    sleep_ms(SETTLE_MS);
    if (change_mask(SIG_BLOCK, SIGCHLD) != 0) return fail(26, "block");
    if (!read_eagain(fd))
        return fail(26, "SIGCHLD still pending after its ignored delivery");
    close(fd);
    return 0;
}

static int sys_epoll_pwait(int ep, struct epoll_event *events, int count,
    int timeout, const sigset_t *mask)
{
    return (int)syscall(SYS_epoll_pwait, ep, events, count, timeout, mask, 8);
}

/* One masked wait: 0 ppoll, 1 pselect6, 2 epoll_pwait, 3 sigsuspend. */
static int masked_wait(int kind, int idle_fd, int ep)
{
    sigset_t temporary;
    mask_without(&temporary, SIGUSR1);
    errno = 0;
    if (kind == 0) {
        struct pollfd entry;
        struct timespec timeout = { 5, 0 };
        entry.fd = idle_fd;
        entry.events = POLLIN;
        entry.revents = 0;
        return ppoll(&entry, 1, &timeout, &temporary);
    }
    if (kind == 1) {
        fd_set set;
        struct timespec timeout = { 5, 0 };
        FD_ZERO(&set);
        FD_SET(idle_fd, &set);
        return pselect(idle_fd + 1, &set, 0, 0, &timeout, &temporary);
    }
    if (kind == 2) {
        struct epoll_event event;
        return sys_epoll_pwait(ep, &event, 1, 5000, &temporary);
    }
    return sigsuspend(&temporary);
}

static int check_masked_waits(void)
{
    static const char *names[4] = { "ppoll", "pselect6", "epoll_pwait",
        "sigsuspend" };
    struct epoll_event event;
    int pipe_fds[2], ep, kind;
    int fd = sfd(-1, bit(SIGUSR1), SFD_NONBLOCK_FLAG);
    if (fd < 0) return fail(27, "signalfd");
    if (pipe(pipe_fds) != 0) return fail(27, "pipe");
    ep = epoll_create1(0);
    if (ep < 0) return fail(27, "epoll_create1");
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, pipe_fds[0], &event) != 0)
        return fail(27, "epoll_ctl");
    for (kind = 0; kind < 4; ++kind) {
        int before = usr1_count, result, saved;
        int64_t start;
        if (!send_collected(fd, SIGUSR1)) {
            fprintf(stderr, "before %s\n", names[kind]);
            return fail(27, "SIGUSR1 not collected");
        }
        if (getenv("SIGNALFD_SEMANTICS_VERBOSE") != 0)
            fprintf(stderr, "signalfd-semantics check 27: %s\n", names[kind]);
        start = now_ms();
        result = masked_wait(kind, pipe_fds[0], ep);
        saved = errno;
        if (result != -1 || saved != EINTR || usr1_count != before + 1 ||
                now_ms() - start > 1000) {
            fprintf(stderr, "%s returned %d errno %d handler %d after "
                "%lld ms\n", names[kind], result, saved,
                usr1_count - before, (long long)(now_ms() - start));
            errno = saved;
            return fail(27, "masked wait did not take the queued signal");
        }
        if (!read_eagain(fd)) {
            fprintf(stderr, "after %s\n", names[kind]);
            return fail(27, "signalfd still reads the delivered signal");
        }
    }
    close(ep);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    close(fd);
    return 0;
}

struct suspender { volatile int ready, tid, result, error; };

static void *suspend_thread(void *argument)
{
    struct suspender *state = argument;
    sigset_t temporary;
    state->tid = gettid_now();
    mask_without(&temporary, SIGUSR1);
    __sync_synchronize();
    state->ready = 1;
    errno = 0;
    state->result = sigsuspend(&temporary);
    state->error = errno;
    return 0;
}

static int check_suspend_woken(void)
{
    struct suspender state;
    pthread_t thread;
    int fd = sfd(-1, bit(SIGUSR1), SFD_NONBLOCK_FLAG), before = usr1_count;
    if (fd < 0) return fail(28, "signalfd");
    memset((void *)&state, 0, sizeof(state));
    if (pthread_create(&thread, 0, suspend_thread, (void *)&state) != 0)
        return fail(28, "pthread_create");
    while (!state.ready) sleep_ms(1);
    sleep_ms(100);
    if (kill(getpid(), SIGUSR1) != 0) return fail(28, "kill");
    pthread_join(thread, 0);
    if (state.result != -1 || state.error != EINTR ||
            usr1_count != before + 1 || usr1_tid != state.tid) {
        fprintf(stderr, "sigsuspend %d errno %d handler %d in %d (thread "
            "%d)\n", state.result, state.error, usr1_count - before,
            usr1_tid, state.tid);
        return fail(28, "sigsuspend not woken by the queued signal");
    }
    if (!read_eagain(fd)) return fail(28, "signalfd saw the signal");
    close(fd);
    return 0;
}

struct directed { volatile int ready, go, tid, got, error; uint32_t signo; };

static void *directed_reader(void *argument)
{
    struct directed *state = argument;
    struct record record;
    int fd = sfd(-1, bit(SIGUSR1), SFD_NONBLOCK_FLAG);
    state->tid = gettid_now();
    __sync_synchronize();
    state->ready = 1;
    if (fd < 0) { state->got = -2; state->error = errno; return 0; }
    while (!state->go) sleep_ms(1);
    errno = 0;
    state->got = (int)read(fd, &record, sizeof(record));
    state->error = errno;
    state->signo = record.ssi_signo;
    if (state->got == RECORD && !read_eagain(fd)) state->got = -3;
    close(fd);
    return 0;
}

static int check_thread_directed(void)
{
    struct directed state;
    pthread_t thread;
    int before = usr1_count;
    memset((void *)&state, 0, sizeof(state));
    if (pthread_create(&thread, 0, directed_reader, (void *)&state) != 0)
        return fail(29, "pthread_create");
    while (!state.ready) sleep_ms(1);
    if (syscall(SYS_tgkill, getpid(), state.tid, SIGUSR1) != 0)
        return fail(29, "tgkill");
    sleep_ms(SETTLE_MS);
    state.go = 1;
    pthread_join(thread, 0);
    if (state.got != RECORD || state.signo != (uint32_t)SIGUSR1) {
        fprintf(stderr, "read %d errno %d signo %u (-2 signalfd, -3 second "
            "record)\n", state.got, state.error, state.signo);
        errno = state.error;
        return fail(29, "thread-directed signal not read by its thread");
    }
    if (usr1_count != before) return fail(29, "handler ran");
    return 0;
}

/* The collector no longer takes SIGUSR1: a blocked kill, then unblocking,
   runs the handler once. */
static int after_close_delivers(int check)
{
    int before = usr1_count;
    sleep_ms(SETTLE_MS);
    if (kill(getpid(), SIGUSR1) != 0) return fail(check, "kill");
    sleep_ms(SETTLE_MS);
    if (usr1_count != before) return fail(check, "handler ran while blocked");
    if (change_mask(SIG_UNBLOCK, SIGUSR1) != 0) return fail(check, "unblock");
    if (change_mask(SIG_BLOCK, SIGUSR1) != 0) return fail(check, "block");
    if (usr1_count != before + 1) {
        fprintf(stderr, "handler ran %d times\n", usr1_count - before);
        return fail(check, "SIGUSR1 not delivered once after the close");
    }
    return 0;
}

static int check_last_close(void)
{
    int fd = sfd(-1, bit(SIGUSR1), SFD_NONBLOCK_FLAG);
    if (fd < 0) return fail(30, "signalfd");
    close(fd);
    return after_close_delivers(30);
}

struct blocked { int fd; volatile int done; ssize_t got; int error;
    int64_t when; };

static void *blocked_reader(void *argument)
{
    struct blocked *state = argument;
    struct record record;
    errno = 0;
    state->got = read(state->fd, &record, sizeof(record));
    state->error = errno;
    state->when = now_ms();
    __sync_synchronize();
    state->done = 1;
    return 0;
}

static int check_close_blocked(void)
{
    struct blocked state;
    pthread_t thread;
    int64_t closed;
    memset(&state, 0, sizeof(state));
    state.fd = sfd(-1, bit(SIGUSR1), 0);
    if (state.fd < 0) return fail(31, "signalfd");
    if (pthread_create(&thread, 0, blocked_reader, &state) != 0)
        return fail(31, "pthread_create");
    sleep_ms(100);
    closed = now_ms();
    close(state.fd);
    pthread_join(thread, 0);
    if (state.got != -1 || state.error != EBADF ||
            state.when - closed > 1100) {
        fprintf(stderr, "got %d errno %d %lld ms after the close\n",
            (int)state.got, state.error, (long long)(state.when - closed));
        errno = state.error;
        return fail(31, "blocked read not EBADF within 1 s of close");
    }
    return after_close_delivers(31);
}

static int exec_pending_image(void)
{
    sigset_t old;
    if (sigprocmask(SIG_BLOCK, 0, &old) != 0 || sigismember(&old, SIGUSR1) != 1)
        return 2;
    if (install(SIGUSR1, on_usr1) != 0) return 2;
    change_mask(SIG_UNBLOCK, SIGUSR1);
    sleep_ms(SETTLE_MS);
    return usr1_count == 1 ? 0 : 1;
}

static int check_exec_pending(const char *self)
{
    int status;
    pid_t child = fork();
    if (child < 0) return fail(32, "fork");
    if (child == 0) {
        kill(getpid(), SIGUSR1);
        execl(self, self, "exec-pending", (char *)0);
        _exit(3);
    }
    if (waitpid(child, &status, 0) != child) return fail(32, "waitpid");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "exec'd image status %#x (exit 1: handler count, "
            "2: SIGUSR1 not blocked, 3: exec failed; signal %d)\n", status,
            WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        return fail(32, "pending blocked signal lost or fatal across execve");
    }
    return 0;
}

static int run_check(int check, const char *self)
{
    switch (check) {
    case 23: return check_consumed_once();
    case 24: return check_unblocked_thread();
    case 25: return check_default_terminates();
    case 26: return check_default_ignored();
    case 27: return check_masked_waits();
    case 28: return check_suspend_woken();
    case 29: return check_thread_directed();
    case 30: return check_last_close();
    case 31: return check_close_blocked();
    default: return check_exec_pending(self);
    }
}

/* SIGNALFD_SEMANTICS_ONLY=N runs check N alone (the checks don't depend
   on each other). */
int main(int argc, char **argv)
{
    sigset_t blocked;
    const char *only_text = getenv("SIGNALFD_SEMANTICS_ONLY");
    int result, check, only = only_text == 0 ? 0 : atoi(only_text);
    if (argc > 1 && strcmp(argv[1], "exec-pending") == 0)
        return exec_pending_image();
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    sigaddset(&blocked, SIGUSR2);
    sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, 0) != 0)
        return fail(10, "sigprocmask");
    if (install(SIGUSR1, on_usr1) != 0 || install(SIGUSR2, on_usr2) != 0)
        return fail(10, "sigaction");
    for (check = 23; check <= 32; ++check) {
        if (only != 0 && check != only) continue;
        arm(check);
        if ((result = run_check(check, argv[0])) != 0) return result;
    }
    return 0;
}
