/*
 * Device facts that decide the Phase 3 part 1 build (eventfd and epoll).
 * Prints one name=value line per finding: fifo_rdwr, robust_mutex,
 * poll_close_race, rdhup_signal. Every wait is bounded; alarm() is a
 * last-resort guard.
 */
#define _QNX_SOURCE 1
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>

static void alarm_handler(int sig)
{
    static const char msg[] = "probe_guard=alarm fired\n";
    (void)sig;
    write(2, msg, sizeof(msg) - 1);
    _exit(99);
}

/* Result of one location: 0 ok, else errno; step is the failing step. */
static int fifo_step;

static int probe_fifo_at(const char *path)
{
    struct pollfd p;
    char c = 'x', r = 0;
    int fd, n, e;
    pid_t pid;

#define FAIL(st, err, unl) do { e = (err); fifo_step = (st); if (unl) unlink(path); return e; } while (0)
    if (mkfifo(path, 0600) < 0) FAIL(2, errno, 0);
    fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) FAIL(3, errno, 1);
    unlink(path);
    if (write(fd, &c, 1) != 1) FAIL(4, errno ? errno : EIO, 0);
    p.fd = fd; p.events = POLLIN; p.revents = 0;
    n = poll(&p, 1, 1000);
    if (n != 1 || !(p.revents & POLLIN)) FAIL(5, EIO, 0);
    if (read(fd, &r, 1) != 1 || r != 'x') FAIL(6, errno ? errno : EIO, 0);
    p.revents = 0;
    n = poll(&p, 1, 0);
    if (n != 0) FAIL(7, EIO, 0);
    pid = fork();
    if (pid < 0) FAIL(8, errno, 0);
    if (pid == 0) {
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
        write(fd, &c, 1);
        _exit(0);
    }
    p.revents = 0;
    n = poll(&p, 1, 2000);
    waitpid(pid, NULL, 0);
    if (n != 1 || !(p.revents & POLLIN)) FAIL(9, EIO, 0);
#undef FAIL
    close(fd);
    return 0;
}

static void probe_fifo(void)
{
    char dir[64], path[96];
    int tmp, disk, tmp_step, first_step;

    /* /tmp is a symlink to /dev/shmem here, which may refuse directories. */
    snprintf(dir, sizeof(dir), "/tmp/p3fifo%d", (int)getpid());
    if (mkdir(dir, 0700) < 0) {
        printf("fifo_note=mkdir %s failed errno=%d\n", dir, errno);
    } else {
        rmdir(dir);
        printf("fifo_note=mkdir under /tmp ok\n");
    }
    snprintf(path, sizeof(path), "/tmp/p3fifo%d", (int)getpid());
    tmp = probe_fifo_at(path);
    tmp_step = fifo_step;
    if (tmp) printf("fifo_rdwr.tmp=fail:%d (step %d)\n", tmp, tmp_step);
    else printf("fifo_rdwr.tmp=ok\n");
    snprintf(path, sizeof(path), "/accounts/1000/shared/misc/p3fifo%d", (int)getpid());
    disk = probe_fifo_at(path);
    if (disk) printf("fifo_rdwr.disk=fail:%d (step %d)\n", disk, fifo_step);
    else printf("fifo_rdwr.disk=ok\n");
    first_step = tmp_step;
    if (!tmp) {
        printf("fifo_rdwr=ok\n");
    } else if (!disk) {
        printf("fifo_rdwr=ok-disk-only\n");
        printf("fifo_rdwr.step=%d (the /tmp failure)\n", first_step);
    } else {
        printf("fifo_rdwr=fail:%d\n", tmp);
        printf("fifo_rdwr.step=%d\n", first_step);
    }
}

struct shared {
    pthread_mutex_t m;
};

static int (*robust_setrobust)(pthread_mutexattr_t *, int);
static int (*robust_consistent)(pthread_mutex_t *);

/* Fork a child that locks the mutex in shm and _exits; lock it here with a
 * 1 s timeout and return the result. Call the consistent+unlock sequence
 * when the lock returns EOWNERDEAD and robust_consistent is set. */
