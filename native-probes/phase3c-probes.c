/*
 * Device facts that decide the Phase 3 part 3 build (inotify).
 * Prints one name=value line per finding; lines starting with '#' are
 * supporting detail. Every wait is bounded; alarm() is a last-resort guard.
 *
 * Usage: phase3c-probes [qnx6-dir] [shmem-dir] [more|special]
 *   defaults: ./inotify-probe.d and /dev/shmem/inotify-probe.d
 *
 * Findings:
 *   native_inotify_symbols   libc exports inotify_init/add_watch/rm_watch?
 *   fsnotify_open, fsevents_open   open() of the device nodes as this user
 *   native_inotify_init      inotify_init() result
 *   events_<fs>              per operation, the events a dir watch reports
 *   rename_cookie_<fs>       MOVED_FROM/MOVED_TO share a nonzero cookie?
 *   subdir_<fs>              does a dir watch report a grandchild change?
 *   foreign_<fs>             events caused by another process seen?
 *   self_delete_<fs>         DELETE_SELF / IGNORED on rmdir of the watched dir
 *   file_watch_<fs>          events from a watch on a regular file
 *   oneshot, overflow, poll_ready, nonblock_read, small_read
 *   ionotify_dir             ionotify() on a directory fd
 *   dir_mtime_resolution     st_mtime granularity on the qnx6 dir
 *   scan_cost_us             readdir+lstat cost for a 1000-entry directory
 *   special_fifo, special_socket   (third argument "special") add_watch on
 *                            a FIFO and on a bound unix socket's path: does
 *                            it return promptly (3 s alarm), and do events
 *                            arrive (FIFO write: MODIFY; chmod: ATTRIB)?
 */
#define _QNX_SOURCE 1
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct ev {
    int32_t wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
    char name[];
};

#define IN_ACCESS        0x00000001
#define IN_MODIFY        0x00000002
#define IN_ATTRIB        0x00000004
#define IN_CLOSE_WRITE   0x00000008
#define IN_CLOSE_NOWRITE 0x00000010
#define IN_OPEN          0x00000020
#define IN_MOVED_FROM    0x00000040
#define IN_MOVED_TO      0x00000080
#define IN_CREATE        0x00000100
#define IN_DELETE        0x00000200
#define IN_DELETE_SELF   0x00000400
#define IN_MOVE_SELF     0x00000800
#define IN_UNMOUNT       0x00002000
#define IN_Q_OVERFLOW    0x00004000
#define IN_IGNORED       0x00008000
#define IN_ISDIR         0x40000000
#define IN_ONESHOT       0x80000000
#define IN_ALL_EVENTS    0x00000fff

static int (*p_init)(void);
static int (*p_add)(int, const char *, uint32_t);
static int (*p_rm)(int, int32_t);

static void alarm_handler(int sig)
{
    static const char msg[] = "probe_guard=alarm fired\n";
    (void)sig;
    write(2, msg, sizeof(msg) - 1);
    _exit(99);
}

static long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static const char *mask_str(uint32_t m, char *buf, size_t size)
{
    static const struct { uint32_t bit; const char *name; } names[] = {
        { IN_ACCESS, "ACCESS" }, { IN_MODIFY, "MODIFY" },
        { IN_ATTRIB, "ATTRIB" }, { IN_CLOSE_WRITE, "CLOSE_WRITE" },
        { IN_CLOSE_NOWRITE, "CLOSE_NOWRITE" }, { IN_OPEN, "OPEN" },
        { IN_MOVED_FROM, "MOVED_FROM" }, { IN_MOVED_TO, "MOVED_TO" },
        { IN_CREATE, "CREATE" }, { IN_DELETE, "DELETE" },
        { IN_DELETE_SELF, "DELETE_SELF" }, { IN_MOVE_SELF, "MOVE_SELF" },
        { IN_UNMOUNT, "UNMOUNT" }, { IN_Q_OVERFLOW, "Q_OVERFLOW" },
        { IN_IGNORED, "IGNORED" }, { IN_ISDIR, "ISDIR" },
    };
    size_t i, used = 0;
    buf[0] = '\0';
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (m & names[i].bit) {
            used += snprintf(buf + used, size - used, "%s%s",
                             used ? "|" : "", names[i].name);
            m &= ~names[i].bit;
            if (used >= size)
                return buf;
        }
    }
    if (m && used < size)
        snprintf(buf + used, size - used, "%s0x%x", used ? "|" : "", m);
    return buf;
}

