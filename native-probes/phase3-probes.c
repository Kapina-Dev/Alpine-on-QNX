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

static void fifo_fail(const char *label, int step, const char *path)
{
    int e = errno;
    if (path) unlink(path);
    printf("%s=fail:%d (step %d)\n", label, e, step);
}

/* Steps 2-9 of the brief against a FIFO created at path. */
static void probe_fifo_at(const char *label, const char *path)
{
    struct pollfd p;
    char c = 'x', r = 0;
    int fd, n;
    pid_t pid;

    if (mkfifo(path, 0600) < 0) { fifo_fail(label, 2, NULL); return; }
    fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) { fifo_fail(label, 3, path); return; }
    unlink(path);
    if (write(fd, &c, 1) != 1) { fifo_fail(label, 4, NULL); return; }
    p.fd = fd; p.events = POLLIN; p.revents = 0;
    n = poll(&p, 1, 1000);
    if (n != 1 || !(p.revents & POLLIN)) {
        printf("%s=fail:%d (step 5 n=%d revents=0x%x)\n", label, EIO, n, p.revents);
        return;
    }
    if (read(fd, &r, 1) != 1 || r != 'x') { fifo_fail(label, 6, NULL); return; }
    p.revents = 0;
    n = poll(&p, 1, 0);
    if (n != 0) {
        printf("%s=fail:%d (step 7 n=%d revents=0x%x)\n", label, EIO, n, p.revents);
        return;
    }
    pid = fork();
    if (pid < 0) { fifo_fail(label, 8, NULL); return; }
    if (pid == 0) {
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
        write(fd, &c, 1);
        _exit(0);
    }
    p.revents = 0;
    n = poll(&p, 1, 2000);
    waitpid(pid, NULL, 0);
    if (n != 1 || !(p.revents & POLLIN)) {
        printf("%s=fail:%d (step 9 n=%d revents=0x%x)\n", label, EIO, n, p.revents);
        return;
    }
    printf("%s=ok\n", label);
}

static void probe_fifo(void)
{
    char dir[64], path[96];

    /* /tmp is a symlink to /dev/shmem here, which may refuse directories. */
    snprintf(dir, sizeof(dir), "/tmp/p3fifo%d", (int)getpid());
    if (mkdir(dir, 0700) < 0) {
        printf("fifo_note=mkdir %s failed errno=%d\n", dir, errno);
    } else {
        snprintf(path, sizeof(path), "%s/f", dir);
        probe_fifo_at("fifo_rdwr.tmpdir", path);
        rmdir(dir);
    }
    snprintf(path, sizeof(path), "/tmp/p3fifo%d", (int)getpid());
    probe_fifo_at("fifo_rdwr", path);
    snprintf(path, sizeof(path), "/accounts/1000/shared/misc/p3fifo%d", (int)getpid());
    probe_fifo_at("fifo_rdwr.disk", path);
}

struct shared {
    pthread_mutex_t m;
};

static void probe_robust(void)
{
    pthread_mutexattr_t a;
    int (*setrobust)(pthread_mutexattr_t *, int);
    int (*consistent)(pthread_mutex_t *);
    struct shared *s;
    char name[64];
    int fd, rc, st;
    pid_t pid;
    struct timespec ts;

    snprintf(name, sizeof(name), "/p3robust%d", (int)getpid());
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { printf("robust_mutex=fail:%d (shm_open)\n", errno); return; }
    shm_unlink(name);
    if (ftruncate(fd, 4096) < 0) { printf("robust_mutex=fail:%d (ftruncate)\n", errno); return; }
    s = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (s == MAP_FAILED) { printf("robust_mutex=fail:%d (mmap)\n", errno); return; }
    pthread_mutexattr_init(&a);
    rc = pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    if (rc) { printf("robust_mutex=fail:%d (setpshared)\n", rc); return; }
    /* The SDK headers declare no robust API, so look the symbols up. */
    setrobust = (int (*)(pthread_mutexattr_t *, int))
        dlsym(RTLD_DEFAULT, "pthread_mutexattr_setrobust");
    if (!setrobust)
        setrobust = (int (*)(pthread_mutexattr_t *, int))
            dlsym(RTLD_DEFAULT, "pthread_mutexattr_setrobust_np");
    consistent = (int (*)(pthread_mutex_t *))
        dlsym(RTLD_DEFAULT, "pthread_mutex_consistent");
    if (!consistent)
        consistent = (int (*)(pthread_mutex_t *))
            dlsym(RTLD_DEFAULT, "pthread_mutex_consistent_np");
    if (!setrobust || !consistent) {
        printf("robust_mutex=absent (setrobust %s, consistent %s in libc)\n",
               setrobust ? "found" : "missing", consistent ? "found" : "missing");
        return;
    }
    rc = setrobust(&a, 1 /* PTHREAD_MUTEX_ROBUST */);
    if (rc == ENOSYS || rc == EINVAL) {
        printf("robust_mutex=absent (setrobust returned %d)\n", rc);
        return;
    }
    if (rc) { printf("robust_mutex=fail:%d (setrobust)\n", rc); return; }
    rc = pthread_mutex_init(&s->m, &a);
    if (rc == ENOSYS || rc == EINVAL) {
        printf("robust_mutex=absent (mutex_init returned %d)\n", rc);
        return;
    }
    if (rc) { printf("robust_mutex=fail:%d (mutex_init)\n", rc); return; }
    pid = fork();
    if (pid < 0) { printf("robust_mutex=fail:%d (fork)\n", errno); return; }
    if (pid == 0) {
        pthread_mutex_lock(&s->m);
        _exit(0);
    }
    waitpid(pid, &st, 0);
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 3;
    rc = pthread_mutex_timedlock(&s->m, &ts);
    if (rc != EOWNERDEAD) {
        printf("robust_mutex=fail:%d (lock after owner death returned %d, expected EOWNERDEAD=%d%s)\n",
               rc, rc, EOWNERDEAD, rc == ETIMEDOUT ? "; timed out, not robust" : "");
        return;
    }
    rc = consistent(&s->m);
    if (rc) { printf("robust_mutex=fail:%d (consistent)\n", rc); return; }
    rc = pthread_mutex_unlock(&s->m);
    if (rc) { printf("robust_mutex=fail:%d (unlock)\n", rc); return; }
    printf("robust_mutex=ok\n");
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
