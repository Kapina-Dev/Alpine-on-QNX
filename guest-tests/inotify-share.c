/*
 * inotify across dup, fork, execve and SCM_RIGHTS (guest fixture, musl).
 *
 * Linuxemu backs inotify with QNX's native one: the queue, the watches
 * and O_NONBLOCK live on the native open-file description, events arrive
 * about 250 ms late, so waits are bounded by 2 s polls.  Watches ask for
 * IN_CREATE only.  Every check arms a watchdog that ends the process with
 * 100 + the check number; forked children and exec'd images are bounded
 * by alarm(5), which shows as a SIGALRM death.
 *
 * Checks (exit code = check number, printed on stderr with errno):
 *   10 setup failed
 *   11 dup and dup3(..., O_CLOEXEC) copies read from the same queue (an
 *      event read through a copy is gone from the original), the dup3
 *      copy is close-on-exec, and O_NONBLOCK set through a copy makes an
 *      empty read on the original fail EAGAIN
 *   12 a forked child reads a CREATE produced after the fork by the
 *      parent's watch, and the parent then finds the queue empty
 *   13 without IN_CLOEXEC the fd survives execve of this fixture
 *      ("--exec FD DIR"): the new image creates a file and reads its
 *      CREATE through the watch added before exec, inotify_add_watch on
 *      the fd succeeds (the fd is an inotify fd to Linuxemu, not just a
 *      native number), and an empty nonblocking read fails EAGAIN
 *   14 an IN_CLOEXEC fd is gone after execve ("--cloexec FD"):
 *      fcntl(fd, F_GETFD) fails EBADF
 *   15 sendmsg (and sendmmsg) with the inotify fd in SCM_RIGHTS over a
 *      socketpair fails EOPNOTSUPP and sends nothing (QNX cannot pass an
 *      inotify fd: its receiver would fail EACCES and lose the message); a
 *      following send of "ok" arrives intact, and a sendmsg passing a pipe
 *      fd still works (the peer reads through it)
 *
 * Exit codes of a fork child or exec'd image (printed by the parent):
 * 3 exec failed, 4-9 the step that failed (see the code).
 *
 * INOTIFY_SHARE_ONLY=N runs check N alone.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DIR_PATH "/tmp/insh-dir"
#define EVENT_WAIT_MS 2000

static volatile int current_check;
static volatile int watchdog_ticks;

static int fail(int check, const char *what)
{
    fprintf(stderr, "inotify-share check %d failed: %s (errno %d %s)\n",
        check, what, errno, strerror(errno));
    return check;
}

static void sleep_ms(int milliseconds)
{
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

static void *watchdog(void *argument)
{
    (void)argument;
    for (;;) {
        sleep_ms(100);
        if (__sync_add_and_fetch(&watchdog_ticks, 1) >= 150) {
            fprintf(stderr, "inotify-share check %d timed out\n",
                current_check);
            _exit(100 + current_check);
        }
    }
    return 0;
}

/* Starts the check's 15 s bound. */
static void arm(int check)
{
    static int started;
    current_check = check;
    watchdog_ticks = 0;
    __sync_synchronize();
    if (!started) {
        pthread_t thread;
        if (pthread_create(&thread, 0, watchdog, 0) == 0) started = 1;
    }
}

static int make_file(const char *directory, const char *name)
{
    char path[PATH_MAX];
    int fd;
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return -1;
    return close(fd);
}

/* Waits up to 2 s for fd to poll readable, then reads one buffer, which
   must hold exactly one record: wd (any when wd is -1), IN_CREATE, name.
   1 when it does, else 0 (the reason on stderr). */