/* Collect events for up to wait_ms after the last one. Returns the text. */
static char text[8192];
static uint32_t seen_cookie_from, seen_cookie_to;
static int seen_count;

static const char *drain(int fd, int wait_ms)
{
    char buf[16384] __attribute__((aligned(8)));
    struct pollfd p;
    size_t used = 0;
    int r;
    text[0] = '\0';
    seen_count = 0;
    for (;;) {
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        r = poll(&p, 1, wait_ms);
        if (r <= 0)
            break;
        r = read(fd, buf, sizeof(buf));
        if (r < 0) {
            used += snprintf(text + used, sizeof(text) - used,
                             "%sread_err=%d", used ? " " : "", errno);
            break;
        }
        if (r == 0) {
            used += snprintf(text + used, sizeof(text) - used,
                             "%sread_eof", used ? " " : "");
            break;
        }
        {
            int off = 0;
            while (off + (int)sizeof(struct ev) <= r) {
                struct ev *e = (struct ev *)(buf + off);
                char m[160];
                if (e->mask & IN_MOVED_FROM)
                    seen_cookie_from = e->cookie;
                if (e->mask & IN_MOVED_TO)
                    seen_cookie_to = e->cookie;
                seen_count++;
                if (used < sizeof(text) - 200)
                    used += snprintf(text + used, sizeof(text) - used,
                                     "%s[wd%d %s c%u l%u '%.*s']",
                                     used ? " " : "", e->wd,
                                     mask_str(e->mask, m, sizeof(m)),
                                     e->cookie, e->len,
                                     (int)(e->len ? strnlen(e->name, e->len) : 0),
                                     e->name);
                off += (int)sizeof(struct ev) + (int)e->len;
            }
            if (off != r && used < sizeof(text) - 64)
                used += snprintf(text + used, sizeof(text) - used,
                                 " trailing=%d", r - off);
        }
    }
    if (!used)
        snprintf(text, sizeof(text), "none");
    return text;
}

static void touch_file(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
        close(fd);
}

static void rm_tree(const char *dir)
{
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    (void)system(cmd);
}

