/*
 * Device facts that decide the Phase 3 part 2 build (timerfd and signalfd).
 * Prints one name=value line per finding: sigwait_collect, sigwait_siginfo,
 * pulse_timer_accuracy, pulse_coalescing, monotonic_in_sleep,
 * realtime_change_detect. Lines starting with '#' are supporting detail.
 * Every wait is bounded; alarm() is a last-resort guard. No signal handler
 * here touches io-pkt (the handlers only set a flag, or write to stderr and _exit).
 */
#define _QNX_SOURCE 1
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>
#include <sys/procmgr.h>
#include <sys/siginfo.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void alarm_handler(int sig)
{
    static const char msg[] = "probe_guard=alarm fired\n";
    (void)sig;
    write(2, msg, sizeof(msg) - 1);
    _exit(99);
}

static volatile int handler_ran;
static void usr1_handler(int sig) { (void)sig; handler_ran++; }

/* ---- sigwait ---- */
struct got { int ok; siginfo_t si; int err; };
static struct got got[3];
static volatile int stop_idle;
static pthread_mutex_t gmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gcv = PTHREAD_COND_INITIALIZER;
static int ready, want, done_n;

static void *idle_thread(void *a)
{
    (void)a;
    while (!stop_idle) usleep(20000);
    return NULL;
}

static void *wait_thread(void *a)
{
    sigset_t set;
    struct timespec to = { 1, 0 };
    int i, r;
    (void)a;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_mutex_lock(&gmu);
    ready = 1;
    pthread_cond_broadcast(&gcv);
    pthread_mutex_unlock(&gmu);
    for (i = 0; i < 3; i++) {
        pthread_mutex_lock(&gmu);
        while (want <= i) pthread_cond_wait(&gcv, &gmu);
        pthread_mutex_unlock(&gmu);
        memset(&got[i].si, 0, sizeof(got[i].si));
        r = sigtimedwait(&set, &got[i].si, &to);
        got[i].ok = r == SIGUSR1;
        got[i].err = r < 0 ? errno : 0;
        pthread_mutex_lock(&gmu);
        done_n = i + 1;
        pthread_cond_broadcast(&gcv);
        pthread_mutex_unlock(&gmu);
    }
    return NULL;
}

static void next_case(int i)
{
    pthread_mutex_lock(&gmu);
    want = i + 1;
    pthread_cond_broadcast(&gcv);
    pthread_mutex_unlock(&gmu);
}

static void wait_case(int i)
{
    pthread_mutex_lock(&gmu);
    while (done_n <= i) pthread_cond_wait(&gcv, &gmu);
    pthread_mutex_unlock(&gmu);
}

static void probe_sigwait(void)
{
    pthread_t a, b, c;
    sigset_t set;
    struct sigaction sa;
    union sigval v;
    pid_t child;
    int i;
    char buf[512];
    int off = 0;
    static const char *nm[3] = { "kill", "sigqueue", "child_kill" };

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = usr1_handler;
    sigaction(SIGUSR1, &sa, NULL);
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);   /* inherited by A, B, C */
    pthread_create(&a, NULL, idle_thread, NULL);
    pthread_create(&b, NULL, idle_thread, NULL);
    pthread_create(&c, NULL, wait_thread, NULL);
    pthread_mutex_lock(&gmu);
    while (!ready) pthread_cond_wait(&gcv, &gmu);
    pthread_mutex_unlock(&gmu);

    next_case(0);
    usleep(50000);
    kill(getpid(), SIGUSR1);
    wait_case(0);

    next_case(1);
    usleep(50000);
    v.sival_int = 42;
    if (sigqueue(getpid(), SIGUSR1, v) < 0)
        printf("# sigqueue errno=%d\n", errno);
    wait_case(1);

    next_case(2);
    usleep(50000);
    child = fork();
    if (child == 0) {
        kill(getppid(), SIGUSR1);
        _exit(0);
    }
    wait_case(2);
    waitpid(child, NULL, 0);

    stop_idle = 1;
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    pthread_join(c, NULL);

    printf("sigwait_collect=%s\n",
           got[0].ok && got[1].ok && got[2].ok && handler_ran == 0 ? "ok" : "fail");
    printf("# sigwait handler_ran=%d err=%d,%d,%d\n", handler_ran,
           got[0].err, got[1].err, got[2].err);
    for (i = 0; i < 3; i++)
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%s%s(pid=%d,uid=%d,code=%d,value=%d)", i ? ";" : "",
                        nm[i], (int)got[i].si.si_pid, (int)got[i].si.si_uid,
                        got[i].si.si_code, got[i].si.si_value.sival_int);
    printf("sigwait_siginfo=%s\n", buf);
    printf("# expected: self pid=%d uid=%d, child pid=%d; SI_USER=%d SI_QUEUE=%d\n",
           (int)getpid(), (int)getuid(), (int)child, SI_USER, SI_QUEUE);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
}