static int read_create(int fd, int wd, const char *name)
{
    char buffer[4096] __attribute__((aligned(4)));
    const struct inotify_event *event =
        (const struct inotify_event *)buffer;
    struct pollfd entry;
    ssize_t got;
    entry.fd = fd;
    entry.events = POLLIN;
    entry.revents = 0;
    if (poll(&entry, 1, EVENT_WAIT_MS) != 1) {
        fprintf(stderr, "inotify-share: no CREATE '%s' on fd %d\n", name,
            fd);
        return 0;
    }
    got = read(fd, buffer, sizeof(buffer));
    if (got < (ssize_t)sizeof(*event) ||
            got != (ssize_t)(sizeof(*event) + event->len) ||
            (wd != -1 && event->wd != wd) || event->mask != IN_CREATE ||
            event->len == 0 ||
            strncmp(event->name, name, event->len) != 0) {
        fprintf(stderr, "inotify-share: fd %d read %d bytes, wd %d mask "
            "0x%x, want CREATE '%s'\n", fd, (int)got,
            got >= (ssize_t)sizeof(*event) ? event->wd : 0,
            got >= (ssize_t)sizeof(*event) ? event->mask : 0, name);
        return 0;
    }
    return 1;
}

/* An empty read fails EAGAIN: 1, else 0. */
static int read_eagain(int fd)
{
    char buffer[512];
    errno = 0;
    return read(fd, buffer, sizeof(buffer)) == -1 && errno == EAGAIN;
}

static void remove_tree(const char *path)
{
    DIR *directory = opendir(path);
    if (directory != 0) {
        struct dirent *entry;
        char child[PATH_MAX];
        while ((entry = readdir(directory)) != 0) {
            if (strcmp(entry->d_name, ".") == 0 ||
                    strcmp(entry->d_name, "..") == 0) continue;
            snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
            unlink(child);
        }
        closedir(directory);
    }
    rmdir(path);
}

/* Fresh inotify fd watching DIR_PATH for IN_CREATE; -1 after reporting. */
static int open_watch(int check, int flags, int *wd)
{
    int fd = inotify_init1(flags);
    if (fd < 0) { fail(check, "inotify_init1"); return -1; }
    *wd = inotify_add_watch(fd, DIR_PATH, IN_CREATE);
    if (*wd < 0) { fail(check, "inotify_add_watch"); close(fd); return -1; }
    return fd;
}

/* Exit status of a child, or 0x100 + signal when it died of one. */
static int wait_child(pid_t pid)
{
    int status;
    if (waitpid(pid, &status, 0) != pid) return 0x200;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return WIFSIGNALED(status) ? 0x100 + WTERMSIG(status) : 0x200;
}

static int child_failed(int check, int status, const char *what)
{
    fprintf(stderr, "inotify-share: check %d child status %#x\n", check,
        status);
    errno = 0;
    return fail(check, what);
}