static void probe_fs(const char *tag, const char *dir)
{
    char a[1100], b[1100], sub[1100], deep[1100];
    int fd, wd, fwd, f;
    pid_t child;

    rm_tree(dir);
    if (mkdir(dir, 0755) != 0) {
        printf("events_%s=skip mkdir %s errno=%d\n", tag, dir, errno);
        return;
    }
    fd = p_init();
    if (fd < 0) {
        printf("events_%s=init_fail errno=%d\n", tag, errno);
        return;
    }
    wd = p_add(fd, dir, IN_ALL_EVENTS);
    printf("# %s add_watch(%s)=%d errno=%d\n", tag, dir, wd,
           wd < 0 ? errno : 0);
    if (wd < 0) {
        printf("events_%s=add_watch_fail errno=%d\n", tag, errno);
        close(fd);
        return;
    }
    printf("# %s initial: %s\n", tag, drain(fd, 300));

    snprintf(a, sizeof(a), "%s/a", dir);
    snprintf(b, sizeof(b), "%s/b", dir);
    snprintf(sub, sizeof(sub), "%s/sub", dir);
    snprintf(deep, sizeof(deep), "%s/sub/deep", dir);

    f = open(a, O_WRONLY | O_CREAT | O_EXCL, 0644);
    printf("# %s create_open: %s\n", tag, drain(fd, 400));
    if (f >= 0) {
        write(f, "x", 1);
        printf("# %s write: %s\n", tag, drain(fd, 400));
        close(f);
    }
    printf("# %s close_write: %s\n", tag, drain(fd, 400));
    f = open(a, O_RDONLY);
    if (f >= 0) {
        char c;
        read(f, &c, 1);
        close(f);
    }
    printf("# %s open_read_close: %s\n", tag, drain(fd, 400));
    chmod(a, 0600);
    printf("# %s chmod: %s\n", tag, drain(fd, 400));
    truncate(a, 0);
    printf("# %s truncate: %s\n", tag, drain(fd, 400));
    seen_cookie_from = seen_cookie_to = 0;
    rename(a, b);
    printf("# %s rename: %s\n", tag, drain(fd, 400));
    printf("rename_cookie_%s=%s from=%u to=%u\n", tag,
           seen_cookie_from && seen_cookie_from == seen_cookie_to ? "paired"
           : (seen_cookie_from || seen_cookie_to) ? "unpaired" : "none",
           seen_cookie_from, seen_cookie_to);
    unlink(b);
    printf("# %s unlink: %s\n", tag, drain(fd, 400));
    mkdir(sub, 0755);
    printf("# %s mkdir: %s\n", tag, drain(fd, 400));
    touch_file(deep);
    printf("subdir_%s=%s\n", tag, drain(fd, 400));
    unlink(deep);
    drain(fd, 300);
    rmdir(sub);
    printf("# %s rmdir_child: %s\n", tag, drain(fd, 400));

    child = fork();
    if (child == 0) {
        snprintf(a, sizeof(a), "%s/fromchild", dir);
        touch_file(a);
        _exit(0);
    }
    if (child > 0)
        waitpid(child, NULL, 0);
    printf("foreign_%s=%s\n", tag, drain(fd, 400));

    /* A watch on a regular file. */
    snprintf(a, sizeof(a), "%s/watched", dir);
    touch_file(a);
    drain(fd, 300);
    fwd = p_add(fd, a, IN_ALL_EVENTS);
    if (fwd < 0) {
        printf("file_watch_%s=add_fail errno=%d\n", tag, errno);
    } else {
        f = open(a, O_WRONLY | O_APPEND);
        if (f >= 0) {
            write(f, "y", 1);
            close(f);
        }
        unlink(a);
        printf("file_watch_%s=wd%d %s\n", tag, fwd, drain(fd, 500));
    }

    /* Remove the watched directory itself. */
    snprintf(a, sizeof(a), "%s/fromchild", dir);
    unlink(a);
    drain(fd, 300);
    rmdir(dir);
    printf("self_delete_%s=%s\n", tag, drain(fd, 600));
    close(fd);
}

static void probe_extra(const char *dir)
{
    char a[1100];
    int fd, wd, i, flags, r;
    struct pollfd p;
    char small[8];

    rm_tree(dir);
    mkdir(dir, 0755);

    /* poll readiness and nonblocking read with nothing queued */
    fd = p_init();
    if (fd < 0)
        return;
    wd = p_add(fd, dir, IN_CREATE);
    flags = fcntl(fd, F_GETFL);
    r = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    errno = 0;
    {
        char buf[256];
        int n = read(fd, buf, sizeof(buf));
        printf("nonblock_read=setfl:%d ret:%d errno:%d\n", r, n, errno);
    }
    fcntl(fd, F_SETFL, flags);
    snprintf(a, sizeof(a), "%s/p", dir);
    touch_file(a);
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    {
        long long t0 = now_us();
        r = poll(&p, 1, 1000);
        printf("poll_ready=ret:%d revents:0x%x latency_us:%lld\n", r,
               p.revents, now_us() - t0);
    }
    errno = 0;
    r = read(fd, small, sizeof(small));
    printf("small_read=ret:%d errno:%d\n", r, errno);
    drain(fd, 200);

    /* IN_MASK_ADD-free re-add returns the same wd? */
    r = p_add(fd, dir, IN_DELETE);
    printf("readd_same_wd=%s first:%d second:%d\n", r == wd ? "yes" : "no",
           wd, r);
    r = p_rm(fd, wd);
    printf("# rm_watch=%d errno=%d after: %s\n", r, r < 0 ? errno : 0,
           drain(fd, 300));
    close(fd);

    /* one-shot */
    fd = p_init();
    wd = p_add(fd, dir, IN_CREATE | IN_ONESHOT);
    snprintf(a, sizeof(a), "%s/o1", dir);
    touch_file(a);
    snprintf(a, sizeof(a), "%s/o2", dir);
    touch_file(a);
    printf("oneshot=wd%d %s\n", wd, drain(fd, 500));
    close(fd);

    /* overflow: many events with nobody reading */
    fd = p_init();
    wd = p_add(fd, dir, IN_CREATE | IN_DELETE);
    {
        long long t0 = now_us();
        for (i = 0; i < 20000 && now_us() - t0 < 15000000; i++) {
            snprintf(a, sizeof(a), "%s/x%d", dir, i % 50);
            touch_file(a);
            unlink(a);
        }
        printf("# overflow ops=%d (create+delete pairs) in %lld ms\n", i,
               (now_us() - t0) / 1000);
    }
    drain(fd, 500);
    {
        const char *q = strstr(text, "Q_OVERFLOW");
        printf("overflow=events:%d q_overflow:%s\n", seen_count,
               q ? "yes" : "no");
    }
    close(fd);
    rm_tree(dir);
}