/* ---- pulse timers ---- */
#define PULSE_CODE (_PULSE_CODE_MINAVAIL + 1)

static long long ns_of(const struct timespec *t)
{
    return (long long)t->tv_sec * 1000000000LL + t->tv_nsec;
}

static long long now_ns(clockid_t clk)
{
    struct timespec t;
    clock_gettime(clk, &t);
    return ns_of(&t);
}

static int cmp_ll(const void *x, const void *y)
{
    long long a = *(const long long *)x, b = *(const long long *)y;
    return a < b ? -1 : a > b;
}

/* Receive one pulse with a timeout; 1 = pulse, 0 = timeout. */
static int recv_pulse(int chid, long long timeout_ns)
{
    struct _pulse p;
    uint64_t to = (uint64_t)timeout_ns;
    int r;
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &to, NULL);
    r = MsgReceivePulse(chid, &p, sizeof(p), NULL);
    return r == 0;
}

static int make_timer(clockid_t clk, int coid, timer_t *t)
{
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, SIGEV_PULSE_PRIO_INHERIT, PULSE_CODE, 7);
    return timer_create(clk, &ev, t);
}

static void accuracy(clockid_t clk, int chid, int coid, long long *med, long long *p99)
{
    enum { N = 200 };
    long long late[N];
    timer_t t;
    struct itimerspec its;
    long long period = 10000000LL, t0;
    int i, got_n = 0;

    memset(late, 0, sizeof(late));
    *med = *p99 = -1;
    if (make_timer(clk, coid, &t) < 0) { printf("# timer_create clk=%d errno=%d\n", (int)clk, errno); return; }
    memset(&its, 0, sizeof(its));
    its.it_interval.tv_nsec = 10000000;
    t0 = now_ns(clk);
    if (clk == CLOCK_REALTIME) {
        long long first = t0 + period;
        its.it_value.tv_sec = first / 1000000000LL;
        its.it_value.tv_nsec = first % 1000000000LL;
        timer_settime(t, TIMER_ABSTIME, &its, NULL);
    } else {
        its.it_value.tv_nsec = 10000000;
        timer_settime(t, 0, &its, NULL);
    }
    for (i = 0; i < N; i++) {
        long long ideal;
        if (!recv_pulse(chid, 500000000LL)) break;
        ideal = t0 + (long long)(i + 1) * period;
        late[got_n++] = now_ns(clk) - ideal;
    }
    timer_delete(t);
    while (recv_pulse(chid, 1000000LL)) ;
    if (got_n < N) printf("# accuracy clk=%d only %d/%d pulses\n", (int)clk, got_n, N);
    if (got_n) {
        int k = (got_n * 99) / 100;
        qsort(late, got_n, sizeof(late[0]), cmp_ll);
        *med = late[got_n / 2] / 1000;
        *p99 = late[k >= got_n ? got_n - 1 : k] / 1000;
    }
}

/* Periodic pulse timer, receiver asleep for sleep_us, then count the queued
 * pulses. Returns the count; -1 if the timer could not be created. */
static int coalesce(int chid, int coid, long period_ns, useconds_t sleep_us, int *ovr)
{
    timer_t t;
    struct itimerspec its;
    int n = 0;

    *ovr = -1;
    while (recv_pulse(chid, 1000000LL)) ;
    if (make_timer(CLOCK_MONOTONIC, coid, &t) < 0) return -1;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = period_ns;
    its.it_interval.tv_nsec = period_ns;
    timer_settime(t, 0, &its, NULL);
    usleep(sleep_us);
    *ovr = timer_getoverrun(t);
    timer_delete(t);
    while (recv_pulse(chid, 2000000LL)) n++;
    return n;
}

