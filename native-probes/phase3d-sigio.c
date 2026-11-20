/*
 * Device facts that decide the Phase 3 part 4 build (socket async I/O).
 * Prints one <case>.<key>=<value> line per finding. Every case runs in its
 * own forked child with a 3 s alarm, so a hang is reported and the next case
 * still runs.
 *
 * Usage: phase3d-sigio                 run every case
 *        phase3d-sigio exec-check FD [self]   (internal, re-entered by exec)
 *
 * Per socket kind: fioasync, setown, getown (+ is_pid), getfl_async, sigio
 * (delivered? si_signo/si_code/si_value; QNX siginfo has no si_fd).
 * Extra cases on AF_UNIX SOCK_STREAM: osetfl (F_SETFL O_ASYNC alone),
 * peerclose, pgrp (F_SETOWN of -getpgrp()), fork, exec, exec-self, thread.
 */
#define _QNX_SOURCE 1
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef FIOASYNC
#define FIOASYNC 0x8004667d /* BSD _IOW('f', 125, int); QNX headers omit it */
#endif
#ifndef O_ASYNC
#define O_ASYNC 0x10000
#endif

static volatile sig_atomic_t g_hits;
static volatile int g_signo, g_code, g_value;
static volatile pthread_t g_handler_thread;
static char g_case[64];
static char **g_argv;

static void on_sigio(int sig, siginfo_t *si, void *ctx)
{
    (void)ctx;
    g_hits++;
    g_signo = si ? si->si_signo : sig;
    g_code = si ? si->si_code : -999;
    g_value = si ? si->si_value.sival_int : -999;
    g_handler_thread = pthread_self();
}

static void on_alarm(int sig)
{
    char buf[128];
    int n;
    (void)sig;
    n = snprintf(buf, sizeof buf, "%s.result=HANG (3s alarm)\n", g_case);
    (void)write(1, buf, n);
    _exit(90);
}

static void out(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void out(const char *key, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n = snprintf(buf, sizeof buf, "%s.%s=", g_case, key);
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - n, fmt, ap);
    va_end(ap);
    buf[n++] = '\n';
    (void)write(1, buf, n);
}

static void install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sigio;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGIO, &sa, NULL);
    signal(SIGALRM, on_alarm);
}

static void reset_hits(void)
{
    g_hits = 0;
    g_signo = g_code = g_value = 0;
}

static int wait_hit(int ms)
{
    struct timespec t0, ts;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!g_hits) {
        long el;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        el = (ts.tv_sec - t0.tv_sec) * 1000 + (ts.tv_nsec - t0.tv_nsec) / 1000000;
        if (el >= ms) break;
        ts.tv_sec = 0;
        ts.tv_nsec = 1000000;
        nanosleep(&ts, NULL);
    }
    return g_hits;
}

static int do_fioasync(int fd, int on)
{
    return ioctl(fd, FIOASYNC, &on);
}

static void out_rc(const char *key, int rc)
{
    if (rc < 0) out(key, "-1 errno=%d", errno);
    else out(key, "%d", rc);
}

static void report_hit(const char *key)
{
    if (wait_hit(1000))
        out(key, "yes si_signo=%d si_code=%d si_value=%d", g_signo, g_code, g_value);
    else
        out(key, "no");
}

struct pair { int w, t, aux; };

static int tcp_listener(int *port)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 4) < 0) return -1;
    getsockname(s, (struct sockaddr *)&a, &l);
    *port = ntohs(a.sin_port);
    return s;
}

static int connect_to(int port, int stype)
{
    struct sockaddr_in a;
    int s = socket(AF_INET, stype, 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) return -1;
    return s;
}

static int make_sp(struct pair *p, int type)
{
    int sv[2];
    if (socketpair(AF_UNIX, type, 0, sv) < 0) return -1;
    p->w = sv[0];
    p->t = sv[1];
    return 0;
}
static int make_unix_stream(struct pair *p) { return make_sp(p, SOCK_STREAM); }
static int make_unix_dgram(struct pair *p) { return make_sp(p, SOCK_DGRAM); }

static int make_tcp(struct pair *p)
{
    int port, l = tcp_listener(&port);
    if (l < 0) return -1;
    p->t = connect_to(port, SOCK_STREAM);
    p->w = accept(l, NULL, NULL);
    p->aux = l;
    return (p->t < 0 || p->w < 0) ? -1 : 0;
}
static int make_tcp_listen(struct pair *p)
{
    int port, l = tcp_listener(&port);
    if (l < 0) return -1;
    p->w = l;
    p->t = -1;
    p->aux = port;
    return 0;
}
static int make_udp(struct pair *p)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    int r = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(r, (struct sockaddr *)&a, sizeof a) < 0) return -1;
    getsockname(r, (struct sockaddr *)&a, &l);
    p->w = r;
    p->t = connect_to(ntohs(a.sin_port), SOCK_DGRAM);
    return p->t < 0 ? -1 : 0;
}
static int make_pty(struct pair *p)
{
    int m = open("/dev/ptyp0", O_RDWR | O_NOCTTY);
    if (m < 0) m = open("/dev/ptyp1", O_RDWR | O_NOCTTY);
    if (m < 0) { out("setup", "pty master open failed errno=%d", errno); return -1; }
    p->w = m;
    p->t = open("/dev/ttyp0", O_RDWR | O_NOCTTY);
    if (p->t < 0) p->t = open("/dev/ttyp1", O_RDWR | O_NOCTTY);
    if (p->t < 0) { out("setup", "pty slave open failed errno=%d", errno); return -1; }
    return 0;
}
static int make_pipe(struct pair *p)
{
    int fds[2];
    if (pipe(fds) < 0) return -1;
    p->w = fds[0];
    p->t = fds[1];
    return 0;
}

