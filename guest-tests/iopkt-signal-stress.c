/*
 * Signal-wait channel against io-pkt: a bounded stress test (guest
 * fixture, musl, pthreads), run by scripts/run-iopkt-stress.sh.
 *
 * WARNING: THIS CAN CRASH io-pkt ON BB10.  Its first 10 s run on the
 * Passport (2026-11-19, docs/FINDINGS.md) killed io-pkt and the network
 * stayed down until reboot.  Not part of the regression; run it only with
 * someone at the phone to reboot it.  Kept to isolate that crash.
 *
 * It was written when each guest thread's signal-wait channel was a
 * self-connected loopback UDP socket that the emulator's host signal
 * handler write()s during a masked wait (ppoll/epoll_pwait with a mask, a
 * blocking eventfd read), so a handler could send an io-pkt message while
 * the same thread was inside its own io-pkt call.  (The channel is a pipe
 * again since that crash.)  It keeps those windows and guest socket calls
 * under signals busy while a signaller tgkills every live thread about
 * every 100 us:
 *
 *   waiters (8)      ppoll 1 ms and 200 ms, epoll_pwait 1 ms and 200 ms
 *                    with SIGUSR1 unblocked only by the call's mask, and a
 *                    blocking eventfd read (SIGUSR1 unblocked) that a
 *                    feeder thread writes; SIGUSR1 has no SA_RESTART
 *   AF_UNIX (2x2)    stream socketpair ping-pong; every message carries a
 *                    fresh pipe's write end via SCM_RIGHTS and a
 *                    sequence-numbered payload; the receiver writes a byte
 *                    through the fd and closes it, the sender reads the
 *                    byte and then EOF; waits are masked ppolls
 *   TCP (1x2)        loopback ping-pong, echoed payloads checked
 *   churn            a short-lived thread per iteration: masked ppoll, an
 *                    AF_UNIX bind/unlink on its own path, exit (often with
 *                    a signal in flight)
 *   bind             AF_UNIX path bind -> listen -> close -> unlink cycle
 *   SIGUSR2 (socket, bind threads) has SA_RESTART and stays unblocked, so
 *   it lands inside sendmsg/recvmsg/bind/unlink too.
 *
 * Usage: iopkt-signal-stress [SECONDS] (default 60).
 *
 * Exit codes (each failure also prints one stderr line naming it):
 *   0  every class progressed, no integrity failure, totals sane
 *   10 setup failed
 *   11 AF_UNIX payload mismatch, lost or extra fd, fd that doesn't work
 *   12 TCP payload mismatch
 *   13 unexpected errno from a call
 *   14 lost wakeup: a 200 ms masked wait timed out although at least 3
 *      signals were sent to the thread during it
 *   15 eventfd count mismatch (written != read + left)
 *   16 EINTR from a call that SA_RESTART should restart
 *   17 totals not sane (a signal kind never handled, more handled than
 *      sent)
 *   21 waiters stalled 10 s   22 AF_UNIX stalled   23 TCP stalled
 *   24 churn stalled          25 bind/unlink stalled 27 signaller stalled
 *   26 hard cap (alarm) fired 28 shutdown did not finish in 20 s
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define WAITERS 8
#define UNIX_PAIRS 2
#define SLOTS 32
#define MAGIC 0x10b4c3d1u
#define FIN_MAGIC 0x0f1e2d3cu
#define REPLY_MAGIC 0x5eedbeefu
#define MAX_PAYLOAD 1100

enum { CLASS_WAITERS, CLASS_UNIX, CLASS_TCP, CLASS_CHURN, CLASS_BIND,
    CLASS_SIGNALLER, CLASSES };
static const char *class_names[CLASSES] = { "waiters", "af_unix", "tcp",
    "churn", "bind_unlink", "signaller" };
static const int stall_codes[CLASSES] = { 21, 22, 23, 24, 25, 27 };

struct slot {
    volatile int tid;          /* 0: empty */
    volatile int signal_number;
    volatile int transient;    /* ESRCH allowed (churn threads) */
    volatile unsigned sent;
};

