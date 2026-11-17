/*
 * Phase 1 stress gate (guest fixture, musl): repeated duplication,
 * replacement, fork, exec and close of path-recorded directory fds, wake
 * eventfds and pipes, single-threaded and with threads, must leave no
 * leaked descriptor and no stale record.
 *
 * Exits: 2 getdents, 3 eventfd, 4 fork child, 5 exec child, 6 threaded,
 * 7-10 open-fd count differs from the baseline after phase A-D.
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

struct fd_set3 { int dir, event, pipe_read, pipe_write; };

static int count_open(void)
{
    struct rlimit limit;
    int fd, count = 0, top = 4096;
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur < 4096)
        top = (int)limit.rlim_cur;
    for (fd = 0; fd < top; ++fd) count += fcntl(fd, F_GETFD) >= 0;
    return count;
}

static int dir_works(int fd)
{
    char buffer[2048];
    return syscall(SYS_getdents64, fd, buffer, sizeof(buffer)) > 0;
}

static int event_works(int writer, int reader)
{
    uint64_t value = 1;
    return write(writer, &value, sizeof(value)) == sizeof(value) &&
        read(reader, &value, sizeof(value)) == sizeof(value);
}

static int make_set(struct fd_set3 *set)
{
    int p[2];
    set->dir = open("/etc", O_RDONLY | O_DIRECTORY);
    set->event = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (pipe(p) != 0 || set->dir < 0 || set->event < 0) return -1;
    set->pipe_read = p[0];
    set->pipe_write = p[1];
    return 0;
}

static void close_set(struct fd_set3 *set)
{
    close(set->dir); close(set->event);
    close(set->pipe_read); close(set->pipe_write);
}

static int phase_a(void)
{
    int i;
    for (i = 0; i < 1000; ++i) {
        struct fd_set3 set;
        int a, b, c, d;
        if (make_set(&set) != 0) return 2;
        a = dup(set.dir);
        b = dup2(set.event, 200 + i % 50);
        c = dup3(set.pipe_read, 260 + i % 20, O_CLOEXEC);
        d = fcntl(set.event, F_DUPFD_CLOEXEC, 10);
        if (a < 0 || !dir_works(a)) return 2;
        if (b < 0 || d < 0 || !event_works(b, d)) return 3;
        if (c < 0) return 3;
        close(a); close(b); close(c); close(d);
        close_set(&set);
    }
    return 0;
}

static int phase_b(void)
{
    int i;
    for (i = 0; i < 200; ++i) {
        struct fd_set3 set;
        int status;
        pid_t child;
        if (make_set(&set) != 0) return 2;
        child = fork();
        if (child == 0)
            _exit(dir_works(set.dir) && event_works(set.event, set.event) ?
                0 : 1);
        if (child < 0 || waitpid(child, &status, 0) != child ||
                !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 4;
        close_set(&set);
    }
    return 0;
}

static int phase_c(const char *self)
{
    int i;
    for (i = 0; i < 50; ++i) {
        struct fd_set3 set;
        char dir_text[16], event_text[16];
        int status;
        pid_t child;
        if (make_set(&set) != 0) return 2;
        if (fcntl(set.event, F_SETFD, 0) != 0) return 5;
        snprintf(dir_text, sizeof(dir_text), "%d", set.dir);
        snprintf(event_text, sizeof(event_text), "%d", set.event);
        child = fork();
        if (child == 0) {
            execl(self, self, "exec-child", dir_text, event_text, (char *)0);
            _exit(1);
        }
        if (child < 0 || waitpid(child, &status, 0) != child ||
                !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 5;
        close_set(&set);
    }
    return 0;
}

static int shared_dir, shared_event;

static void *churn(void *argument)
{
    int i;
    (void)argument;
    for (i = 0; i < 2000; ++i) {
        int x = dup(shared_event), y = dup(shared_dir);
        if (x >= 0) close(x);
        if (y >= 0) close(y);
    }
    return 0;
}

static int phase_d(void)
{
    struct fd_set3 set;
    pthread_t threads[4];
    int i;
    if (make_set(&set) != 0) return 2;
    shared_dir = set.dir;
    shared_event = set.event;
    for (i = 0; i < 4; ++i)
        if (pthread_create(&threads[i], 0, churn, 0) != 0) return 6;
    for (i = 0; i < 4; ++i) pthread_join(threads[i], 0);
    if (!dir_works(set.dir) || !event_works(set.event, set.event)) return 6;
    close_set(&set);
    return 0;
}

int main(int argc, char **argv)
{
    int baseline, result;
    if (argc == 4 && strcmp(argv[1], "exec-child") == 0)
        return dir_works(atoi(argv[2])) &&
            event_works(atoi(argv[3]), atoi(argv[3])) ? 0 : 1;
    baseline = count_open();
    if ((result = phase_a()) != 0) return result;
    if (count_open() != baseline) return 7;
    if ((result = phase_b()) != 0) return result;
    if (count_open() != baseline) return 8;
    if ((result = phase_c(argv[0])) != 0) return result;
    if (count_open() != baseline) return 9;
    if ((result = phase_d()) != 0) return result;
    if (count_open() != baseline) return 10;
    return 0;
}
