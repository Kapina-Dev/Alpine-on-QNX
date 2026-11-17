/*
 * Exec carry-over of fd records (guest fixture, musl).
 *
 * Parent: a directory fd dup2'd to 300 and a wake eventfd e that survives
 * exec, plus a close-on-exec wake eventfd ec.  It execs itself with the
 * set of fds the child may see: the fds open before the test, 300, and
 * whatever appeared when e was created (its hidden write end, if visible).
 * Child: getdents64 on 300 works (exit 2), e still works as an eventfd
 * (exit 3), and no other fd is open, including ec and its write end (4).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MAX_SCAN 4096

static int scan_limit(void)
{
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur > MAX_SCAN)
        return MAX_SCAN;
    return (int)limit.rlim_cur;
}

static void open_set(unsigned char *set)
{
    int fd, limit = scan_limit();
    memset(set, 0, MAX_SCAN);
    for (fd = 0; fd < limit; ++fd) set[fd] = fcntl(fd, F_GETFD) >= 0;
}

static int child(char **argv)
{
    unsigned char allowed[MAX_SCAN], now[MAX_SCAN];
    char buffer[4096], *cursor;
    uint64_t value = 1;
    int e = atoi(argv[3]), fd;
    if (syscall(SYS_getdents64, 300, buffer, sizeof(buffer)) <= 0) return 2;
    if (write(e, &value, sizeof(value)) != sizeof(value) ||
            read(e, &value, sizeof(value)) != sizeof(value)) return 3;
    memset(allowed, 0, sizeof(allowed));
    for (cursor = argv[4]; *cursor != '\0'; ) {
        fd = (int)strtol(cursor, &cursor, 10);
        if (fd >= 0 && fd < MAX_SCAN) allowed[fd] = 1;
        if (*cursor == ',') ++cursor;
    }
    open_set(now);
    for (fd = 0; fd < MAX_SCAN; ++fd)
        if (now[fd] && !allowed[fd]) {
            struct stat st;
            fstat(fd, &st);
            fprintf(stderr, "unexpected fd %d after exec: mode=%o ino=%lu "
                "cloexec=%d\n", fd, (unsigned)st.st_mode,
                (unsigned long)st.st_ino, fcntl(fd, F_GETFD));
            return 4;
        }
    return 0;
}

int main(int argc, char **argv)
{
    unsigned char before[MAX_SCAN], with_e[MAX_SCAN];
    char e_text[16], allowed[MAX_SCAN * 6], *p = allowed;
    int d, e, ec, fd;
    if (argc == 5 && strcmp(argv[1], "child") == 0) return child(argv);
    open_set(before);
    d = open("/etc", O_RDONLY | O_DIRECTORY);
    if (d < 0 || dup2(d, 300) != 300) return 10;
    close(d);
    e = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (e < 0 || fcntl(e, F_SETFD, 0) != 0) return 11;
    open_set(with_e);
    ec = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ec < 0) return 12;
    for (fd = 0; fd < MAX_SCAN; ++fd)
        if (before[fd] || with_e[fd] || fd == 300)
            p += sprintf(p, "%s%d", p == allowed ? "" : ",", fd);
    snprintf(e_text, sizeof(e_text), "%d", e);
    execl("/proc/self/exe", argv[0], "child", "300", e_text, allowed,
        (char *)0);
    execl(argv[0], argv[0], "child", "300", e_text, allowed, (char *)0);
    return 13;
}