static struct slot slots[SLOTS];
static volatile int stop;
static volatile int signaller_stop;
static volatile int failure_code;
static volatile unsigned progress[CLASSES];
static volatile unsigned handled_usr1, handled_usr2;
static volatile unsigned sent_usr1, sent_usr2, sent_esrch;
static volatile unsigned waiter_eintr, waiter_timeouts, waiter_long_eintr;
static volatile unsigned waiter_long_timeouts, waiter_long_quiet;
static volatile unsigned eventfd_reads, eventfd_eintr;
static volatile unsigned long long eventfd_written, eventfd_read_total;
static volatile unsigned socket_poll_eintr, restart_eintr;
static volatile unsigned fds_passed, unix_round_trips, tcp_round_trips;
static volatile unsigned churn_threads, churn_binds, bind_cycles;
static volatile int live_threads;
static int event_fds[WAITERS];
static pid_t process_id;
static char tmp_prefix[64];
static __thread struct slot *my_slot;

static int gettid_(void) { return (int)syscall(SYS_gettid); }

static void fail(int code, const char *what)
{
    int saved = errno;
    fprintf(stderr, "iopkt-signal-stress failure %d: %s (errno %d %s)\n",
        code, what, saved, strerror(saved));
    __sync_bool_compare_and_swap(&failure_code, 0, code);
    stop = 1;
}

static void handler(int signal_number)
{
    int saved = errno;
    if (signal_number == SIGUSR1) __sync_fetch_and_add(&handled_usr1, 1);
    else __sync_fetch_and_add(&handled_usr2, 1);
    errno = saved;
}

static struct slot *claim_slot(int signal_number, int transient)
{
    int i;
    for (i = 0; i != SLOTS; ++i) {
        if (__sync_bool_compare_and_swap(&slots[i].tid, 0, -1)) {
            slots[i].signal_number = signal_number;
            slots[i].transient = transient;
            slots[i].sent = 0;
            __sync_synchronize();
            slots[i].tid = gettid_();
            my_slot = &slots[i];
            return &slots[i];
        }
    }
    return 0;
}

static void release_slot(struct slot *slot)
{
    if (slot != 0) { __sync_synchronize(); slot->tid = 0; }
}

static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static void sleep_us(long us)
{
    struct timespec t = { us / 1000000, (us % 1000000) * 1000 };
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {}
}

static void sigset_without(sigset_t *set, int unblocked)
{
    sigemptyset(set);
    sigaddset(set, SIGUSR1);
    sigaddset(set, SIGUSR2);
    sigdelset(set, unblocked);
}

static void unblock(int signal_number)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, signal_number);
    pthread_sigmask(SIG_UNBLOCK, &set, 0);
}

/* ---------------------------------------------------------------- waiters */

static void check_long_wait(int result, unsigned before, const char *what)
{
    if (result < 0 && errno == EINTR) {
        __sync_fetch_and_add(&waiter_long_eintr, 1);
    } else if (result == 0) {
        unsigned during = my_slot->sent - before;
        __sync_fetch_and_add(&waiter_long_timeouts, 1);
        if (during >= 3) {
            char message[96];
            snprintf(message, sizeof(message),
                "%s timed out with %u signals sent during it", what, during);
            errno = 0;
            fail(14, message);
        } else __sync_fetch_and_add(&waiter_long_quiet, 1);
    } else {
        fail(13, what);
    }
}

static void *waiter(void *argument)
{
    int index = (int)(intptr_t)argument;
    int efd = event_fds[index], epfd, empty[2];
    struct epoll_event event, events[4];
    sigset_t mask;
    struct timespec short_wait = { 0, 1000000 }, long_wait = { 0, 200000000 };
    struct slot *slot = claim_slot(SIGUSR1, 0);
    if (slot == 0) { errno = 0; fail(10, "no slot for waiter"); return 0; }
    sigset_without(&mask, SIGUSR1);
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0 || pipe(empty) != 0) {
        fail(10, "waiter epoll/pipe"); release_slot(slot); return 0;
    }
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, empty[0], &event) != 0) {
        fail(10, "waiter epoll_ctl"); release_slot(slot); return 0;
    }
    while (!stop) {
        int r;
        unsigned before;
        uint64_t value;

        r = ppoll(0, 0, &short_wait, &mask);
        if (r < 0 && errno == EINTR) __sync_fetch_and_add(&waiter_eintr, 1);
        else if (r == 0) __sync_fetch_and_add(&waiter_timeouts, 1);
        else { fail(13, "ppoll 1 ms"); break; }

        r = epoll_pwait(epfd, events, 4, 1, &mask);
        if (r < 0 && errno == EINTR) __sync_fetch_and_add(&waiter_eintr, 1);
        else if (r == 0) __sync_fetch_and_add(&waiter_timeouts, 1);
        else { fail(13, "epoll_pwait 1 ms"); break; }

        before = slot->sent;
        r = ppoll(0, 0, &long_wait, &mask);
        check_long_wait(r, before, "ppoll 200 ms");
        if (stop) break;

        before = slot->sent;
        r = epoll_pwait(epfd, events, 4, 200, &mask);
        check_long_wait(r, before, "epoll_pwait 200 ms");
        if (stop) break;

        unblock(SIGUSR1);
        r = (int)read(efd, &value, sizeof(value));
        {
            int saved = errno;
            sigset_t usr1;
            sigemptyset(&usr1);
            sigaddset(&usr1, SIGUSR1);
            pthread_sigmask(SIG_BLOCK, &usr1, 0);
            errno = saved;
        }
        if (r == (int)sizeof(value)) {
            __sync_fetch_and_add(&eventfd_reads, 1);
            __sync_fetch_and_add(&eventfd_read_total, value);
        } else if (r < 0 && errno == EINTR) {
            __sync_fetch_and_add(&eventfd_eintr, 1);
        } else { fail(13, "eventfd read"); break; }

        __sync_fetch_and_add(&progress[CLASS_WAITERS], 1);
    }
    close(epfd); close(empty[0]); close(empty[1]);
    release_slot(slot);
    return 0;
}