static void on_alarm_noop(int sig) { (void)sig; }

/* Delivery latency, blocking read, fork inheritance, other filesystems,
 * limits. Run with "more" as the third argument. */
static void probe_more(const char *dir)
{
    char a[1100];
    int fd, wd, i, r;
    long long lat[40], t0;
    struct pollfd p;
    char buf[4096] __attribute__((aligned(8)));

    rm_tree(dir);
    mkdir(dir, 0755);
    fd = p_init();
    wd = p_add(fd, dir, IN_CREATE);
    for (i = 0; i < 40; i++) {
        snprintf(a, sizeof(a), "%s/l%d", dir, i);
        t0 = now_us();
        touch_file(a);
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        r = poll(&p, 1, 2000);
        lat[i] = r > 0 ? now_us() - t0 : -1;
        if (r > 0)
            read(fd, buf, sizeof(buf));
        usleep(i % 2 ? 50000 : 300000);
    }
    {
        long long s[40], tmp;
        int j, k, n = 0;
        for (j = 0; j < 40; j++)
            if (lat[j] >= 0)
                s[n++] = lat[j];
        for (j = 0; j < n; j++)
            for (k = j + 1; k < n; k++)
                if (s[k] < s[j]) { tmp = s[j]; s[j] = s[k]; s[k] = tmp; }
        printf("delivery_latency_us=n:%d min:%lld median:%lld p90:%lld max:%lld\n",
               n, n ? s[0] : -1, n ? s[n / 2] : -1, n ? s[n * 9 / 10] : -1,
               n ? s[n - 1] : -1);
        printf("# latencies:");
        for (j = 0; j < 40; j++)
            printf(" %lld", lat[j]);
        printf("\n");
    }

    /* Blocking read with nothing queued: does it block? (2 s alarm) */
    {
        struct sigaction sa, old;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_alarm_noop;
        sigaction(SIGALRM, &sa, &old);
        alarm(2);
        t0 = now_us();
        errno = 0;
        r = read(fd, buf, sizeof(buf));
        printf("blocking_read_empty=ret:%d errno:%d waited_ms:%lld\n", r,
               errno, (now_us() - t0) / 1000);
        alarm(0);
        sigaction(SIGALRM, &old, NULL);
        alarm(180);
    }

    /* Blocking read woken by an event from a child. */
    {
        pid_t c = fork();
        if (c == 0) {
            usleep(300000);
            snprintf(a, sizeof(a), "%s/wake", dir);
            touch_file(a);
            _exit(0);
        }
        t0 = now_us();
        r = read(fd, buf, sizeof(buf));
        printf("blocking_read_wake=ret:%d waited_ms:%lld\n", r,
               (now_us() - t0) / 1000);
        waitpid(c, NULL, 0);
    }

    /* Fork: does the child's copy of the fd share the queue? */
    {
        int pipefd[2];
        pid_t c;
        pipe(pipefd);
        c = fork();
        if (c == 0) {
            char m[64];
            int n;
            p.fd = fd;
            p.events = POLLIN;
            p.revents = 0;
            n = poll(&p, 1, 2000);
            if (n > 0)
                n = read(fd, buf, sizeof(buf));
            snprintf(m, sizeof(m), "%d", n);
            write(pipefd[1], m, strlen(m) + 1);
            _exit(0);
        }
        usleep(200000);
        snprintf(a, sizeof(a), "%s/forked", dir);
        touch_file(a);
        {
            char m[64] = "";
            read(pipefd[0], m, sizeof(m));
            waitpid(c, NULL, 0);
            printf("fork_shared_queue=child_read:%s parent_after:%s\n", m,
                   drain(fd, 300));
        }
        close(pipefd[0]);
        close(pipefd[1]);
    }
    p_rm(fd, wd);
    drain(fd, 200);

    /* Watches on other filesystems. */
    {
        static const char *paths[] = {
            "/dev/shmem", "/tmp", "/base/bin", "/pps", "/accounts/1000/shared",
            "/accounts/1000/removable/sdcard", "/proc", "/",
        };
        unsigned k;
        for (k = 0; k < sizeof(paths) / sizeof(paths[0]); k++) {
            errno = 0;
            r = p_add(fd, paths[k], IN_CREATE | IN_DELETE);
            printf("# add_watch(%s)=%d errno=%d\n", paths[k], r,
                   r < 0 ? errno : 0);
            if (r >= 0)
                p_rm(fd, r);
        }
        drain(fd, 200);
        /* /dev/shmem: create an object directly (no subdirectories there). */
        r = p_add(fd, "/dev/shmem", IN_CREATE | IN_DELETE);
        if (r >= 0) {
            touch_file("/dev/shmem/inotify-probe-obj");
            unlink("/dev/shmem/inotify-probe-obj");
            printf("shmem_events=%s\n", drain(fd, 600));
            p_rm(fd, r);
            drain(fd, 200);
        } else {
            printf("shmem_events=add_fail errno=%d\n", errno);
        }
    }

    /* Error cases. */
    errno = 0;
    r = p_add(fd, "/nonexistent-inotify-probe", IN_CREATE);
    printf("err_enoent=ret:%d errno:%d\n", r, errno);
    errno = 0;
    r = p_add(fd, dir, 0);
    printf("err_zero_mask=ret:%d errno:%d\n", r, errno);
    errno = 0;
    r = p_rm(fd, 9999);
    printf("err_bad_wd=ret:%d errno:%d\n", r, errno);
    errno = 0;
    r = p_add(fd, dir, 0x01000000 /* IN_ONLYDIR */ | IN_CREATE);
    printf("onlydir_on_dir=ret:%d errno:%d\n", r, errno);
    snprintf(a, sizeof(a), "%s/l0", dir);
    errno = 0;
    r = p_add(fd, a, 0x01000000 | IN_CREATE);
    printf("onlydir_on_file=ret:%d errno:%d\n", r, errno);
    {
        /* FIONREAD with one event queued. */
        int n = -7, w;
        w = p_add(fd, dir, IN_CREATE);
        drain(fd, 200);
        snprintf(a, sizeof(a), "%s/fionread", dir);
        touch_file(a);
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        poll(&p, 1, 1000);
        errno = 0;
        r = ioctl(fd, FIONREAD, &n);
        printf("fionread=ret:%d errno:%d value:%d\n", r, errno, n);
        drain(fd, 200);
        /* IN_MASK_ADD: CREATE watch, add DELETE; does CREATE stay? */
        r = p_add(fd, dir, IN_DELETE | 0x20000000);
        snprintf(a, sizeof(a), "%s/maskadd", dir);
        touch_file(a);
        unlink(a);
        printf("mask_add=wd%d/%d %s\n", r, w, drain(fd, 600));
        p_rm(fd, w);
        drain(fd, 200);
        /* IN_DONT_FOLLOW on a symlink to the directory. */
        snprintf(a, sizeof(a), "%s.link", dir);
        unlink(a);
        symlink(dir, a);
        errno = 0;
        r = p_add(fd, a, IN_CREATE | IN_ATTRIB | 0x02000000);
        {
            char t[1100];
            snprintf(t, sizeof(t), "%s/viafollow", dir);
            touch_file(t);
        }
        printf("dont_follow=ret:%d errno:%d %s\n", r, errno, drain(fd, 600));
        unlink(a);
    }
    close(fd);

    /* Limits: instances and watches. */
    {
        int fds[300], n = 0;
        for (i = 0; i < 300; i++) {
            fds[i] = p_init();
            if (fds[i] < 0)
                break;
            n++;
        }
        printf("max_instances=%d%s errno=%d\n", n, n == 300 ? "+" : "",
               n < 300 ? errno : 0);
        for (i = 0; i < n; i++)
            close(fds[i]);
    }
    {
        int n = 0;
        fd = p_init();
        for (i = 0; i < 2000; i++) {
            snprintf(a, sizeof(a), "%s/w%d", dir, i);
            touch_file(a);
            if (p_add(fd, a, IN_MODIFY) < 0)
                break;
            n++;
        }
        printf("max_watches=%d%s errno=%d\n", n, n == 2000 ? "+" : "",
               n < 2000 ? errno : 0);
        close(fd);
    }
    rm_tree(dir);
}

