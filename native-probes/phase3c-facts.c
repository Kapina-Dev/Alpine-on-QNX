/*
 * Phase 3 part 3 follow-up facts: inotify fd identity (fstat), survival
 * across execve, delivery after SCM_RIGHTS, and the exact overflow record.
 * Usage: phase3c-facts <qnx6-dir>   (internal: phase3c-facts --exec <fd> <dir>)
 */
#define _QNX_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

static void touch_file(const char *p)
{
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
        close(fd);
}

/* Waits up to 2 s for a CREATE named name; returns 1 if seen. */
static int saw_create(int fd, const char *name)
{
    char buf[8192] __attribute__((aligned(8)));
    struct pollfd p = { fd, POLLIN, 0 };
    int r, off;
    while (poll(&p, 1, 2000) > 0) {
        r = read(fd, buf, sizeof(buf));
        if (r <= 0)
            return 0;
        for (off = 0; off + 16 <= r;) {
            struct inotify_event *e = (struct inotify_event *)(buf + off);
            if ((e->mask & IN_CREATE) && e->len && !strcmp(e->name, name))
                return 1;
            off += 16 + e->len;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    char dir[1024], path[1100];
    int a, b, wd, i;
    struct stat sa, sb, sd;

    signal(SIGALRM, SIG_DFL);
    alarm(120);
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc == 4 && !strcmp(argv[1], "--exec")) {
        int fd = atoi(argv[2]);
        struct stat st;
        printf("# exec image: fstat(%d)=%d\n", fd, fstat(fd, &st));
        snprintf(path, sizeof(path), "%s/afterexec", argv[3]);
        touch_file(path);
        printf("exec_survives=%s\n", saw_create(fd, "afterexec") ? "events" : "no_events");
        return 0;
    }
    if (argc < 2)
        return 2;
    snprintf(dir, sizeof(dir), "%s", argv[1]);
    snprintf(path, sizeof(path), "rm -rf '%s'; mkdir -p '%s'", dir, dir);
    system(path);

    a = inotify_init();
    b = inotify_init();
    printf("# fstat a=%d b=%d\n", fstat(a, &sa), fstat(b, &sb));
    stat("/dev/fsnotify", &sd);
    printf("identity=a:%u/%llu mode:0%o rdev:%u b:%u/%llu dev_fsnotify:%u/%llu rdev:%u same_ab:%s same_dev:%s\n",
           (unsigned)sa.st_dev, (unsigned long long)sa.st_ino, (unsigned)sa.st_mode,
           (unsigned)sa.st_rdev, (unsigned)sb.st_dev, (unsigned long long)sb.st_ino,
           (unsigned)sd.st_dev, (unsigned long long)sd.st_ino, (unsigned)sd.st_rdev,
           sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino ? "yes" : "no",
           sa.st_dev == sd.st_dev && sa.st_ino == sd.st_ino ? "yes" : "no");
    {
        int devnull = open("/dev/null", O_RDONLY);
        struct stat sn;
        fstat(devnull, &sn);
        printf("# /dev/null identity=%u/%llu same_as_inotify:%s\n", (unsigned)sn.st_dev,
               (unsigned long long)sn.st_ino,
               sn.st_dev == sa.st_dev && sn.st_ino == sa.st_ino ? "yes" : "no");
        close(devnull);
    }
    close(b);

    /* exec */
    wd = inotify_add_watch(a, dir, IN_CREATE);
    {
        pid_t c = fork();
        if (c == 0) {
            char fdtext[16];
            snprintf(fdtext, sizeof(fdtext), "%d", a);
            execl(argv[0], argv[0], "--exec", fdtext, dir, (char *)0);
            _exit(127);
        }
        waitpid(c, NULL, 0);
    }
    /* drain parent's view of the same queue */
    { char buf[8192]; int fl = fcntl(a, F_GETFL); fcntl(a, F_SETFL, fl | O_NONBLOCK);
      while (read(a, buf, sizeof(buf)) > 0) ; fcntl(a, F_SETFL, fl); }

    /* SCM_RIGHTS to a fresh-instance-free child: child closes its inherited copy first */
    {
        int sv[2];
        pid_t c;
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        c = fork();
        if (c == 0) {
            char data, cbuf[CMSG_SPACE(sizeof(int))];
            struct iovec io = { &data, 1 };
            struct msghdr m;
            struct cmsghdr *cm;
            int got = -1;
            close(a);
            memset(&m, 0, sizeof(m));
            m.msg_iov = &io; m.msg_iovlen = 1;
            m.msg_control = cbuf; m.msg_controllen = sizeof(cbuf);
            {
                int rr = recvmsg(sv[1], &m, 0);
                int e = errno;
                cm = CMSG_FIRSTHDR(&m);
                if (rr > 0 && cm)
                    memcpy(&got, CMSG_DATA(cm), sizeof(int));
                printf("# rights child recvmsg=%d errno=%d flags=0x%x controllen=%u cmsg=%p got fd %d\n",
                       rr, rr < 0 ? e : 0, m.msg_flags, (unsigned)m.msg_controllen, (void *)cm, got);
            }
            write(sv[1], "r", 1);
            printf("rights_delivers=%s\n", got >= 0 && saw_create(got, "afterrights") ? "events" : "no_events");
            _exit(0);
        }
        {
            char data = 'x', cbuf[CMSG_SPACE(sizeof(int))], ack;
            struct iovec io = { &data, 1 };
            struct msghdr m;
            struct cmsghdr *cm;
            memset(&m, 0, sizeof(m));
            m.msg_iov = &io; m.msg_iovlen = 1;
            m.msg_control = cbuf; m.msg_controllen = sizeof(cbuf);
            cm = CMSG_FIRSTHDR(&m);
            cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS;
            cm->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cm), &a, sizeof(int));
            printf("# rights sendmsg=%d errno=%d\n", (int)sendmsg(sv[0], &m, 0), errno);
            read(sv[0], &ack, 1);
            snprintf(path, sizeof(path), "%s/afterrights", dir);
            touch_file(path);
            waitpid(c, NULL, 0);
        }
    }
    inotify_rm_watch(a, wd);
    close(a);

    /* overflow record: fill without reading, then dump the records with wd<0 or Q_OVERFLOW */
    a = inotify_init();
    wd = inotify_add_watch(a, dir, IN_CREATE | IN_DELETE);
    for (i = 0; i < 1400; i++) {
        snprintf(path, sizeof(path), "%s/o%d", dir, i % 40);
        touch_file(path);
        unlink(path);
    }
    sleep(1);
    {
        char buf[65536] __attribute__((aligned(8)));
        int r, off, total = 0, found = 0, fion = -1;
        ioctl(a, FIONREAD, &fion);
        printf("# fionread_full=%d\n", fion);
        fcntl(a, F_SETFL, fcntl(a, F_GETFL) | O_NONBLOCK);
        while ((r = read(a, buf, sizeof(buf))) > 0) {
            for (off = 0; off + 16 <= r;) {
                struct inotify_event *e = (struct inotify_event *)(buf + off);
                total++;
                if (e->mask & IN_Q_OVERFLOW) {
                    found++;
                    printf("overflow_record=wd:%d mask:0x%x cookie:%u len:%u position:%d\n",
                           e->wd, e->mask, e->cookie, e->len, total);
                }
                off += 16 + e->len;
            }
        }
        printf("# overflow total_records=%d overflow_records=%d\n", total, found);
    }
    close(a);
    snprintf(path, sizeof(path), "rm -rf '%s'", dir);
    system(path);
    printf("probe_done=1\n");
    return 0;
}