static void *feeder(void *argument)
{
    uint64_t one = 1;
    int i;
    (void)argument;
    while (!stop) {
        for (i = 0; i != WAITERS; ++i) {
            if (write(event_fds[i], &one, sizeof(one)) == sizeof(one))
                __sync_fetch_and_add(&eventfd_written, 1);
            else if (errno != EINTR) { fail(13, "eventfd write"); return 0; }
        }
        sleep_us(300);
    }
    /* Wake any waiter still blocked in its read. */
    for (i = 0; i != WAITERS; ++i)
        if (write(event_fds[i], &one, sizeof(one)) == sizeof(one))
            __sync_fetch_and_add(&eventfd_written, 1);
    return 0;
}

/* ------------------------------------------------------------ socket I/O */

/* Waits for fd readable with SIGUSR2 unblocked by the mask (a masked wait
   between socket calls).  Returns 1 readable, 0 stop, -1 failure. */
static int wait_readable(int fd)
{
    struct pollfd p = { fd, POLLIN, 0 };
    struct timespec wait = { 0, 100000000 };
    sigset_t mask;
    sigset_without(&mask, SIGUSR2);
    for (;;) {
        int r = ppoll(&p, 1, &wait, &mask);
        if (r > 0) return 1;
        if (r < 0 && errno == EINTR) {
            __sync_fetch_and_add(&socket_poll_eintr, 1);
            continue;
        }
        if (r < 0) { fail(13, "socket ppoll"); return -1; }
        if (stop) return 0;
    }
}

static int send_all(int fd, const void *buffer, size_t length,
    int pass_fd)
{
    const char *bytes = buffer;
    size_t done = 0;
    while (done < length) {
        ssize_t n;
        if (pass_fd >= 0) {
            struct msghdr message;
            struct iovec iov = { (void *)bytes, length };
            union { struct cmsghdr header; char space[CMSG_SPACE(sizeof(int))]; }
                control;
            struct cmsghdr *cmsg;
            memset(&message, 0, sizeof(message));
            memset(&control, 0, sizeof(control));
            message.msg_iov = &iov;
            message.msg_iovlen = 1;
            message.msg_control = control.space;
            message.msg_controllen = sizeof(control.space);
            cmsg = CMSG_FIRSTHDR(&message);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cmsg), &pass_fd, sizeof(int));
            n = sendmsg(fd, &message, MSG_NOSIGNAL);
            if (n > 0) pass_fd = -1;
        } else n = send(fd, bytes + done, length - done, MSG_NOSIGNAL);
        if (n > 0) { done += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) {
            __sync_fetch_and_add(&restart_eintr, 1);
            continue;
        }
        return -1;
    }
    return 0;
}

/* Receives exactly length bytes; *received_fd gets an SCM_RIGHTS fd if
   one comes (more than one, or one on a later chunk, fails).  Returns 1,
   0 on stop or EOF, -1 on failure (already reported). */