/* add_watch on a special file, bounded by a 3 s interrupting alarm; when
 * it succeeds, poke the file and report the events. */
static void probe_special_one(const char *tag, const char *path, int is_fifo)
{
    struct sigaction sa, old;
    long long t0;
    int fd, wd, saved;
    fd = p_init();
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm_noop;
    sigaction(SIGALRM, &sa, &old);
    alarm(3);
    t0 = now_us();
    errno = 0;
    wd = p_add(fd, path, IN_MODIFY | IN_ATTRIB | IN_OPEN | IN_CLOSE_WRITE |
                             IN_CLOSE_NOWRITE);
    saved = errno;
    alarm(0);
    sigaction(SIGALRM, &old, NULL);
    alarm(180);
    printf("special_%s=add:%d errno:%d waited_ms:%lld", tag, wd,
           wd < 0 ? saved : 0, (now_us() - t0) / 1000);
    if (wd >= 0) {
        if (is_fifo) {
            int f = open(path, O_RDWR | O_NONBLOCK);
            if (f >= 0) {
                write(f, "x", 1);
                close(f);
            }
        }
        chmod(path, 0600);
        printf(" %s", drain(fd, 1000));
    }
    printf("\n");
    close(fd);
}

static void probe_special(const char *dir)
{
    char a[1100];
    struct sockaddr_un sun;
    int s;
    rm_tree(dir);
    mkdir(dir, 0755);
    snprintf(a, sizeof(a), "%s/fifo", dir);
    if (mkfifo(a, 0644) != 0)
        printf("special_fifo=mkfifo_fail errno:%d\n", errno);
    else
        probe_special_one("fifo", a, 1);
    snprintf(a, sizeof(a), "%s/sock", dir);
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", a);
    s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0 || bind(s, (struct sockaddr *)&sun, SUN_LEN(&sun)) != 0) {
        printf("special_socket=bind_fail errno:%d\n", errno);
    } else {
        listen(s, 1);
        probe_special_one("socket", a, 0);
    }
    if (s >= 0)
        close(s);
    rm_tree(dir);
}