static void probe_pulse(void)
{
    int chid = ChannelCreate(0);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    long long m1, p1, m2, p2;

    if (chid < 0 || coid < 0) {
        printf("pulse_timer_accuracy=unknown (channel errno=%d)\n", errno);
        printf("pulse_coalescing=unknown\n");
        return;
    }
    accuracy(CLOCK_MONOTONIC, chid, coid, &m1, &p1);
    accuracy(CLOCK_REALTIME, chid, coid, &m2, &p2);
    printf("pulse_timer_accuracy=%lld,%lld (monotonic relative median_us,p99_us; realtime absolute %lld,%lld; lateness vs ideal, 200 x 10 ms)\n",
           m1, p1, m2, p2);

    /* coalescing: three runs separate a tick limit from a queue cap */
    {
        int ovr_c, ovr_a, ovr_b;
        int nc = coalesce(chid, coid, 100000, 50000, &ovr_c);   /* 100 us, 50 ms */
        int na = coalesce(chid, coid, 100000, 100000, &ovr_a);  /* 100 us, 100 ms */
        int nb = coalesce(chid, coid, 5000000, 50000, &ovr_b);  /* 5 ms, 50 ms */
        const char *queue = nb >= 8 ? "queued" : nb <= 2 ? "merged" : "partial";
        const char *limit = na >= nc * 3 / 2 ? "tick_limited" : "queue_capped";
        printf("pulse_coalescing=%s,%s (100us/50ms=%d of 500; 100us/100ms=%d of 1000; 5ms/50ms=%d of 10; overrun=%d,%d,%d)\n",
               queue, limit, nc, na, nb, ovr_c, ovr_a, ovr_b);
    }
}

/* ---- clocks ---- */
static void probe_clocks(void)
{
    struct timespec rt, mo;
    struct timeval tv;
    int chid = ChannelCreate(0);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    struct sigevent ev;
    int st_err = 0, tod_id, reg_err = 0, tv_err = 0, trig, trig_err = 0, pulse;
    long long off1, off2;

    /* monotonic_in_sleep needs a screen-off device sleep; not unattended. */
    printf("monotonic_in_sleep=unknown\n");
    printf("# monotonic_in_sleep cannot force screen-off sleep unattended\n");

    clock_gettime(CLOCK_REALTIME, &rt);
    clock_gettime(CLOCK_MONOTONIC, &mo);
    off1 = ns_of(&rt) - ns_of(&mo);
    if (clock_settime(CLOCK_REALTIME, &rt) < 0) st_err = errno;
    gettimeofday(&tv, NULL);
    if (settimeofday(&tv, NULL) < 0) tv_err = errno;
    clock_gettime(CLOCK_REALTIME, &rt);
    clock_gettime(CLOCK_MONOTONIC, &mo);
    off2 = ns_of(&rt) - ns_of(&mo);
    printf("# clock_settime(REALTIME)=%s errno=%d; settimeofday errno=%d; uid=%d; offset drift across calls ns=%lld\n",
           st_err ? "fail" : "ok", st_err, tv_err, (int)getuid(), off2 - off1);

    SIGEV_PULSE_INIT(&ev, coid, SIGEV_PULSE_PRIO_INHERIT, PULSE_CODE, 9);
    tod_id = procmgr_event_notify_add(PROCMGR_EVENT_TOD, &ev);
    if (tod_id < 0) reg_err = errno;
    printf("# procmgr_event_notify_add(PROCMGR_EVENT_TOD) = %d errno=%d\n", tod_id, reg_err);
    pulse = recv_pulse(chid, 100000000LL);
    printf("# spurious TOD pulse after registration: %d\n", pulse);
    if (st_err == 0) {
        pulse = recv_pulse(chid, 500000000LL);
        printf("# TOD pulse after real clock_settime: %d\n", pulse);
    }
    trig = procmgr_event_trigger(PROCMGR_EVENT_TOD);
    if (trig < 0) trig_err = errno;
    pulse = recv_pulse(chid, 500000000LL);
    printf("# procmgr_event_trigger(TOD)=%d errno=%d, pulse delivered=%d\n", trig, trig_err, pulse);

    if (tod_id >= 0 && st_err == 0)
        printf("realtime_change_detect=procmgr_event_tod\n");
    else if (tod_id >= 0 && trig == 0 && pulse)
        printf("realtime_change_detect=procmgr_event_tod_unverified (registration ok, only a manually triggered pulse delivered; no real clock change possible, clock_settime errno=%d)\n", st_err);
    else
        printf("realtime_change_detect=none (clock_settime errno=%d, procmgr_event_notify_add errno=%d)\n", st_err, reg_err);
    if (tod_id >= 0) procmgr_event_notify_delete(tod_id);
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = alarm_handler;
    sigaction(SIGALRM, &sa, NULL);
    alarm(60);
    setvbuf(stdout, NULL, _IOLBF, 0);
    probe_sigwait();
    probe_pulse();
    probe_clocks();
    return 0;
}