static int recv_all(int fd, void *buffer, size_t length, int *received_fd,
    int failure)
{
    char *bytes = buffer;
    size_t done = 0;
    if (received_fd != 0) *received_fd = -1;
    while (done < length) {
        struct msghdr message;
        struct iovec iov = { bytes + done, length - done };
        union { struct cmsghdr header; char space[CMSG_SPACE(4 * sizeof(int))]; }
            control;
        struct cmsghdr *cmsg;
        ssize_t n;
        int w = wait_readable(fd);
        if (w <= 0) return w;
        memset(&message, 0, sizeof(message));
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        message.msg_control = control.space;
        message.msg_controllen = sizeof(control.space);
        n = recvmsg(fd, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) {
            __sync_fetch_and_add(&restart_eintr, 1);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n < 0) { fail(13, "recvmsg"); return -1; }
        if (n == 0) return 0;
        if (message.msg_flags & MSG_CTRUNC) {
            errno = 0; fail(failure, "control truncated"); return -1;
        }
        for (cmsg = CMSG_FIRSTHDR(&message); cmsg != 0;
                cmsg = CMSG_NXTHDR(&message, cmsg)) {
            int count;
            if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
                continue;
            count = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            if (received_fd == 0 || count != 1 || *received_fd >= 0) {
                errno = 0; fail(failure, "unexpected SCM_RIGHTS fds");
                return -1;
            }
            memcpy(received_fd, CMSG_DATA(cmsg), sizeof(int));
        }
        done += (size_t)n;
    }
    return 1;
}

static size_t payload_length(uint32_t seq)
{
    return 12 + (seq * 37u) % 1009u;
}

static void fill_payload(unsigned char *buffer, uint32_t magic, uint32_t seq,
    uint32_t length)
{
    uint32_t i;
    memcpy(buffer, &magic, 4);
    memcpy(buffer + 4, &seq, 4);
    memcpy(buffer + 8, &length, 4);
    for (i = 12; i < length; ++i)
        buffer[i] = (unsigned char)(seq * 13u + i * 7u);
}

static int check_payload(const unsigned char *buffer, uint32_t seq,
    uint32_t length)
{
    uint32_t i;
    for (i = 12; i < length; ++i)
        if (buffer[i] != (unsigned char)(seq * 13u + i * 7u)) return -1;
    return 0;
}

struct pair { int fd; int tcp; };

static void *unix_ping(void *argument)
{
    struct pair *pair = argument;
    unsigned char buffer[MAX_PAYLOAD];
    struct slot *slot = claim_slot(SIGUSR2, 0);
    uint32_t seq;
    if (slot == 0) { errno = 0; fail(10, "no slot"); return 0; }
    unblock(SIGUSR2);
    for (seq = 1; !stop; ++seq) {
        int p[2], r;
        uint32_t length = (uint32_t)payload_length(seq), reply[2];
        unsigned char byte;
        if (pipe2(p, O_CLOEXEC) != 0) { fail(13, "pipe2"); break; }
        fill_payload(buffer, MAGIC, seq, length);
        if (send_all(pair->fd, buffer, length, p[1]) != 0) {
            fail(13, "unix sendmsg"); close(p[0]); close(p[1]); break;
        }
        close(p[1]);
        __sync_fetch_and_add(&fds_passed, 1);
        r = recv_all(pair->fd, reply, sizeof(reply), 0, 11);
        if (r <= 0) {
            close(p[0]);
            if (r == 0 && !stop) { errno = 0; fail(11, "unix reply EOF"); }
            break;
        }
        if (reply[0] != REPLY_MAGIC || reply[1] != seq) {
            errno = 0; fail(11, "unix reply sequence"); close(p[0]); break;
        }
        while ((r = (int)read(p[0], &byte, 1)) < 0 && errno == EINTR)
            __sync_fetch_and_add(&restart_eintr, 1);
        if (r != 1 || byte != (unsigned char)seq) {
            fail(11, "byte through passed fd missing or wrong");
            close(p[0]); break;
        }
        while ((r = (int)read(p[0], &byte, 1)) < 0 && errno == EINTR)
            __sync_fetch_and_add(&restart_eintr, 1);
        if (r != 0) {
            fail(11, "passed fd not closed by receiver (no EOF)");
            close(p[0]); break;
        }
        close(p[0]);
        __sync_fetch_and_add(&unix_round_trips, 1);
        __sync_fetch_and_add(&progress[CLASS_UNIX], 1);
    }
    fill_payload(buffer, FIN_MAGIC, 0, 12);
    send_all(pair->fd, buffer, 12, -1);
    release_slot(slot);
    return 0;
}