static void probe_ionotify(const char *dir)
{
    int fd = open(dir, O_RDONLY);
    struct sigevent ev;
    int r;
    if (fd < 0) {
        printf("ionotify_dir=open_fail errno=%d\n", errno);
        return;
    }
    SIGEV_NONE_INIT(&ev);
    r = ionotify(fd, _NOTIFY_ACTION_POLL,
                 _NOTIFY_COND_INPUT | _NOTIFY_COND_OUTPUT |
                     _NOTIFY_COND_OBAND | _NOTIFY_COND_EXTEN,
                 &ev);
    printf("ionotify_dir=ret:0x%x errno:%d\n", r, r < 0 ? errno : 0);
    close(fd);
}

static void probe_scan(const char *dir)
{
    char a[1100];
    struct stat st1, st2;
    int i, n;
    long long t0, best = -1;

    rm_tree(dir);
    mkdir(dir, 0755);
    stat(dir, &st1);
    usleep(200000);
    snprintf(a, sizeof(a), "%s/m", dir);
    touch_file(a);
    stat(dir, &st2);
    printf("dir_mtime_resolution=changed_after_200ms:%s mtime:%ld->%ld "
           "(QNX stat has whole seconds)\n",
           st1.st_mtime != st2.st_mtime ? "yes" : "no",
           (long)st1.st_mtime, (long)st2.st_mtime);
    for (i = 0; i < 1000; i++) {
        snprintf(a, sizeof(a), "%s/f%04d", dir, i);
        touch_file(a);
    }
    for (i = 0; i < 3; i++) {
        DIR *d = opendir(dir);
        struct dirent *de;
        t0 = now_us();
        n = 0;
        while (d && (de = readdir(d))) {
            struct stat st;
            snprintf(a, sizeof(a), "%s/%s", dir, de->d_name);
            lstat(a, &st);
            n++;
        }
        if (d)
            closedir(d);
        t0 = now_us() - t0;
        if (best < 0 || t0 < best)
            best = t0;
    }
    printf("scan_cost_us=%lld entries:%d dir:%s\n", best, n, dir);
    rm_tree(dir);
}