static void poke(struct pair *p, int listen_kind)
{
    if (listen_kind) {
        struct sockaddr_in a;
        int s = socket(AF_INET, SOCK_STREAM, 0);
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons(p->aux);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        connect(s, (struct sockaddr *)&a, sizeof a);
    } else {
        (void)write(p->t, "x", 1);
    }
}

static void kind_case(const char *name, int (*make)(struct pair *), int listen_kind)
{
    struct pair p = { -1, -1, -1 };
    int rc, fl, own;
    snprintf(g_case, sizeof g_case, "%s", name);
    alarm(3);
    if (make(&p) < 0) { out("setup", "failed errno=%d", errno); return; }
    rc = do_fioasync(p.w, 1);
    out_rc("fioasync", rc);
    rc = fcntl(p.w, F_SETOWN, getpid());
    out_rc("setown", rc);
    own = fcntl(p.w, F_GETOWN);
    if (own < 0) out("getown", "-1 errno=%d", errno);
    else out("getown", "%d is_pid=%d", own, own == (int)getpid());
    fl = fcntl(p.w, F_GETFL);
    out("getfl_async", "%d", fl < 0 ? -1 : (fl & O_ASYNC) != 0);
    reset_hits();
    poke(&p, listen_kind);
    report_hit("sigio");
    alarm(0);
}

static void *blocker(void *arg)
{
    char b[4];
    (void)read(*(int *)arg, b, sizeof b);
    return NULL;
}

static void unix_extra(void)
{
    struct pair p;
    int rc, own, fl;
    char b[8];

    /* F_SETFL O_ASYNC alone (no FIOASYNC) */
    snprintf(g_case, sizeof g_case, "unix-stream-osetfl");
    alarm(3);
    make_unix_stream(&p);
    fl = fcntl(p.w, F_GETFL);
    rc = fcntl(p.w, F_SETFL, fl | O_ASYNC);
    out_rc("setfl", rc);
    out("getfl_async", "%d", (fcntl(p.w, F_GETFL) & O_ASYNC) != 0);
    rc = fcntl(p.w, F_SETOWN, getpid());
    out_rc("setown", rc);
    reset_hits();
    (void)write(p.t, "x", 1);
    report_hit("sigio");
    /* turning it off again */
    rc = do_fioasync(p.w, 0);
    out_rc("fioasync_off", rc);
    out("getfl_async_after_off", "%d", (fcntl(p.w, F_GETFL) & O_ASYNC) != 0);
    (void)read(p.w, b, sizeof b);
    reset_hits();
    (void)write(p.t, "y", 1);
    report_hit("sigio_after_off");
    alarm(0);

    /* peer close */
    snprintf(g_case, sizeof g_case, "unix-stream-peerclose");
    alarm(3);
    make_unix_stream(&p);
    do_fioasync(p.w, 1);
    fcntl(p.w, F_SETOWN, getpid());
    reset_hits();
    close(p.t);
    report_hit("sigio");
    alarm(0);

    /* F_SETOWN(-getpgrp()) */
    snprintf(g_case, sizeof g_case, "unix-stream-pgrp");
    alarm(3);
    make_unix_stream(&p);
    do_fioasync(p.w, 1);
    rc = fcntl(p.w, F_SETOWN, -getpgrp());
    out_rc("setown_neg_pgrp", rc);
    own = fcntl(p.w, F_GETOWN);
    out("getown", "%d (pgrp=%d pid=%d)", own, (int)getpgrp(), (int)getpid());
    reset_hits();
    (void)write(p.t, "x", 1);
    report_hit("sigio");
    alarm(0);

    /* fork child */
    snprintf(g_case, sizeof g_case, "unix-stream-fork");
    alarm(3);
    make_unix_stream(&p);
    do_fioasync(p.w, 1);
    fcntl(p.w, F_SETOWN, getpid());
    {
        int pf[2];
        pid_t c;
        char buf[200];
        int n;
        pipe(pf);
        c = fork();
        if (c == 0) {
            int o = fcntl(p.w, F_GETOWN);
            int f = fcntl(p.w, F_GETFL);
            n = snprintf(buf, sizeof buf, "own=%d (parent=%d child=%d) async=%d",
                         o, (int)getppid(), (int)getpid(), f < 0 ? -1 : (f & O_ASYNC) != 0);
            (void)write(pf[1], buf, n);
            _exit(0);
        }
        close(pf[1]);
        n = read(pf[0], buf, sizeof buf - 1);
        buf[n > 0 ? n : 0] = 0;
        out("child_state", "%s", buf);
        waitpid(c, NULL, 0);
        /* the child's peer write: does the parent get the signal? */
        reset_hits();
        c = fork();
        if (c == 0) {
            (void)write(p.t, "c", 1);
            _exit(0);
        }
        waitpid(c, NULL, 0);
        report_hit("child_write_signals_parent");
    }
    alarm(0);

    /* which thread runs the handler while another is blocked in read */
    snprintf(g_case, sizeof g_case, "unix-stream-thread");
    alarm(3);
    make_unix_stream(&p);
    {
        pthread_t th;
        int bp[2];
        pipe(bp);
        pthread_create(&th, NULL, blocker, &bp[0]);
        usleep(100000);
        do_fioasync(p.w, 1);
        fcntl(p.w, F_SETOWN, getpid());
        reset_hits();
        (void)write(p.t, "x", 1);
        if (wait_hit(1000))
            out("handler_thread", "%s",
                pthread_equal(g_handler_thread, th) ? "blocked-thread" : "main-thread");
        else
            out("handler_thread", "no sigio");
        (void)write(bp[1], "x", 1);
        pthread_join(th, NULL);
    }
    alarm(0);
}