static void *unix_pong(void *argument)
{
    struct pair *pair = argument;
    unsigned char buffer[MAX_PAYLOAD];
    struct slot *slot = claim_slot(SIGUSR2, 0);
    uint32_t expected;
    if (slot == 0) { errno = 0; fail(10, "no slot"); return 0; }
    unblock(SIGUSR2);
    for (expected = 1;; ++expected) {
        uint32_t magic, seq, length, reply[2];
        int fd, r;
        unsigned char byte;
        r = recv_all(pair->fd, buffer, 12, &fd, 11);
        if (r <= 0) {
            if (r == 0 && !stop) { errno = 0; fail(11, "unix pong EOF"); }
            break;
        }
        memcpy(&magic, buffer, 4);
        memcpy(&seq, buffer + 4, 4);
        memcpy(&length, buffer + 8, 4);
        if (magic == FIN_MAGIC) { if (fd >= 0) close(fd); break; }
        if (magic != MAGIC || seq != expected || length < 12 ||
                length > MAX_PAYLOAD || length != payload_length(seq)) {
            errno = 0; fail(11, "unix header mismatch");
            if (fd >= 0) close(fd);
            break;
        }
        if (length > 12) {
            int extra = -1;
            r = recv_all(pair->fd, buffer + 12, length - 12, &extra, 11);
            if (extra >= 0) {
                if (fd < 0) fd = extra;   /* rights may ride a later byte */
                else { close(extra); errno = 0;
                    fail(11, "second fd in one message"); break; }
            }
            if (r <= 0) {
                if (fd >= 0) close(fd);
                if (r == 0 && !stop) { errno = 0; fail(11, "unix body EOF"); }
                break;
            }
        }
        if (check_payload(buffer, seq, length) != 0) {
            errno = 0; fail(11, "unix payload mismatch");
            if (fd >= 0) close(fd);
            break;
        }
        if (fd < 0) { errno = 0; fail(11, "SCM_RIGHTS fd lost"); break; }
        byte = (unsigned char)seq;
        while ((r = (int)write(fd, &byte, 1)) < 0 && errno == EINTR)
            __sync_fetch_and_add(&restart_eintr, 1);
        if (r != 1) { fail(11, "write through passed fd"); close(fd); break; }
        close(fd);
        reply[0] = REPLY_MAGIC;
        reply[1] = seq;
        if (send_all(pair->fd, reply, sizeof(reply), -1) != 0) {
            fail(13, "unix reply send"); break;
        }
    }
    release_slot(slot);
    return 0;
}

static void *tcp_ping(void *argument)
{
    struct pair *pair = argument;
    unsigned char buffer[MAX_PAYLOAD], echo[MAX_PAYLOAD];
    struct slot *slot = claim_slot(SIGUSR2, 0);
    uint32_t seq;
    if (slot == 0) { errno = 0; fail(10, "no slot"); return 0; }
    unblock(SIGUSR2);
    for (seq = 1; !stop; ++seq) {
        uint32_t length = (uint32_t)payload_length(seq * 3u);
        int r;
        fill_payload(buffer, MAGIC, seq, length);
        if (send_all(pair->fd, buffer, length, -1) != 0) {
            fail(13, "tcp send"); break;
        }
        r = recv_all(pair->fd, echo, length, 0, 12);
        if (r <= 0) {
            if (r == 0 && !stop) { errno = 0; fail(12, "tcp echo EOF"); }
            break;
        }
        if (memcmp(buffer, echo, length) != 0) {
            errno = 0; fail(12, "tcp echo mismatch"); break;
        }
        __sync_fetch_and_add(&tcp_round_trips, 1);
        __sync_fetch_and_add(&progress[CLASS_TCP], 1);
    }
    fill_payload(buffer, FIN_MAGIC, 0, 12);
    send_all(pair->fd, buffer, 12, -1);
    release_slot(slot);
    return 0;
}

static void *tcp_pong(void *argument)
{
    struct pair *pair = argument;
    unsigned char buffer[MAX_PAYLOAD];
    struct slot *slot = claim_slot(SIGUSR2, 0);
    uint32_t expected;
    if (slot == 0) { errno = 0; fail(10, "no slot"); return 0; }
    unblock(SIGUSR2);
    for (expected = 1;; ++expected) {
        uint32_t magic, seq, length;
        int r = recv_all(pair->fd, buffer, 12, 0, 12);
        if (r <= 0) {
            if (r == 0 && !stop) { errno = 0; fail(12, "tcp pong EOF"); }
            break;
        }
        memcpy(&magic, buffer, 4);
        memcpy(&seq, buffer + 4, 4);
        memcpy(&length, buffer + 8, 4);
        if (magic == FIN_MAGIC) break;
        if (magic != MAGIC || seq != expected ||
                length != payload_length(seq * 3u)) {
            errno = 0; fail(12, "tcp header mismatch"); break;
        }
        if (length > 12) {
            r = recv_all(pair->fd, buffer + 12, length - 12, 0, 12);
            if (r <= 0) {
                if (r == 0 && !stop) { errno = 0; fail(12, "tcp body EOF"); }
                break;
            }
        }
        if (check_payload(buffer, seq, length) != 0) {
            errno = 0; fail(12, "tcp payload mismatch"); break;
        }
        if (send_all(pair->fd, buffer, length, -1) != 0) {
            fail(13, "tcp echo send"); break;
        }
    }
    release_slot(slot);
    return 0;
}