int main(int argc, char **argv)
{
    const char *qnx6_dir = argc > 1 ? argv[1] : "./inotify-probe.d";
    const char *shm_dir = argc > 2 ? argv[2] : "/dev/shmem/inotify-probe.d";
    char extra[1100];
    int fd;

    if (argc > 3 && strcmp(argv[3], "more") == 0) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        signal(SIGALRM, alarm_handler);
        alarm(180);
        p_init = (int (*)(void))dlsym(RTLD_DEFAULT, "inotify_init");
        p_add = (int (*)(int, const char *, uint32_t))dlsym(
            RTLD_DEFAULT, "inotify_add_watch");
        p_rm = (int (*)(int, int32_t))dlsym(RTLD_DEFAULT, "inotify_rm_watch");
        if (!p_init || !p_add || !p_rm)
            return 1;
        snprintf(extra, sizeof(extra), "%s.more", qnx6_dir);
        probe_more(extra);
        printf("probe_done=1\n");
        return 0;
    }

    if (argc > 3 && strcmp(argv[3], "special") == 0) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        signal(SIGALRM, alarm_handler);
        alarm(180);
        p_init = (int (*)(void))dlsym(RTLD_DEFAULT, "inotify_init");
        p_add = (int (*)(int, const char *, uint32_t))dlsym(
            RTLD_DEFAULT, "inotify_add_watch");
        p_rm = (int (*)(int, int32_t))dlsym(RTLD_DEFAULT, "inotify_rm_watch");
        if (!p_init || !p_add || !p_rm)
            return 1;
        snprintf(extra, sizeof(extra), "%s.special", qnx6_dir);
        probe_special(extra);
        printf("probe_done=1\n");
        return 0;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGALRM, alarm_handler);
    alarm(180);

    p_init = (int (*)(void))dlsym(RTLD_DEFAULT, "inotify_init");
    p_add = (int (*)(int, const char *, uint32_t))dlsym(RTLD_DEFAULT,
                                                        "inotify_add_watch");
    p_rm = (int (*)(int, int32_t))dlsym(RTLD_DEFAULT, "inotify_rm_watch");
    printf("native_inotify_symbols=init:%s add:%s rm:%s\n",
           p_init ? "yes" : "no", p_add ? "yes" : "no", p_rm ? "yes" : "no");

    fd = open("/dev/fsnotify", O_RDONLY);
    printf("fsnotify_open=rdonly:%d/%d", fd, fd < 0 ? errno : 0);
    if (fd >= 0)
        close(fd);
    fd = open("/dev/fsnotify", O_RDWR);
    printf(" rdwr:%d/%d\n", fd, fd < 0 ? errno : 0);
    if (fd >= 0)
        close(fd);
    fd = open("/dev/fsevents", O_RDONLY);
    printf("fsevents_open=rdonly:%d/%d", fd, fd < 0 ? errno : 0);
    if (fd >= 0)
        close(fd);
    fd = open("/dev/fsevents", O_RDWR);
    printf(" rdwr:%d/%d\n", fd, fd < 0 ? errno : 0);
    if (fd >= 0)
        close(fd);

    probe_ionotify(".");

    if (!p_init || !p_add || !p_rm) {
        printf("native_inotify_init=absent\n");
    } else {
        fd = p_init();
        printf("native_inotify_init=%d errno=%d\n", fd, fd < 0 ? errno : 0);
        if (fd >= 0) {
            close(fd);
            probe_fs("qnx6", qnx6_dir);
            probe_fs("shmem", shm_dir);
            snprintf(extra, sizeof(extra), "%s.extra", qnx6_dir);
            probe_extra(extra);
        }
    }
    snprintf(extra, sizeof(extra), "%s.scan", qnx6_dir);
    probe_scan(extra);
    printf("probe_done=1\n");
    return 0;
}
