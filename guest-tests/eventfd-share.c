/*
 * One eventfd shared across fork, exec and SCM_RIGHTS (guest fixture, musl).
 *
 * Checks (the exit code is the check number, also printed on stderr with
 * errno; a check that hangs is ended by SIGALRM with exit code 100 + the
 * check number, because BerryCore timeout never fires on the device):
 *   10 setup failed
 *   11 after fork, the child writes 4 and exits; the parent reads 4
 *   12 after fork, the child sets O_NONBLOCK with F_SETFL; the parent's
 *      F_GETFL shows it, and a read on the empty eventfd fails EAGAIN
 *   13 (roadmap Phase 1 extra gate) the parent creates an eventfd and a
 *      pipe and forks; the child closes its inherited copies of both and
 *      receives them over an AF_UNIX socketpair with SCM_RIGHTS, into a
 *      control buffer sized for exactly 2 fds; the message carried exactly
 *      2 fds and no MSG_CTRUNC; the child writes 6 to the eventfd and "x"
 *      into the pipe; the parent reads 6 and "x"
 *   14 an eventfd without EFD_CLOEXEC holding 3 survives execve of this
 *      fixture in a forked child; the new image reads 3 and writes 1, which
 *      the original parent then reads
 *   15 an eventfd sent to this same process over a socketpair arrives as
 *      a new number that is the same object (a write through one is read
 *      through the other); the message carried exactly 1 fd (msg_controllen
 *      and cmsg_len as on Linux, with a larger buffer), and no extra fd was
 *      left open by the transfer
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile int current_check;

static int fail(int check, const char *what)
{
    fprintf(stderr, "eventfd-share check %d failed: %s (errno %d %s)\n",
        check, what, errno, strerror(errno));
    return check;
}

static void timed_out(int signal_number)
{
    static const char text[] = "eventfd-share: a check timed out\n";
    (void)signal_number;
    (void)write(2, text, sizeof(text) - 1);
    _exit(100 + current_check);
}

/* Starts the check's 10 s bound; forked children inherit the handler. */
static void arm(int check)
{
    current_check = check;
    alarm(10);
}

static int read_value(int fd, uint64_t *value)
{
    return read(fd, value, sizeof(*value)) == (ssize_t)sizeof(*value);
}

static int write_value(int fd, uint64_t value)
{
    return write(fd, &value, sizeof(value)) == (ssize_t)sizeof(value);
}

/* The child's exit status, or -1. */
static int wait_child(pid_t pid)
{
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 200 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

static int open_count(void)
{
    struct rlimit limit;
    int fd, count = 0, top = 1024;
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur < 4096)
        top = (int)limit.rlim_cur;
    for (fd = 0; fd < top; ++fd) count += fcntl(fd, F_GETFD) >= 0;
    return count;
}

static int send_fds(int socket_fd, const int *fds, int count)
{
    char byte = 'm';
    union { struct cmsghdr align; char bytes[CMSG_SPACE(4 * sizeof(int))]; }
        control;
    struct iovec iov = { &byte, 1 };
    struct msghdr message;
    struct cmsghdr *header;
    memset(&message, 0, sizeof(message));
    memset(&control, 0, sizeof(control));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = CMSG_SPACE(count * sizeof(int));
    header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(count * sizeof(int));
    memcpy(CMSG_DATA(header), fds, count * sizeof(int));
    return sendmsg(socket_fd, &message, 0) == 1 ? 0 : -1;
}

/* Receives exactly `count` fds into fds with a control buffer of
   `capacity` fds.  Returns 0, or a nonzero reason. */
static int receive_fds(int socket_fd, int *fds, int count, int capacity)
{
    char byte;
    union { struct cmsghdr align; char bytes[CMSG_SPACE(16 * sizeof(int))]; }
        control;
    struct iovec iov = { &byte, 1 };
    struct msghdr message;
    struct cmsghdr *header;
    memset(&message, 0, sizeof(message));
    memset(&control, 0, sizeof(control));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = CMSG_SPACE(capacity * sizeof(int));
    if (recvmsg(socket_fd, &message, 0) != 1) return 1;
    if ((message.msg_flags & MSG_CTRUNC) != 0) return 2;
    if (message.msg_controllen != CMSG_SPACE(count * sizeof(int))) {
        fprintf(stderr, "msg_controllen %u, expected %u\n",
            (unsigned)message.msg_controllen,
            (unsigned)CMSG_SPACE(count * sizeof(int)));
        return 3;
    }
    header = CMSG_FIRSTHDR(&message);
    if (header == 0 || header->cmsg_level != SOL_SOCKET ||
            header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len != CMSG_LEN(count * sizeof(int))) return 4;
    if (CMSG_NXTHDR(&message, header) != 0) return 5;
    memcpy(fds, CMSG_DATA(header), count * sizeof(int));
    return 0;
}

static int check_fork_write(void)
{
    uint64_t value = 0;
    pid_t pid;
    int e = eventfd(0, 0), status;
    arm(11);
    if (e < 0) return fail(10, "eventfd(0, 0)");
    pid = fork();
    if (pid < 0) return fail(10, "fork");
    if (pid == 0) _exit(write_value(e, 4) ? 0 : 1);
    if (!read_value(e, &value)) return fail(11, "parent read");
    if (value != 4) { errno = 0; return fail(11, "parent read != 4"); }
    status = wait_child(pid);
    if (status != 0) { errno = 0; return fail(11, "child write failed"); }
    close(e);
    return 0;
}