/* ----------------------------------------------------------------- churn */

static int unix_bind_cycle(const char *path, int do_listen)
{
    struct sockaddr_un address;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0), r;
    if (fd < 0) { fail(13, "unix socket"); return -1; }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
    while ((r = unlink(path)) != 0 && errno == EINTR)
        __sync_fetch_and_add(&restart_eintr, 1);
    while ((r = bind(fd, (struct sockaddr *)&address, sizeof(address))) != 0
            && errno == EINTR)
        __sync_fetch_and_add(&restart_eintr, 1);
    if (r != 0) { fail(13, "bind"); close(fd); return -1; }
    if (do_listen && listen(fd, 4) != 0) {
        fail(13, "listen"); close(fd); unlink(path); return -1;
    }
    close(fd);
    while ((r = unlink(path)) != 0 && errno == EINTR)
        __sync_fetch_and_add(&restart_eintr, 1);
    if (r != 0) { fail(13, "unlink"); return -1; }
    return 0;
}

static void *short_lived(void *argument)
{
    unsigned iteration = (unsigned)(uintptr_t)argument;
    struct slot *slot = claim_slot(SIGUSR1, 1);
    struct timespec wait = { 0, 1000000 };
    sigset_t mask;
    char path[96];
    int r;
    if (slot == 0) { errno = 0; fail(10, "no slot for churn"); return 0; }
    sigset_without(&mask, SIGUSR1);
    r = ppoll(0, 0, &wait, &mask);
    if (r < 0 && errno != EINTR) fail(13, "churn ppoll");
    snprintf(path, sizeof(path), "%s-c%u.sock", tmp_prefix, iteration & 7u);
    if (unix_bind_cycle(path, 0) == 0)
        __sync_fetch_and_add(&churn_binds, 1);
    r = ppoll(0, 0, &wait, &mask);
    if (r < 0 && errno != EINTR) fail(13, "churn ppoll 2");
    release_slot(slot);
    return 0;   /* exits with signals possibly still in flight */
}

static void *churn(void *argument)
{
    unsigned iteration;
    (void)argument;
    for (iteration = 0; !stop; ++iteration) {
        pthread_t thread;
        int error = pthread_create(&thread, 0, short_lived,
            (void *)(uintptr_t)iteration);
        if (error != 0) { errno = error; fail(13, "churn pthread_create"); break; }
        pthread_join(thread, 0);
        __sync_fetch_and_add(&churn_threads, 1);
        __sync_fetch_and_add(&progress[CLASS_CHURN], 1);
    }
    return 0;
}

static void *binder(void *argument)
{
    char path[96];
    struct slot *slot = claim_slot(SIGUSR2, 0);
    (void)argument;
    if (slot == 0) { errno = 0; fail(10, "no slot"); return 0; }
    unblock(SIGUSR2);
    snprintf(path, sizeof(path), "%s-bind.sock", tmp_prefix);
    while (!stop) {
        if (unix_bind_cycle(path, 1) != 0) break;
        __sync_fetch_and_add(&bind_cycles, 1);
        __sync_fetch_and_add(&progress[CLASS_BIND], 1);
    }
    release_slot(slot);
    return 0;
}

/* ------------------------------------------------------------- signaller */

static void *signaller(void *argument)
{
    uint64_t start = now_us();
    unsigned long long total = 0;
    int cursor = 0;
    (void)argument;
    while (!signaller_stop) {
        uint64_t due = (now_us() - start) / 100u;
        int burst = 0;
        while (total < due && burst < 64) {
            int i, tid = 0, sig = 0, transient = 0;
            struct slot *slot = 0;
            for (i = 0; i != SLOTS; ++i) {
                slot = &slots[(cursor + i) % SLOTS];
                tid = slot->tid;
                if (tid > 0) {
                    sig = slot->signal_number;
                    transient = slot->transient;
                    cursor = (cursor + i + 1) % SLOTS;
                    break;
                }
            }
            if (tid <= 0) break;
            if (syscall(SYS_tgkill, process_id, tid, sig) == 0) {
                __sync_fetch_and_add(&slot->sent, 1);
                if (sig == SIGUSR1) __sync_fetch_and_add(&sent_usr1, 1);
                else __sync_fetch_and_add(&sent_usr2, 1);
            } else if (errno == ESRCH && (transient || slot->tid != tid)) {
                __sync_fetch_and_add(&sent_esrch, 1);
            } else { fail(13, "tgkill"); return 0; }
            ++total;
            ++burst;
        }
        if (total < due) total = due;   /* don't build a backlog */
        __sync_fetch_and_add(&progress[CLASS_SIGNALLER], 1);
        sleep_us(100);
    }
    return 0;
}