static int dead_owner(int robust, int *tail)
{
    pthread_mutexattr_t a;
    struct shared *s;
    char name[64];
    int fd, rc, st;
    pid_t pid;
    struct timespec ts;

    *tail = 0;
    snprintf(name, sizeof(name), "/p3robust%d", (int)getpid());
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1000 - errno;
    shm_unlink(name);
    if (ftruncate(fd, 4096) < 0) return -1000 - errno;
    s = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (s == MAP_FAILED) return -1000 - errno;
    pthread_mutexattr_init(&a);
    rc = pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    if (rc) return -2000 - rc;
    if (robust) {
        rc = robust_setrobust(&a, 1 /* PTHREAD_MUTEX_ROBUST */);
        if (rc) return -3000 - rc;
    }
    rc = pthread_mutex_init(&s->m, &a);
    if (rc) return -4000 - rc;
    pid = fork();
    if (pid < 0) return -1000 - errno;
    if (pid == 0) {
        pthread_mutex_lock(&s->m);
        _exit(0);
    }
    waitpid(pid, &st, 0);
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;
    rc = pthread_mutex_timedlock(&s->m, &ts);
    if (rc == EOWNERDEAD && robust && robust_consistent) {
        int r1 = robust_consistent(&s->m);
        int r2 = pthread_mutex_unlock(&s->m);
        *tail = r1 ? r1 : r2;
        if (r1) *tail = r1;
    }
    return rc;
}

static void print_plain(int rc)
{
    if (rc == 0) printf("dead_owner_plain=ok\n");
    else if (rc == ETIMEDOUT) printf("dead_owner_plain=ETIMEDOUT\n");
    else if (rc == EOWNERDEAD) printf("dead_owner_plain=EOWNERDEAD\n");
    else printf("dead_owner_plain=other:%d\n", rc < 0 ? -rc : rc);
}

static void probe_robust(void)
{
    void *ctl;
    int rc, tail;
    const char *how;

    /* Positive control: a symbol that certainly exists must resolve. */
    ctl = dlsym(RTLD_DEFAULT, "pthread_mutexattr_setpshared");
    printf("dlsym_control=pthread_mutexattr_setpshared %s\n", ctl ? "found" : "missing");

    /* (c) what a dead owner does to a plain shared mutex. */
    print_plain(dead_owner(0, &tail));

#ifdef PTHREAD_MUTEX_ROBUST
    printf("robust_header=PTHREAD_MUTEX_ROBUST defined\n");
    robust_setrobust = pthread_mutexattr_setrobust;
    robust_consistent = pthread_mutex_consistent;
    how = "header";
#else
    printf("robust_header=PTHREAD_MUTEX_ROBUST not defined\n");
    how = "dlsym";
    if (ctl) {
        robust_setrobust = (int (*)(pthread_mutexattr_t *, int))
            dlsym(RTLD_DEFAULT, "pthread_mutexattr_setrobust");
        if (!robust_setrobust)
            robust_setrobust = (int (*)(pthread_mutexattr_t *, int))
                dlsym(RTLD_DEFAULT, "pthread_mutexattr_setrobust_np");
        robust_consistent = (int (*)(pthread_mutex_t *))
            dlsym(RTLD_DEFAULT, "pthread_mutex_consistent");
        if (!robust_consistent)
            robust_consistent = (int (*)(pthread_mutex_t *))
                dlsym(RTLD_DEFAULT, "pthread_mutex_consistent_np");
        printf("dlsym_setrobust=%s dlsym_consistent=%s\n",
               robust_setrobust ? "found" : "missing",
               robust_consistent ? "found" : "missing");
    }
#endif
    if (!robust_setrobust || !robust_consistent) {
        if (!ctl) printf("robust_mutex=unknown:dlsym (control lookup failed)\n");
        else printf("robust_mutex=absent (via %s: setrobust %s, consistent %s)\n", how,
                    robust_setrobust ? "found" : "missing",
                    robust_consistent ? "found" : "missing");
        return;
    }
    rc = dead_owner(1, &tail);
    if (rc <= -3000 && rc > -4000 && (-rc - 3000 == ENOSYS || -rc - 3000 == EINVAL)) {
        printf("robust_mutex=absent (setrobust returned %d)\n", -rc - 3000);
    } else if (rc == EOWNERDEAD && tail == 0) {
        printf("robust_mutex=ok\n");
    } else if (rc == EOWNERDEAD) {
        printf("robust_mutex=fail:%d (consistent/unlock)\n", tail);
    } else {
        printf("robust_mutex=fail:%d (lock after owner death returned %d, expected EOWNERDEAD=%d)\n",
               rc < 0 ? -rc % 1000 : rc, rc, EOWNERDEAD);
    }
}