static int check_dup(void)
{
    int wd, copy, copy3, flags, result = 0;
    int fd = open_watch(11, 0, &wd);
    if (fd < 0) return 11;
    copy = dup(fd);
    copy3 = dup3(fd, fd + 10, O_CLOEXEC);
    if (copy < 0 || copy3 != fd + 10) { result = fail(11, "dup or dup3"); goto done; }
    flags = fcntl(copy3, F_GETFD);
    if (flags < 0 || (flags & FD_CLOEXEC) == 0 ||
            (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0) {
        result = fail(11, "dup3 O_CLOEXEC not on the copy alone");
        goto done;
    }
    if (make_file(DIR_PATH, "dup-a") != 0) { result = fail(10, "create"); goto done; }
    if (!read_create(copy, wd, "dup-a")) { errno = 0; result = fail(11, "dup copy didn't read"); goto done; }
    if (make_file(DIR_PATH, "dup-b") != 0) { result = fail(10, "create"); goto done; }
    if (!read_create(copy3, wd, "dup-b")) { errno = 0; result = fail(11, "dup3 copy didn't read"); goto done; }
    flags = fcntl(copy, F_GETFL);
    if (flags < 0 || fcntl(copy, F_SETFL, flags | O_NONBLOCK) != 0) {
        result = fail(11, "F_SETFL O_NONBLOCK on the copy");
        goto done;
    }
    if ((fcntl(fd, F_GETFL) & O_NONBLOCK) == 0) {
        errno = 0;
        result = fail(11, "O_NONBLOCK not shared with the original");
        goto done;
    }
    if (!read_eagain(fd))
        result = fail(11, "original not empty or not nonblocking");
done:
    if (copy >= 0) close(copy);
    if (copy3 >= 0) close(copy3);
    close(fd);
    return result;
}

static int check_fork(void)
{
    int wd, status, result = 0;
    int fd = open_watch(12, IN_NONBLOCK, &wd);
    pid_t child;
    if (fd < 0) return 12;
    child = fork();
    if (child < 0) { close(fd); return fail(10, "fork"); }
    if (child == 0) {
        alarm(5);
        _exit(read_create(fd, wd, "fork-a") ? 0 : 4);
    }
    if (make_file(DIR_PATH, "fork-a") != 0) {
        kill(child, SIGKILL);
        waitpid(child, 0, 0);
        close(fd);
        return fail(10, "create");
    }
    status = wait_child(child);
    if (status != 0)
        result = child_failed(12, status, "child didn't read the parent's "
            "event (4: no or wrong event)");
    else if (!read_eagain(fd))
        result = fail(12, "parent's queue not empty after the child read");
    close(fd);
    return result;
}

/* The image exec'd by check 13: fd was inherited with an IN_CREATE watch
   on directory. */
static int exec_image_inherit(int fd, const char *directory)
{
    alarm(5);
    if (make_file(directory, "exec-a") != 0) return 4;
    if (!read_create(fd, -1, "exec-a")) return 5;
    if (inotify_add_watch(fd, directory, IN_CREATE) < 0) return 6;
    if (!read_eagain(fd)) return 7;
    return 0;
}

/* The image exec'd by check 14: fd was IN_CLOEXEC. */
static int exec_image_cloexec(int fd)
{
    errno = 0;
    if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) return 4;
    return 0;
}

static int exec_check(int check, const char *self)
{
    char fd_text[16];
    int status;
    pid_t child = fork();
    if (child < 0) return fail(10, "fork");
    if (child == 0) {
        int wd, fd;
        alarm(5);
        fd = inotify_init1(IN_NONBLOCK | (check == 14 ? IN_CLOEXEC : 0));
        if (fd < 0) _exit(9);
        wd = inotify_add_watch(fd, DIR_PATH, IN_CREATE);
        if (wd < 0) _exit(8);
        snprintf(fd_text, sizeof(fd_text), "%d", fd);
        /* The new image sets its own alarm. */
        alarm(0);
        if (check == 13)
            execl(self, self, "--exec", fd_text, DIR_PATH, (char *)0);
        else
            execl(self, self, "--cloexec", fd_text, (char *)0);
        _exit(3);
    }
    status = wait_child(child);
    if (status != 0)
        return child_failed(check, status, check == 13 ?
            "inotify fd lost across execve (4: create, 5: no CREATE, 6: "
            "add_watch failed, 7: not empty or not nonblocking, 8/9: "
            "setup)" :
            "IN_CLOEXEC fd survived execve (4: F_GETFD didn't fail EBADF)");
    return 0;
}

static void fill_rights(struct msghdr *message, struct iovec *iov,
    char *byte, char *control, size_t control_size, int fd)
{
    struct cmsghdr *header;
    memset(message, 0, sizeof(*message));
    memset(control, 0, control_size);
    iov->iov_base = byte;
    iov->iov_len = 1;
    message->msg_iov = iov;
    message->msg_iovlen = 1;
    message->msg_control = control;
    message->msg_controllen = CMSG_SPACE(sizeof(int));
    header = CMSG_FIRSTHDR(message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &fd, sizeof(int));
}

static int receive_fd(int socket_fd, char *byte)
{
    union { struct cmsghdr align; char bytes[CMSG_SPACE(4 * sizeof(int))]; }
        control;
    struct iovec iov = { byte, 1 };
    struct msghdr message;
    struct cmsghdr *header;
    int fd;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    if (recvmsg(socket_fd, &message, MSG_DONTWAIT) != 1) return -1;
    header = CMSG_FIRSTHDR(&message);
    if (header == 0 || header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len != CMSG_LEN(sizeof(int))) return -1;
    memcpy(&fd, CMSG_DATA(header), sizeof(int));
    return fd;
}