/* -------------------------------------------------------------- watchdog */

static void *watchdog(void *argument)
{
    unsigned last[CLASSES];
    int idle[CLASSES], i, shutdown_seconds = 0;
    (void)argument;
    memset(idle, 0, sizeof(idle));
    for (i = 0; i != CLASSES; ++i) last[i] = progress[i];
    for (;;) {
        sleep_us(1000000);
        if (stop) {
            if (++shutdown_seconds >= 20) {
                fprintf(stderr, "iopkt-signal-stress failure 28: shutdown "
                    "hung (%d threads still running)\n", live_threads);
                _exit(28);
            }
            continue;
        }
        for (i = 0; i != CLASSES; ++i) {
            unsigned value = progress[i];
            if (value == last[i]) {
                if (++idle[i] >= 10) {
                    fprintf(stderr, "iopkt-signal-stress failure %d: %s made "
                        "no progress for 10 s (count %u)\n", stall_codes[i],
                        class_names[i], value);
                    _exit(stall_codes[i]);
                }
            } else idle[i] = 0;
            last[i] = value;
        }
    }
    return 0;
}

static void on_alarm(int signal_number)
{
    static const char message[] =
        "iopkt-signal-stress failure 26: hard cap alarm fired\n";
    (void)signal_number;
    write(2, message, sizeof(message) - 1);
    _exit(26);
}

/* ------------------------------------------------------------------ main */

struct worker { pthread_t thread; const char *name; };

static int tcp_pair(int fds[2])
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
    if (listener < 0) return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
            getsockname(listener, (struct sockaddr *)&address, &length) != 0 ||
            listen(listener, 1) != 0) { close(listener); return -1; }
    fds[0] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fds[0] < 0 || connect(fds[0], (struct sockaddr *)&address,
            sizeof(address)) != 0) { close(listener); return -1; }
    fds[1] = accept4(listener, 0, 0, SOCK_CLOEXEC);
    close(listener);
    if (fds[1] < 0) return -1;
    setsockopt(fds[0], IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fds[1], IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return 0;
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 60, i, n = 0, worker_count;
    struct worker workers[WAITERS + 2 * UNIX_PAIRS + 2 + 2];
    pthread_t signal_thread, watchdog_thread, feeder_thread;
    struct pair unix_pairs[UNIX_PAIRS][2], tcp_pairs[2];
    struct sigaction action;
    sigset_t blocked;
    unsigned long long left = 0;
    uint64_t started;
    int code;

    if (seconds <= 0) seconds = 60;
    process_id = getpid();
    snprintf(tmp_prefix, sizeof(tmp_prefix), "/tmp/iopkt-stress-%d",
        (int)process_id);
    setvbuf(stdout, 0, _IOLBF, 0);

    memset(&action, 0, sizeof(action));
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;                         /* waiters: EINTR */
    if (sigaction(SIGUSR1, &action, 0) != 0) { fail(10, "sigaction"); return 10; }
    action.sa_flags = SA_RESTART;                /* socket I/O, bind */
    if (sigaction(SIGUSR2, &action, 0) != 0) { fail(10, "sigaction"); return 10; }
    action.sa_handler = on_alarm;
    action.sa_flags = 0;
    sigaction(SIGALRM, &action, 0);
    signal(SIGPIPE, SIG_IGN);

    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    sigaddset(&blocked, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &blocked, 0);   /* every thread inherits */

    for (i = 0; i != WAITERS; ++i) {
        event_fds[i] = eventfd(0, EFD_CLOEXEC);
        if (event_fds[i] < 0) { fail(10, "eventfd"); return 10; }
    }
    for (i = 0; i != UNIX_PAIRS; ++i) {
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
            fail(10, "socketpair"); return 10;
        }
        unix_pairs[i][0].fd = fds[0];
        unix_pairs[i][1].fd = fds[1];
    }
    {
        int fds[2];
        if (tcp_pair(fds) != 0) { fail(10, "tcp loopback pair"); return 10; }
        tcp_pairs[0].fd = fds[0];
        tcp_pairs[1].fd = fds[1];
    }

    alarm((unsigned)seconds + 90);
    started = now_us();