static void exec_case(void)
{
    struct pair p;
    char fdbuf[16];
    pid_t c;
    snprintf(g_case, sizeof g_case, "unix-stream-exec");
    alarm(3);
    make_unix_stream(&p);
    do_fioasync(p.w, 1);
    fcntl(p.w, F_SETOWN, getpid());
    snprintf(fdbuf, sizeof fdbuf, "%d", p.w);
    /* fork + exec: the child has a new pid; is the owner (the parent) kept? */
    c = fork();
    if (c == 0) {
        execl(g_argv[0], g_argv[0], "exec-check", fdbuf, "child", (char *)NULL);
        _exit(91);
    }
    waitpid(c, NULL, 0);
    /* exec in place: pid is kept, so the owner stays valid; the image
     * re-installs its handler, pokes the peer and looks for SIGIO */
    execl(g_argv[0], g_argv[0], "exec-check", fdbuf, "self", (char *)NULL);
    out("exec_self", "failed errno=%d", errno);
    alarm(0);
}

/* After exec: report owner and flag; for "self" also poke the peer (fd+1,
 * the next descriptor the socketpair allocated) and look for the signal. */
static int exec_check(int fd, int self)
{
    int o = fcntl(fd, F_GETOWN);
    int f = fcntl(fd, F_GETFL);
    snprintf(g_case, sizeof g_case, "unix-stream-exec-%s", self ? "inplace" : "forked");
    install();
    alarm(3);
    out("after_exec_state", "own=%d pid=%d ppid=%d async=%d", o, (int)getpid(),
        (int)getppid(), f < 0 ? -1 : (f & O_ASYNC) != 0);
    if (self) {
        reset_hits();
        (void)write(fd + 1, "x", 1);
        report_hit("sigio_after_exec");
    }
    return 0;
}

static void run(void (*fn)(void))
{
    pid_t c = fork();
    if (c == 0) {
        fn();
        _exit(0);
    }
    waitpid(c, NULL, 0);
}

#define KIND(fnname, label, mk, lk) \
    static void fnname(void) { kind_case(label, mk, lk); }
KIND(k_us, "unix-stream", make_unix_stream, 0)
KIND(k_ud, "unix-dgram", make_unix_dgram, 0)
KIND(k_tcp, "tcp-loopback", make_tcp, 0)
KIND(k_tl, "tcp-listen", make_tcp_listen, 1)
KIND(k_udp, "udp-loopback", make_udp, 0)
KIND(k_pty, "pty-master", make_pty, 0)
KIND(k_pipe, "pipe", make_pipe, 0)

int main(int argc, char **argv)
{
    g_argv = argv;
    install();
    if (argc >= 3 && strcmp(argv[1], "exec-check") == 0)
        return exec_check(atoi(argv[2]), argc >= 4 && strcmp(argv[3], "self") == 0);
    printf("pid=%d pgrp=%d FIOASYNC=0x%x O_ASYNC=0x%x\n", (int)getpid(),
           (int)getpgrp(), (unsigned)FIOASYNC, (unsigned)O_ASYNC);
    fflush(stdout);
    run(k_us);
    run(k_ud);
    run(k_tcp);
    run(k_tl);
    run(k_udp);
    run(k_pty);
    run(k_pipe);
    run(unix_extra);
    run(exec_case);
    return 0;
}
