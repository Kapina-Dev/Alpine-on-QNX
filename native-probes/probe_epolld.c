/*
 * Quick look at RIM's /dev/android/epoll: owner, group and mode of it and
 * /dev/binder, then an O_RDWR open and the epoll_ctl devctl. Protocol
 * recovery is out of scope, so the devctl is recorded as "not attempted".
 * Prints key=value lines.
 */
#define _QNX_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void show(const char *path)
{
    struct stat st;
    struct passwd *pw;
    struct group *gr;

    if (stat(path, &st) < 0) {
        printf("stat.%s=fail errno=%d (%s)\n", path, errno, strerror(errno));
        return;
    }
    pw = getpwuid(st.st_uid);
    gr = getgrgid(st.st_gid);
    printf("stat.%s=uid=%d(%s) gid=%d(%s) mode=%o\n", path, (int)st.st_uid,
           pw ? pw->pw_name : "?", (int)st.st_gid, gr ? gr->gr_name : "?",
           (unsigned)(st.st_mode & 07777));
}

int main(void)
{
    int fd;

    alarm(20);
    show("/dev/android/epoll");
    show("/dev/binder");
    fd = open("/dev/android/epoll", O_RDWR);
    if (fd < 0) {
        printf("open./dev/android/epoll=fail errno=%d (%s)\n", errno, strerror(errno));
        printf("epoll_ctl_devctl=not attempted (open failed)\n");
        return 0;
    }
    printf("open./dev/android/epoll=ok fd=%d\n", fd);
    printf("epoll_ctl_devctl=not attempted (protocol recovery out of scope)\n");
    close(fd);
    return 0;
}