#define START(fn, arg, label) do { \
        int error_ = pthread_create(&workers[n].thread, 0, fn, arg); \
        if (error_ != 0) { errno = error_; fail(10, "pthread_create"); \
            return 10; } \
        workers[n++].name = label; \
        __sync_fetch_and_add(&live_threads, 1); \
    } while (0)
    for (i = 0; i != WAITERS; ++i) START(waiter, (void *)(intptr_t)i, "waiter");
    for (i = 0; i != UNIX_PAIRS; ++i) {
        START(unix_ping, &unix_pairs[i][0], "unix_ping");
        START(unix_pong, &unix_pairs[i][1], "unix_pong");
    }
    START(tcp_ping, &tcp_pairs[0], "tcp_ping");
    START(tcp_pong, &tcp_pairs[1], "tcp_pong");
    START(churn, 0, "churn");
    START(binder, 0, "bind_unlink");
    worker_count = n;
    if (pthread_create(&feeder_thread, 0, feeder, 0) != 0 ||
            pthread_create(&signal_thread, 0, signaller, 0) != 0 ||
            pthread_create(&watchdog_thread, 0, watchdog, 0) != 0) {
        fail(10, "pthread_create"); return 10;
    }

    while (!stop && now_us() - started < (uint64_t)seconds * 1000000u)
        sleep_us(200000);
    stop = 1;
    for (i = 0; i != worker_count; ++i) {
        pthread_join(workers[i].thread, 0);
        __sync_fetch_and_add(&live_threads, -1);
    }
    pthread_join(feeder_thread, 0);
    signaller_stop = 1;
    pthread_join(signal_thread, 0);

    for (i = 0; i != WAITERS; ++i) {
        uint64_t value;
        int flags_fd = event_fds[i];
        struct pollfd p = { flags_fd, POLLIN, 0 };
        if (poll(&p, 1, 0) == 1 && read(flags_fd, &value, sizeof(value)) ==
                sizeof(value))
            left += value;
    }

    printf("iopkt-signal-stress seconds=%d elapsed_ms=%llu\n", seconds,
        (unsigned long long)((now_us() - started) / 1000u));
    printf("waiters: iterations=%u short_eintr=%u short_timeouts=%u "
        "long_eintr=%u long_timeouts=%u long_quiet_timeouts=%u\n",
        progress[CLASS_WAITERS], waiter_eintr, waiter_timeouts,
        waiter_long_eintr, waiter_long_timeouts, waiter_long_quiet);
    printf("eventfd: written=%llu read_total=%llu left=%llu reads=%u "
        "eintr=%u\n", eventfd_written, eventfd_read_total, left,
        eventfd_reads, eventfd_eintr);
    printf("af_unix: round_trips=%u fds_passed=%u\n", unix_round_trips,
        fds_passed);
    printf("tcp: round_trips=%u\n", tcp_round_trips);
    printf("socket: ppoll_eintr=%u sa_restart_eintr=%u\n", socket_poll_eintr,
        restart_eintr);
    printf("churn: threads=%u binds=%u  bind_unlink: cycles=%u\n",
        churn_threads, churn_binds, bind_cycles);
    printf("signals: sent_usr1=%u sent_usr2=%u esrch=%u handled_usr1=%u "
        "handled_usr2=%u\n", sent_usr1, sent_usr2, sent_esrch, handled_usr1,
        handled_usr2);

    code = failure_code;
    if (code == 0 && eventfd_written != eventfd_read_total + left) {
        errno = 0; fail(15, "eventfd written != read + left"); code = 15;
    }
    if (code == 0 && restart_eintr != 0) {
        errno = 0; fail(16, "EINTR despite SA_RESTART"); code = 16;
    }
    if (code == 0 && (handled_usr1 == 0 || handled_usr2 == 0 ||
            handled_usr1 > sent_usr1 || handled_usr2 > sent_usr2 ||
            progress[CLASS_WAITERS] == 0 || unix_round_trips == 0 ||
            tcp_round_trips == 0 || churn_threads == 0 || bind_cycles == 0)) {
        errno = 0; fail(17, "totals not sane"); code = 17;
    }
    printf("iopkt-signal-stress result=%d\n", code);
    return code;
}