static int check_fork_status(void)
{
    uint64_t value;
    pid_t pid;
    int e = eventfd(0, 0), flags;
    arm(12);
    if (e < 0) return fail(10, "eventfd(0, 0)");
    pid = fork();
    if (pid < 0) return fail(10, "fork");
    if (pid == 0) {
        flags = fcntl(e, F_GETFL);
        _exit(flags >= 0 && fcntl(e, F_SETFL, flags | O_NONBLOCK) == 0 ?
            0 : 1);
    }
    if (wait_child(pid) != 0) { errno = 0; return fail(12, "child F_SETFL"); }
    flags = fcntl(e, F_GETFL);
    if (flags < 0 || (flags & O_NONBLOCK) == 0)
        return fail(12, "parent F_GETFL lacks O_NONBLOCK");
    errno = 0;
    if (read(e, &value, sizeof(value)) >= 0 || errno != EAGAIN)
        return fail(12, "parent read on empty eventfd not EAGAIN");
    close(e);
    return 0;
}

static int rights_child(int socket_fd, int e, int pipe_read, int pipe_write)
{
    int fds[2], reason;
    close(e);
    close(pipe_read);
    close(pipe_write);
    reason = receive_fds(socket_fd, fds, 2, 2);
    if (reason != 0) return 10 + reason;
    if (!write_value(fds[0], 6)) return 2;
    if (write(fds[1], "x", 1) != 1) return 3;
    return 0;
}

static int check_rights(void)
{
    int e = eventfd(0, 0), pipe_fds[2], pair[2], sent[2], status;
    uint64_t value = 0;
    char text = 0;
    pid_t pid;
    arm(13);
    if (e < 0 || pipe(pipe_fds) != 0 ||
            socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        return fail(10, "eventfd, pipe or socketpair");
    pid = fork();
    if (pid < 0) return fail(10, "fork");
    if (pid == 0) {
        close(pair[0]);
        _exit(rights_child(pair[1], e, pipe_fds[0], pipe_fds[1]));
    }
    close(pair[1]);
    sent[0] = e;
    sent[1] = pipe_fds[1];
    if (send_fds(pair[0], sent, 2) != 0) return fail(13, "sendmsg");
    status = wait_child(pid);
    if (status != 0) {
        fprintf(stderr, "eventfd-share: rights child exit %d\n", status);
        errno = 0;
        return fail(13, "child");
    }
    if (!read_value(e, &value) || value != 6)
        return fail(13, "parent eventfd read != 6");
    if (read(pipe_fds[0], &text, 1) != 1 || text != 'x')
        return fail(13, "parent pipe read != x");
    close(e);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    close(pair[0]);
    return 0;
}

/* The image check 14 execs: argv "exec-child" fd. */
static int exec_child(const char *fd_text)
{
    uint64_t value = 0;
    int e = atoi(fd_text);
    arm(14);
    if (!read_value(e, &value)) return 2;
    if (value != 3) return 3;
    if (!write_value(e, 1)) return 4;
    return 0;
}

static int check_exec(const char *self)
{
    uint64_t value = 0;
    char e_text[16];
    pid_t pid;
    int e = eventfd(3, 0), status;
    arm(14);
    if (e < 0) return fail(10, "eventfd(3, 0)");
    snprintf(e_text, sizeof(e_text), "%d", e);
    pid = fork();
    if (pid < 0) return fail(10, "fork");
    if (pid == 0) {
        execl("/proc/self/exe", self, "exec-child", e_text, (char *)0);
        execl(self, self, "exec-child", e_text, (char *)0);
        _exit(5);
    }
    status = wait_child(pid);
    if (status != 0) {
        fprintf(stderr, "eventfd-share: exec child exit %d\n", status);
        errno = 0;
        return fail(14, "exec child");
    }
    if (!read_value(e, &value) || value != 1)
        return fail(14, "parent read != 1");
    close(e);
    return 0;
}

static int check_self(void)
{
    int e = eventfd(0, EFD_NONBLOCK), pair[2], received, before, after,
        reason;
    uint64_t value = 0;
    arm(15);
    if (e < 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        return fail(10, "eventfd or socketpair");
    before = open_count();
    if (send_fds(pair[0], &e, 1) != 0) return fail(15, "sendmsg");
    reason = receive_fds(pair[1], &received, 1, 8);
    if (reason != 0) {
        fprintf(stderr, "eventfd-share: receive reason %d\n", reason);
        return fail(15, "recvmsg");
    }
    after = open_count();
    if (after != before + 1) {
        fprintf(stderr, "eventfd-share: %d fds open before, %d after\n",
            before, after);
        errno = 0;
        return fail(15, "the transfer leaked fds");
    }
    if (received == e) { errno = 0; return fail(15, "same number"); }
    if (!write_value(received, 5)) return fail(15, "write via received");
    if (!read_value(e, &value) || value != 5)
        return fail(15, "read via original != 5");
    if (!write_value(e, 2)) return fail(15, "write via original");
    if (!read_value(received, &value) || value != 2)
        return fail(15, "read via received != 2");
    close(received);
    close(e);
    close(pair[0]);
    close(pair[1]);
    return 0;
}

int main(int argc, char **argv)
{
    int result;
    signal(SIGALRM, timed_out);
    if (argc == 3 && strcmp(argv[1], "exec-child") == 0)
        return exec_child(argv[2]);
    if ((result = check_fork_write()) != 0) return result;
    if ((result = check_fork_status()) != 0) return result;
    if ((result = check_rights()) != 0) return result;
    if ((result = check_exec(argv[0])) != 0) return result;
    if ((result = check_self()) != 0) return result;
    return 0;
}