static int race_fd;

static void *race_closer(void *arg)
{
    int fds[2], i;
    struct timespec ts = {0, 100000000};
    (void)arg;
    nanosleep(&ts, NULL);
    close(race_fd);
    /* Open pipes until one reuses the number (lowest free fd first). */
    for (i = 0; i < 16; i++) {
        if (pipe(fds) < 0) break;
        if (fds[0] == race_fd) {
            write(fds[1], "n", 1);
            printf("race_note=new pipe reused fd %d and was written\n", race_fd);
            break;
        }
        close(fds[1]);
    }
    if (i == 16) printf("race_note=fd number not reused\n");
    return NULL;
}

static void probe_race(void)
{
    int fds[2];
    pthread_t t;
    struct pollfd p;
    struct timespec a, b;
    long ms;
    int n;

    if (pipe(fds) < 0) { printf("poll_close_race=fail:%d (pipe)\n", errno); return; }
    race_fd = fds[0];
    pthread_create(&t, NULL, race_closer, NULL);
    p.fd = race_fd; p.events = POLLIN; p.revents = 0;
    clock_gettime(CLOCK_MONOTONIC, &a);
    n = poll(&p, 1, 2000);
    clock_gettime(CLOCK_MONOTONIC, &b);
    ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    pthread_join(t, NULL);
    printf("poll_close_race=n=%d revents=0x%x (POLLIN=0x%x POLLNVAL=0x%x) after %ldms%s\n",
           n, p.revents, POLLIN, POLLNVAL, ms,
           n == 0 ? " (blocked until timeout)" : "");
}

static void report(const char *label, int b)
{
    struct pollfd p;
    char c;
    int n, r;

    p.fd = b; p.events = POLLIN | POLLHUP | POLLRDNORM; p.revents = 0;
    n = poll(&p, 1, 1000);
    r = recv(b, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    printf("rdhup_signal.%s=n=%d revents=0x%x (POLLIN=%s POLLHUP=%s POLLRDNORM=%s) peek=%d errno=%d\n",
           label, n, p.revents,
           (p.revents & POLLIN) ? "y" : "n",
           (p.revents & POLLHUP) ? "y" : "n",
           (p.revents & POLLRDNORM) ? "y" : "n", r, r < 0 ? errno : 0);
}

static void probe_rdhup(void)
{
    int ls, a, b, sv[2];
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);

    ls = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(ls, 1) < 0 || getsockname(ls, (struct sockaddr *)&sa, &sl) < 0) {
        printf("rdhup_signal.tcp=fail:%d (setup)\n", errno);
    } else {
        a = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(a, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            printf("rdhup_signal.tcp=fail:%d (connect)\n", errno);
        } else {
            struct pollfd lp = { ls, POLLIN, 0 };
            poll(&lp, 1, 1000);
            b = accept(ls, NULL, NULL);
            if (b < 0) {
                printf("rdhup_signal.tcp=fail:%d (accept)\n", errno);
            } else {
                shutdown(a, SHUT_WR);
                report("tcp", b);
            }
        }
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        printf("rdhup_signal.unix=fail:%d (socketpair)\n", errno);
    } else {
        shutdown(sv[0], SHUT_WR);
        report("unix", sv[1]);
    }
}

int main(void)
{
    signal(SIGALRM, alarm_handler);
    alarm(60);
    setvbuf(stdout, NULL, _IOLBF, 0);
    probe_fifo();
    probe_robust();
    probe_race();
    probe_rdhup();
    return 0;
}