static int check_rights(void)
{
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; }
        control;
    struct msghdr message;
    struct mmsghdr multi;
    struct iovec iov;
    char byte = 'i', text[4] = { 0, 0, 0, 0 };
    int wd, pair[2], pipe_fds[2], received = -1, result = 0;
    int fd = open_watch(15, IN_NONBLOCK, &wd);
    if (fd < 0) return 15;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 ||
            pipe(pipe_fds) != 0) {
        close(fd);
        return fail(10, "socketpair or pipe");
    }
    fill_rights(&message, &iov, &byte, control.bytes, sizeof(control.bytes),
        fd);
    errno = 0;
    if (sendmsg(pair[0], &message, 0) != -1 || errno != EOPNOTSUPP) {
        result = fail(15, "sendmsg of an inotify fd didn't fail EOPNOTSUPP");
        goto done;
    }
    memset(&multi, 0, sizeof(multi));
    multi.msg_hdr = message;
    errno = 0;
    if (sendmmsg(pair[0], &multi, 1, 0) != -1 || errno != EOPNOTSUPP) {
        result = fail(15, "sendmmsg of an inotify fd didn't fail "
            "EOPNOTSUPP");
        goto done;
    }
    if (send(pair[0], "ok", 2, 0) != 2) { result = fail(15, "send ok"); goto done; }
    {
        struct pollfd entry = { pair[1], POLLIN, 0 };
        if (poll(&entry, 1, EVENT_WAIT_MS) != 1 ||
                recv(pair[1], text, sizeof(text), MSG_DONTWAIT) != 2 ||
                memcmp(text, "ok", 2) != 0) {
            fprintf(stderr, "inotify-share: peer got '%.4s'\n", text);
            result = fail(15, "\"ok\" not received intact after the "
                "refused sends");
            goto done;
        }
    }
    byte = 'p';
    fill_rights(&message, &iov, &byte, control.bytes, sizeof(control.bytes),
        pipe_fds[0]);
    if (sendmsg(pair[0], &message, 0) != 1) { result = fail(15, "sendmsg of a pipe fd"); goto done; }
    {
        struct pollfd entry = { pair[1], POLLIN, 0 };
        byte = 0;
        if (poll(&entry, 1, EVENT_WAIT_MS) != 1 ||
                (received = receive_fd(pair[1], &byte)) < 0 || byte != 'p') {
            result = fail(15, "pipe fd not received");
            goto done;
        }
    }
    if (write(pipe_fds[1], "x", 1) != 1 || read(received, text, 1) != 1 ||
            text[0] != 'x')
        result = fail(15, "received pipe fd doesn't read the pipe");
    close(received);
done:
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    close(pair[0]);
    close(pair[1]);
    close(fd);
    return result;
}

static int run_check(int check, const char *self)
{
    switch (check) {
    case 11: return check_dup();
    case 12: return check_fork();
    case 13: return exec_check(13, self);
    case 14: return exec_check(14, self);
    default: return check_rights();
    }
}

int main(int argc, char **argv)
{
    const char *only_text = getenv("INOTIFY_SHARE_ONLY");
    int result = 0, check, only = only_text == 0 ? 0 : atoi(only_text);
    if (argc > 3 && strcmp(argv[1], "--exec") == 0)
        return exec_image_inherit(atoi(argv[2]), argv[3]);
    if (argc > 2 && strcmp(argv[1], "--cloexec") == 0)
        return exec_image_cloexec(atoi(argv[2]));
    signal(SIGPIPE, SIG_IGN);
    remove_tree(DIR_PATH);
    if (mkdir(DIR_PATH, 0755) != 0) return fail(10, "mkdir " DIR_PATH);
    for (check = 11; check <= 15 && result == 0; ++check) {
        if (only != 0 && check != only) continue;
        arm(check);
        result = run_check(check, argv[0]);
    }
    remove_tree(DIR_PATH);
    return result;
}
